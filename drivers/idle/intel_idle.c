// SPDX-License-Identifier: GPL-2.0-only
/*
 * intel_idle.c - native hardware idle loop for modern Intel processors
 *
 * Copyright (c) 2013 - 2020, Intel Corporation.
 * Len Brown <len.brown@intel.com>
 * Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */

/*
 * intel_idle is a cpuidle driver that loads on all Intel CPUs with MWAIT
 * in lieu of the legacy ACPI processor_idle driver.  The intent is to
 * make Linux more efficient on these processors, as intel_idle knows
 * more than ACPI, as well as make Linux more immune to ACPI BIOS bugs.
 */

/*
 * Design Assumptions
 *
 * All CPUs have same idle states as boot CPU
 *
 * Chipset BM_STS (bus master status) bit is a NOP
 *	for preventing entry into deep C-states
 *
 * CPU will flush caches as needed when entering a C-state via MWAIT
 *	(in contrast to entering ACPI C3, in which case the WBINVD
 *	instruction needs to be executed to flush the caches)
 */

/*
 * Known limitations
 *
 * ACPI has a .suspend hack to turn off deep c-statees during suspend
 * to avoid complications with the lapic timer workaround.
 * Have not seen issues with suspend, but may need same workaround here.
 *
 */

/* un-comment DEBUG to enable pr_debug() statements */
/* #define DEBUG */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/tick.h>
#include <trace/events/power.h>
#include <linux/sched.h>
#include <linux/sched/smt.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/sysfs.h>
#include <asm/cpuid/api.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <asm/mwait.h>
#include <asm/spec-ctrl.h>
#include <asm/msr.h>
#include <asm/tsc.h>
#include <asm/fpu/api.h>
#include <asm/smp.h>

#define INTEL_IDLE_VERSION "0.5.1"

static struct cpuidle_driver intel_idle_driver = {
	.name = "intel_idle",
	.owner = THIS_MODULE,
};
/* intel_idle.max_cstate=0 disables driver */
static int max_cstate = CPUIDLE_STATE_MAX - 1;
static unsigned int disabled_states_mask __read_mostly;
static unsigned int preferred_states_mask __read_mostly;
static bool force_irq_on __read_mostly;
static bool ibrs_off __read_mostly;

static struct cpuidle_device __percpu *intel_idle_cpuidle_devices;

static unsigned long auto_demotion_disable_flags;

static enum {
	C1E_PROMOTION_PRESERVE,
	C1E_PROMOTION_ENABLE,
	C1E_PROMOTION_DISABLE
} c1e_promotion = C1E_PROMOTION_PRESERVE;

struct idle_cpu {
	struct cpuidle_state *state_table;

	/*
	 * Hardware C-state auto-demotion may not always be optimal.
	 * Indicate which enable bits to clear here.
	 */
	unsigned long auto_demotion_disable_flags;
	bool disable_promotion_to_c1e;
	bool c1_demotion_supported;
	bool use_acpi;
};

static bool c1_demotion_supported;
static DEFINE_MUTEX(c1_demotion_mutex);

static struct device *sysfs_root __initdata;

static const struct idle_cpu *icpu __initdata;
static struct cpuidle_state *cpuidle_state_table __initdata;

static unsigned int mwait_substates __initdata;

/*
 * Enable interrupts before entering the C-state. On some platforms and for
 * some C-states, this may measurably decrease interrupt latency.
 */
#define CPUIDLE_FLAG_IRQ_ENABLE		BIT(14)

/*
 * Enable this state by default even if the ACPI _CST does not list it.
 */
#define CPUIDLE_FLAG_ALWAYS_ENABLE	BIT(15)

/*
 * Disable IBRS across idle (when KERNEL_IBRS), is exclusive vs IRQ_ENABLE
 * above.
 */
#define CPUIDLE_FLAG_IBRS		BIT(16)

/*
 * Initialize large xstate for the C6-state entrance.
 */
#define CPUIDLE_FLAG_INIT_XSTATE	BIT(17)

/*
 * Ignore the sub-state when matching mwait hints between the ACPI _CST and
 * custom tables.
 */
#define CPUIDLE_FLAG_PARTIAL_HINT_MATCH	BIT(18)

/*
 * MWAIT takes an 8-bit "hint" in EAX "suggesting"
 * the C-state (top nibble) and sub-state (bottom nibble)
 * 0x00 means "MWAIT(C1)", 0x10 means "MWAIT(C2)" etc.
 *
 * We store the hint at the top of our "flags" for each state.
 */
#define flg2MWAIT(flags) (((flags) >> 24) & 0xFF)
#define MWAIT2flg(eax) ((eax & 0xFF) << 24)

static __always_inline int __intel_idle(struct cpuidle_device *dev,
					struct cpuidle_driver *drv,
					int index, bool irqoff)
{
	struct cpuidle_state *state = &drv->states[index];
	unsigned int eax = flg2MWAIT(state->flags);
	unsigned int ecx = 1*irqoff; /* break on interrupt flag */

	mwait_idle_with_hints(eax, ecx);

	return index;
}

/**
 * intel_idle - Ask the processor to enter the given idle state.
 * @dev: cpuidle device of the target CPU.
 * @drv: cpuidle driver (assumed to point to intel_idle_driver).
 * @index: Target idle state index.
 *
 * Use the MWAIT instruction to notify the processor that the CPU represented by
 * @dev is idle and it can try to enter the idle state corresponding to @index.
 *
 * If the local APIC timer is not known to be reliable in the target idle state,
 * enable one-shot tick broadcasting for the target CPU before executing MWAIT.
 *
 * Must be called under local_irq_disable().
 */
static __cpuidle int intel_idle(struct cpuidle_device *dev,
				struct cpuidle_driver *drv, int index)
{
	return __intel_idle(dev, drv, index, true);
}

static __cpuidle int intel_idle_irq(struct cpuidle_device *dev,
				    struct cpuidle_driver *drv, int index)
{
	return __intel_idle(dev, drv, index, false);
}

static __cpuidle int intel_idle_ibrs(struct cpuidle_device *dev,
				     struct cpuidle_driver *drv, int index)
{
	bool smt_active = sched_smt_active();
	u64 spec_ctrl = spec_ctrl_current();
	int ret;

	if (smt_active)
		__update_spec_ctrl(0);

	ret = __intel_idle(dev, drv, index, true);

	if (smt_active)
		__update_spec_ctrl(spec_ctrl);

	return ret;
}

static __cpuidle int intel_idle_xstate(struct cpuidle_device *dev,
				       struct cpuidle_driver *drv, int index)
{
	fpu_idle_fpregs();
	return __intel_idle(dev, drv, index, true);
}

/**
 * intel_idle_s2idle - Ask the processor to enter the given idle state.
 * @dev: cpuidle device of the target CPU.
 * @drv: cpuidle driver (assumed to point to intel_idle_driver).
 * @index: Target idle state index.
 *
 * Use the MWAIT instruction to notify the processor that the CPU represented by
 * @dev is idle and it can try to enter the idle state corresponding to @index.
 *
 * Invoked as a suspend-to-idle callback routine with frozen user space, frozen
 * scheduler tick and suspended scheduler clock on the target CPU.
 */
static __cpuidle int intel_idle_s2idle(struct cpuidle_device *dev,
				       struct cpuidle_driver *drv, int index)
{
	struct cpuidle_state *state = &drv->states[index];
	unsigned int eax = flg2MWAIT(state->flags);
	unsigned int ecx = 1; /* break on interrupt flag */

	if (state->flags & CPUIDLE_FLAG_INIT_XSTATE)
		fpu_idle_fpregs();

	mwait_idle_with_hints(eax, ecx);

	return 0;
}

static void intel_idle_enter_dead(struct cpuidle_device *dev, int index)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	struct cpuidle_state *state = &drv->states[index];
	unsigned long eax = flg2MWAIT(state->flags);

	mwait_play_dead(eax);
}

/*
 * States are indexed by the cstate number,
 * which is also the index into the MWAIT hint array.
 * Thus C0 is a dummy.
 */
static struct cpuidle_state nehalem_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 3,
		.target_residency = 6,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 20,
		.target_residency = 80,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 200,
		.target_residency = 800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state snb_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 80,
		.target_residency = 211,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 104,
		.target_residency = 345,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7",
		.desc = "MWAIT 0x30",
		.flags = MWAIT2flg(0x30) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 109,
		.target_residency = 345,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state byt_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6N",
		.desc = "MWAIT 0x58",
		.flags = MWAIT2flg(0x58) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 300,
		.target_residency = 275,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6S",
		.desc = "MWAIT 0x52",
		.flags = MWAIT2flg(0x52) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 500,
		.target_residency = 560,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 1200,
		.target_residency = 4000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7S",
		.desc = "MWAIT 0x64",
		.flags = MWAIT2flg(0x64) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 10000,
		.target_residency = 20000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state cht_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6N",
		.desc = "MWAIT 0x58",
		.flags = MWAIT2flg(0x58) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 80,
		.target_residency = 275,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6S",
		.desc = "MWAIT 0x52",
		.flags = MWAIT2flg(0x52) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 200,
		.target_residency = 560,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 1200,
		.target_residency = 4000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7S",
		.desc = "MWAIT 0x64",
		.flags = MWAIT2flg(0x64) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 10000,
		.target_residency = 20000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state ivb_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 59,
		.target_residency = 156,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 80,
		.target_residency = 300,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7",
		.desc = "MWAIT 0x30",
		.flags = MWAIT2flg(0x30) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 87,
		.target_residency = 300,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state ivt_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 80,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 59,
		.target_residency = 156,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 82,
		.target_residency = 300,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state ivt_cstates_4s[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 250,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 59,
		.target_residency = 300,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 84,
		.target_residency = 400,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state ivt_cstates_8s[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 59,
		.target_residency = 600,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 88,
		.target_residency = 700,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state hsw_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 33,
		.target_residency = 100,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 133,
		.target_residency = 400,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7s",
		.desc = "MWAIT 0x32",
		.flags = MWAIT2flg(0x32) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 166,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 300,
		.target_residency = 900,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C9",
		.desc = "MWAIT 0x50",
		.flags = MWAIT2flg(0x50) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 600,
		.target_residency = 1800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 2600,
		.target_residency = 7700,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};
static struct cpuidle_state bdw_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 40,
		.target_residency = 100,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 133,
		.target_residency = 400,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7s",
		.desc = "MWAIT 0x32",
		.flags = MWAIT2flg(0x32) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 166,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 300,
		.target_residency = 900,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C9",
		.desc = "MWAIT 0x50",
		.flags = MWAIT2flg(0x50) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 600,
		.target_residency = 1800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 2600,
		.target_residency = 7700,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state skl_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C3",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 70,
		.target_residency = 100,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 85,
		.target_residency = 200,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7s",
		.desc = "MWAIT 0x33",
		.flags = MWAIT2flg(0x33) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 124,
		.target_residency = 800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 200,
		.target_residency = 800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C9",
		.desc = "MWAIT 0x50",
		.flags = MWAIT2flg(0x50) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 480,
		.target_residency = 5000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 890,
		.target_residency = 5000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state skx_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_IRQ_ENABLE,
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED | CPUIDLE_FLAG_IBRS,
		.exit_latency = 133,
		.target_residency = 600,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state icx_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_IRQ_ENABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 4,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 170,
		.target_residency = 600,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

/*
 * On AlderLake C1 has to be disabled if C1E is enabled, and vice versa.
 * C1E is enabled only if "C1E promotion" bit is set in MSR_IA32_POWER_CTL.
 * But in this case there is effectively no C1, because C1 requests are
 * promoted to C1E. If the "C1E promotion" bit is cleared, then both C1
 * and C1E requests end up with C1, so there is effectively no C1E.
 *
 * By default we enable C1E and disable C1 by marking it with
 * 'CPUIDLE_FLAG_UNUSABLE'.
 */
static struct cpuidle_state adl_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_UNUSABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 220,
		.target_residency = 600,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 280,
		.target_residency = 800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 680,
		.target_residency = 2000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state adl_l_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_UNUSABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 170,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 200,
		.target_residency = 600,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 230,
		.target_residency = 700,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state mtl_l_cstates[] __initdata = {
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 140,
		.target_residency = 420,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 310,
		.target_residency = 930,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state gmt_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_UNUSABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 195,
		.target_residency = 585,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 260,
		.target_residency = 1040,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 660,
		.target_residency = 1980,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state spr_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_INIT_XSTATE,
		.exit_latency = 290,
		.target_residency = 800,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state gnr_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 4,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_INIT_XSTATE |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 170,
		.target_residency = 650,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6P",
		.desc = "MWAIT 0x21",
		.flags = MWAIT2flg(0x21) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_INIT_XSTATE |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 210,
		.target_residency = 1000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state gnrd_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 4,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_INIT_XSTATE |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 220,
		.target_residency = 650,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6P",
		.desc = "MWAIT 0x21",
		.flags = MWAIT2flg(0x21) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_INIT_XSTATE |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 240,
		.target_residency = 750,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state atom_cstates[] __initdata = {
	{
		.name = "C1E",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C2",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10),
		.exit_latency = 20,
		.target_residency = 80,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C4",
		.desc = "MWAIT 0x30",
		.flags = MWAIT2flg(0x30) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 100,
		.target_residency = 400,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x52",
		.flags = MWAIT2flg(0x52) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 140,
		.target_residency = 560,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};
static struct cpuidle_state tangier_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 4,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C4",
		.desc = "MWAIT 0x30",
		.flags = MWAIT2flg(0x30) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 100,
		.target_residency = 400,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x52",
		.flags = MWAIT2flg(0x52) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 140,
		.target_residency = 560,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 1200,
		.target_residency = 4000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C9",
		.desc = "MWAIT 0x64",
		.flags = MWAIT2flg(0x64) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 10000,
		.target_residency = 20000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};
static struct cpuidle_state avn_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x51",
		.flags = MWAIT2flg(0x51) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 15,
		.target_residency = 45,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};
static struct cpuidle_state knl_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 1,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle },
	{
		.name = "C6",
		.desc = "MWAIT 0x10",
		.flags = MWAIT2flg(0x10) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 120,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle },
	{
		.enter = NULL }
};

static struct cpuidle_state bxt_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 133,
		.target_residency = 133,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C7s",
		.desc = "MWAIT 0x31",
		.flags = MWAIT2flg(0x31) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 155,
		.target_residency = 155,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C8",
		.desc = "MWAIT 0x40",
		.flags = MWAIT2flg(0x40) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 1000,
		.target_residency = 1000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C9",
		.desc = "MWAIT 0x50",
		.flags = MWAIT2flg(0x50) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 2000,
		.target_residency = 2000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C10",
		.desc = "MWAIT 0x60",
		.flags = MWAIT2flg(0x60) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 10000,
		.target_residency = 10000,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state dnv_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 10,
		.target_residency = 20,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 50,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

/*
 * Note, depending on HW and FW revision, SnowRidge SoC may or may not support
 * C6, and this is indicated in the CPUID mwait leaf.
 */
static struct cpuidle_state snr_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00),
		.exit_latency = 2,
		.target_residency = 2,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 15,
		.target_residency = 25,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6",
		.desc = "MWAIT 0x20",
		.flags = MWAIT2flg(0x20) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 130,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state grr_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 10,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6S",
		.desc = "MWAIT 0x22",
		.flags = MWAIT2flg(0x22) | CPUIDLE_FLAG_TLB_FLUSHED,
		.exit_latency = 140,
		.target_residency = 500,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static struct cpuidle_state srf_cstates[] __initdata = {
	{
		.name = "C1",
		.desc = "MWAIT 0x00",
		.flags = MWAIT2flg(0x00) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 1,
		.target_residency = 1,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C1E",
		.desc = "MWAIT 0x01",
		.flags = MWAIT2flg(0x01) | CPUIDLE_FLAG_ALWAYS_ENABLE,
		.exit_latency = 2,
		.target_residency = 10,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6S",
		.desc = "MWAIT 0x22",
		.flags = MWAIT2flg(0x22) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 270,
		.target_residency = 700,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.name = "C6SP",
		.desc = "MWAIT 0x23",
		.flags = MWAIT2flg(0x23) | CPUIDLE_FLAG_TLB_FLUSHED |
					   CPUIDLE_FLAG_PARTIAL_HINT_MATCH,
		.exit_latency = 310,
		.target_residency = 900,
		.enter = &intel_idle,
		.enter_s2idle = intel_idle_s2idle, },
	{
		.enter = NULL }
};

static const struct idle_cpu idle_cpu_nehalem __initconst = {
	.state_table = nehalem_cstates,
	.auto_demotion_disable_flags = NHM_C1_AUTO_DEMOTE | NHM_C3_AUTO_DEMOTE,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_nhx __initconst = {
	.state_table = nehalem_cstates,
	.auto_demotion_disable_flags = NHM_C1_AUTO_DEMOTE | NHM_C3_AUTO_DEMOTE,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_atom __initconst = {
	.state_table = atom_cstates,
};

static const struct idle_cpu idle_cpu_tangier __initconst = {
	.state_table = tangier_cstates,
};

static const struct idle_cpu idle_cpu_lincroft __initconst = {
	.state_table = atom_cstates,
	.auto_demotion_disable_flags = ATM_LNC_C6_AUTO_DEMOTE,
};

static const struct idle_cpu idle_cpu_snb __initconst = {
	.state_table = snb_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_snx __initconst = {
	.state_table = snb_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_byt __initconst = {
	.state_table = byt_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_cht __initconst = {
	.state_table = cht_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_ivb __initconst = {
	.state_table = ivb_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_ivt __initconst = {
	.state_table = ivt_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_hsw __initconst = {
	.state_table = hsw_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_hsx __initconst = {
	.state_table = hsw_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_bdw __initconst = {
	.state_table = bdw_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_bdx __initconst = {
	.state_table = bdw_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_skl __initconst = {
	.state_table = skl_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_skx __initconst = {
	.state_table = skx_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_icx __initconst = {
	.state_table = icx_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_adl __initconst = {
	.state_table = adl_cstates,
};

static const struct idle_cpu idle_cpu_adl_l __initconst = {
	.state_table = adl_l_cstates,
};

static const struct idle_cpu idle_cpu_mtl_l __initconst = {
	.state_table = mtl_l_cstates,
};

static const struct idle_cpu idle_cpu_gmt __initconst = {
	.state_table = gmt_cstates,
};

static const struct idle_cpu idle_cpu_spr __initconst = {
	.state_table = spr_cstates,
	.disable_promotion_to_c1e = true,
	.c1_demotion_supported = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_gnr __initconst = {
	.state_table = gnr_cstates,
	.disable_promotion_to_c1e = true,
	.c1_demotion_supported = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_gnrd __initconst = {
	.state_table = gnrd_cstates,
	.disable_promotion_to_c1e = true,
	.c1_demotion_supported = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_avn __initconst = {
	.state_table = avn_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_knl __initconst = {
	.state_table = knl_cstates,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_bxt __initconst = {
	.state_table = bxt_cstates,
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_dnv __initconst = {
	.state_table = dnv_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_tmt __initconst = {
	.disable_promotion_to_c1e = true,
};

static const struct idle_cpu idle_cpu_snr __initconst = {
	.state_table = snr_cstates,
	.disable_promotion_to_c1e = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_grr __initconst = {
	.state_table = grr_cstates,
	.disable_promotion_to_c1e = true,
	.c1_demotion_supported = true,
	.use_acpi = true,
};

static const struct idle_cpu idle_cpu_srf __initconst = {
	.state_table = srf_cstates,
	.disable_promotion_to_c1e = true,
	.c1_demotion_supported = true,
	.use_acpi = true,
};

static const struct x86_cpu_id intel_idle_ids[] __initconst = {
	X86_MATCH_VFM(INTEL_NEHALEM_EP,		&idle_cpu_nhx),
	X86_MATCH_VFM(INTEL_NEHALEM,		&idle_cpu_nehalem),
	X86_MATCH_VFM(INTEL_NEHALEM_G,		&idle_cpu_nehalem),
	X86_MATCH_VFM(INTEL_WESTMERE,		&idle_cpu_nehalem),
	X86_MATCH_VFM(INTEL_WESTMERE_EP,	&idle_cpu_nhx),
	X86_MATCH_VFM(INTEL_NEHALEM_EX,		&idle_cpu_nhx),
	X86_MATCH_VFM(INTEL_ATOM_BONNELL,	&idle_cpu_atom),
	X86_MATCH_VFM(INTEL_ATOM_BONNELL_MID,	&idle_cpu_lincroft),
	X86_MATCH_VFM(INTEL_WESTMERE_EX,	&idle_cpu_nhx),
	X86_MATCH_VFM(INTEL_SANDYBRIDGE,	&idle_cpu_snb),
	X86_MATCH_VFM(INTEL_SANDYBRIDGE_X,	&idle_cpu_snx),
	X86_MATCH_VFM(INTEL_ATOM_SALTWELL,	&idle_cpu_atom),
	X86_MATCH_VFM(INTEL_ATOM_SILVERMONT,	&idle_cpu_byt),
	X86_MATCH_VFM(INTEL_ATOM_SILVERMONT_MID, &idle_cpu_tangier),
	X86_MATCH_VFM(INTEL_ATOM_AIRMONT,	&idle_cpu_cht),
	X86_MATCH_VFM(INTEL_IVYBRIDGE,		&idle_cpu_ivb),
	X86_MATCH_VFM(INTEL_IVYBRIDGE_X,	&idle_cpu_ivt),
	X86_MATCH_VFM(INTEL_HASWELL,		&idle_cpu_hsw),
	X86_MATCH_VFM(INTEL_HASWELL_X,		&idle_cpu_hsx),
	X86_MATCH_VFM(INTEL_HASWELL_L,		&idle_cpu_hsw),
	X86_MATCH_VFM(INTEL_HASWELL_G,		&idle_cpu_hsw),
	X86_MATCH_VFM(INTEL_ATOM_SILVERMONT_D,	&idle_cpu_avn),
	X86_MATCH_VFM(INTEL_BROADWELL,		&idle_cpu_bdw),
	X86_MATCH_VFM(INTEL_BROADWELL_G,	&idle_cpu_bdw),
	X86_MATCH_VFM(INTEL_BROADWELL_X,	&idle_cpu_bdx),
	X86_MATCH_VFM(INTEL_BROADWELL_D,	&idle_cpu_bdx),
	X86_MATCH_VFM(INTEL_SKYLAKE_L,		&idle_cpu_skl),
	X86_MATCH_VFM(INTEL_SKYLAKE,		&idle_cpu_skl),
	X86_MATCH_VFM(INTEL_KABYLAKE_L,		&idle_cpu_skl),
	X86_MATCH_VFM(INTEL_KABYLAKE,		&idle_cpu_skl),
	X86_MATCH_VFM(INTEL_SKYLAKE_X,		&idle_cpu_skx),
	X86_MATCH_VFM(INTEL_ICELAKE_X,		&idle_cpu_icx),
	X86_MATCH_VFM(INTEL_ICELAKE_D,		&idle_cpu_icx),
	X86_MATCH_VFM(INTEL_ALDERLAKE,		&idle_cpu_adl),
	X86_MATCH_VFM(INTEL_ALDERLAKE_L,	&idle_cpu_adl_l),
	X86_MATCH_VFM(INTEL_METEORLAKE_L,	&idle_cpu_mtl_l),
	X86_MATCH_VFM(INTEL_ATOM_GRACEMONT,	&idle_cpu_gmt),
	X86_MATCH_VFM(INTEL_SAPPHIRERAPIDS_X,	&idle_cpu_spr),
	X86_MATCH_VFM(INTEL_EMERALDRAPIDS_X,	&idle_cpu_spr),
	X86_MATCH_VFM(INTEL_GRANITERAPIDS_X,	&idle_cpu_gnr),
	X86_MATCH_VFM(INTEL_GRANITERAPIDS_D,	&idle_cpu_gnrd),
	X86_MATCH_VFM(INTEL_XEON_PHI_KNL,	&idle_cpu_knl),
	X86_MATCH_VFM(INTEL_XEON_PHI_KNM,	&idle_cpu_knl),
	X86_MATCH_VFM(INTEL_ATOM_GOLDMONT,	&idle_cpu_bxt),
	X86_MATCH_VFM(INTEL_ATOM_GOLDMONT_PLUS,	&idle_cpu_bxt),
	X86_MATCH_VFM(INTEL_ATOM_GOLDMONT_D,	&idle_cpu_dnv),
	X86_MATCH_VFM(INTEL_ATOM_TREMONT,       &idle_cpu_tmt),
	X86_MATCH_VFM(INTEL_ATOM_TREMONT_L,     &idle_cpu_tmt),
	X86_MATCH_VFM(INTEL_ATOM_TREMONT_D,	&idle_cpu_snr),
	X86_MATCH_VFM(INTEL_ATOM_CRESTMONT,	&idle_cpu_grr),
	X86_MATCH_VFM(INTEL_ATOM_CRESTMONT_X,	&idle_cpu_srf),
	X86_MATCH_VFM(INTEL_ATOM_DARKMONT_X,	&idle_cpu_srf),
	{}
};

static const struct x86_cpu_id intel_mwait_ids[] __initconst = {
	X86_MATCH_VENDOR_FAM_FEATURE(INTEL, 6, X86_FEATURE_MWAIT, NULL),
	{}
};

static bool __init intel_idle_max_cstate_reached(int cstate)
{
	if (cstate + 1 > max_cstate) {
		pr_info("max_cstate %d reached\n", max_cstate);
		return true;
	}
	return false;
}

static bool __init intel_idle_state_needs_timer_stop(struct cpuidle_state *state)
{
	unsigned long eax = flg2MWAIT(state->flags);

	if (boot_cpu_has(X86_FEATURE_ARAT))
		return false;

	/*
	 * Switch over to one-shot tick broadcast if the target C-state
	 * is deeper than C1.
	 */
	return !!((eax >> MWAIT_SUBSTATE_SIZE) & MWAIT_CSTATE_MASK);
}

#ifdef CONFIG_ACPI_PROCESSOR_CSTATE
#include <acpi/processor.h>

static bool no_acpi __read_mostly;
module_param(no_acpi, bool, 0444);
MODULE_PARM_DESC(no_acpi, "Do not use ACPI _CST for building the idle states list");

static bool force_use_acpi __read_mostly; /* No effect if no_acpi is set. */
module_param_named(use_acpi, force_use_acpi, bool, 0444);
MODULE_PARM_DESC(use_acpi, "Use ACPI _CST for building the idle states list");

static bool no_native __read_mostly; /* No effect if no_acpi is set. */
module_param_named(no_native, no_native, bool, 0444);
MODULE_PARM_DESC(no_native, "Ignore cpu specific (native) idle states in lieu of ACPI idle states");

static struct acpi_processor_power acpi_state_table __initdata;

/**
 * intel_idle_cst_usable - Check if the _CST information can be used.
 *
 * Check if all of the C-states listed by _CST in the max_cstate range are
 * ACPI_CSTATE_FFH, which means that they should be entered via MWAIT.
 */
static bool __init intel_idle_cst_usable(void)
{
	int cstate, limit;

	limit = min_t(int, min_t(int, CPUIDLE_STATE_MAX, max_cstate + 1),
		      acpi_state_table.count);

	for (cstate = 1; cstate < limit; cstate++) {
		struct acpi_processor_cx *cx = &acpi_state_table.states[cstate];

		if (cx->entry_method != ACPI_CSTATE_FFH)
			return false;
	}

	return true;
}

static bool __init intel_idle_acpi_cst_extract(void)
{
	unsigned int cpu;

	if (no_acpi) {
		pr_debug("Not allowed to use ACPI _CST\n");
		return false;
	}

	for_each_possible_cpu(cpu) {
		struct acpi_processor *pr = per_cpu(processors, cpu);

		if (!pr)
			continue;

		if (acpi_processor_evaluate_cst(pr->handle, cpu, &acpi_state_table))
			continue;

		acpi_state_table.count++;

		if (!intel_idle_cst_usable())
			continue;

		if (!acpi_processor_claim_cst_control())
			break;

		return true;
	}

	acpi_state_table.count = 0;
	pr_debug("ACPI _CST not found or not usable\n");
	return false;
}

static void __init intel_idle_init_cstates_acpi(struct cpuidle_driver *drv)
{
	int cstate, limit = min_t(int, CPUIDLE_STATE_MAX, acpi_state_table.count);

	/*
	 * If limit > 0, intel_idle_cst_usable() has returned 'true', so all of
	 * the interesting states are ACPI_CSTATE_FFH.
	 */
	for (cstate = 1; cstate < limit; cstate++) {
		struct acpi_processor_cx *cx;
		struct cpuidle_state *state;

		if (intel_idle_max_cstate_reached(cstate - 1))
			break;

		cx = &acpi_state_table.states[cstate];

		state = &drv->states[drv->state_count++];

		snprintf(state->name, CPUIDLE_NAME_LEN, "C%d_ACPI", cstate);
		strscpy(state->desc, cx->desc, CPUIDLE_DESC_LEN);
		state->exit_latency = cx->latency;
		/*
		 * For C1-type C-states use the same number for both the exit
		 * latency and target residency, because that is the case for
		 * C1 in the majority of the static C-states tables above.
		 * For the other types of C-states, however, set the target
		 * residency to 3 times the exit latency which should lead to
		 * a reasonable balance between energy-efficiency and
		 * performance in the majority of interesting cases.
		 */
		state->target_residency = cx->latency;
		if (cx->type > ACPI_STATE_C1)
			state->target_residency *= 3;

		state->flags = MWAIT2flg(cx->address);
		if (cx->type > ACPI_STATE_C2)
			state->flags |= CPUIDLE_FLAG_TLB_FLUSHED;

		if (disabled_states_mask & BIT(cstate))
			state->flags |= CPUIDLE_FLAG_OFF;

		if (intel_idle_state_needs_timer_stop(state))
			state->flags |= CPUIDLE_FLAG_TIMER_STOP;

		if (cx->type > ACPI_STATE_C1 && !boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
			mark_tsc_unstable("TSC halts in idle");

		state->enter = intel_idle;
		state->enter_dead = intel_idle_enter_dead;
		state->enter_s2idle = intel_idle_s2idle;
	}
}

static bool __init intel_idle_off_by_default(unsigned int flags, u32 mwait_hint)
{
	int cstate, limit;

	/*
	 * If there are no _CST C-states, do not disable any C-states by
	 * default.
	 */
	if (!acpi_state_table.count)
		return false;

	limit = min_t(int, CPUIDLE_STATE_MAX, acpi_state_table.count);
	/*
	 * If limit > 0, intel_idle_cst_usable() has returned 'true', so all of
	 * the interesting states are ACPI_CSTATE_FFH.
	 */
	for (cstate = 1; cstate < limit; cstate++) {
		u32 acpi_hint = acpi_state_table.states[cstate].address;
		u32 table_hint = mwait_hint;

		if (flags & CPUIDLE_FLAG_PARTIAL_HINT_MATCH) {
			acpi_hint &= ~MWAIT_SUBSTATE_MASK;
			table_hint &= ~MWAIT_SUBSTATE_MASK;
		}

		if (acpi_hint == table_hint)
			return false;
	}
	return true;
}

static inline bool ignore_native(void)
{
	return no_native && !no_acpi;
}
#else /* !CONFIG_ACPI_PROCESSOR_CSTATE */
#define force_use_acpi	(false)

static inline bool intel_idle_acpi_cst_extract(void) { return false; }
static inline void intel_idle_init_cstates_acpi(struct cpuidle_driver *drv) { }
static inline bool intel_idle_off_by_default(unsigned int flags, u32 mwait_hint)
{
	return false;
}
static inline bool ignore_native(void) { return false; }
#endif /* !CONFIG_ACPI_PROCESSOR_CSTATE */

/**
 * ivt_idle_state_table_update - Tune the idle states table for Ivy Town.
 *
 * Tune IVT multi-socket targets.
 * Assumption: num_sockets == (max_package_num + 1).
 */
static void __init ivt_idle_state_table_update(void)
{
	/* IVT uses a different table for 1-2, 3-4, and > 4 sockets */
	int cpu, package_num, num_sockets = 1;

	for_each_online_cpu(cpu) {
		package_num = topology_physical_package_id(cpu);
		if (package_num + 1 > num_sockets) {
			num_sockets = package_num + 1;

			if (num_sockets > 4) {
				cpuidle_state_table = ivt_cstates_8s;
				return;
			}
		}
	}

	if (num_sockets > 2)
		cpuidle_state_table = ivt_cstates_4s;

	/* else, 1 and 2 socket systems use default ivt_cstates */
}

/**
 * irtl_2_usec - IRTL to microseconds conversion.
 * @irtl: IRTL MSR value.
 *
 * Translate the IRTL (Interrupt Response Time Limit) MSR value to microseconds.
 */
static unsigned long long __init irtl_2_usec(unsigned long long irtl)
{
	static const unsigned int irtl_ns_units[] __initconst = {
		1, 32, 1024, 32768, 1048576, 33554432, 0, 0
	};
	unsigned long long ns;

	if (!irtl)
		return 0;

	ns = irtl_ns_units[(irtl >> 10) & 0x7];

	return div_u64((irtl & 0x3FF) * ns, NSEC_PER_USEC);
}

/**
 * bxt_idle_state_table_update - Fix up the Broxton idle states table.
 *
 * On BXT, trust the IRTL (Interrupt Response Time Limit) MSR to show the
 * definitive maximum latency and use the same value for target_residency.
 */
static void __init bxt_idle_state_table_update(void)
{
	unsigned long long msr;
	unsigned int usec;

	rdmsrq(MSR_PKGC6_IRTL, msr);
	usec = irtl_2_usec(msr);
	if (usec) {
		bxt_cstates[2].exit_latency = usec;
		bxt_cstates[2].target_residency = usec;
	}

	rdmsrq(MSR_PKGC7_IRTL, msr);
	usec = irtl_2_usec(msr);
	if (usec) {
		bxt_cstates[3].exit_latency = usec;
		bxt_cstates[3].target_residency = usec;
	}

	rdmsrq(MSR_PKGC8_IRTL, msr);
	usec = irtl_2_usec(msr);
	if (usec) {
		bxt_cstates[4].exit_latency = usec;
		bxt_cstates[4].target_residency = usec;
	}

	rdmsrq(MSR_PKGC9_IRTL, msr);
	usec = irtl_2_usec(msr);
	if (usec) {
		bxt_cstates[5].exit_latency = usec;
		bxt_cstates[5].target_residency = usec;
	}

	rdmsrq(MSR_PKGC10_IRTL, msr);
	usec = irtl_2_usec(msr);
	if (usec) {
		bxt_cstates[6].exit_latency = usec;
		bxt_cstates[6].target_residency = usec;
	}

}

/**
 * sklh_idle_state_table_update - Fix up the Sky Lake idle states table.
 *
 * On SKL-H (model 0x5e) skip C8 and C9 if C10 is enabled and SGX disabled.
 */
static void __init sklh_idle_state_table_update(void)
{
	unsigned long long msr;
	unsigned int eax, ebx, ecx, edx;


	/* if PC10 disabled via cmdline intel_idle.max_cstate=7 or shallower */
	if (max_cstate <= 7)
		return;

	/* if PC10 not present in CPUID.MWAIT.EDX */
	if ((mwait_substates & (0xF << 28)) == 0)
		return;

	rdmsrq(MSR_PKG_CST_CONFIG_CONTROL, msr);

	/* PC10 is not enabled in PKG C-state limit */
	if ((msr & 0xF) != 8)
		return;

	ecx = 0;
	cpuid(7, &eax, &ebx, &ecx, &edx);

	/* if SGX is present */
	if (ebx & (1 << 2)) {

		rdmsrq(MSR_IA32_FEAT_CTL, msr);

		/* if SGX is enabled */
		if (msr & (1 << 18))
			return;
	}

	skl_cstates[5].flags |= CPUIDLE_FLAG_UNUSABLE;	/* C8-SKL */
	skl_cstates[6].flags |= CPUIDLE_FLAG_UNUSABLE;	/* C9-SKL */
}

/**
 * skx_idle_state_table_update - Adjust the Sky Lake/Cascade Lake
 * idle states table.
 */
static void __init skx_idle_state_table_update(void)
{
	unsigned long long msr;

	rdmsrq(MSR_PKG_CST_CONFIG_CONTROL, msr);

	/*
	 * 000b: C0/C1 (no package C-state support)
	 * 001b: C2
	 * 010b: C6 (non-retention)
	 * 011b: C6 (retention)
	 * 111b: No Package C state limits.
	 */
	if ((msr & 0x7) < 2) {
		/*
		 * Uses the CC6 + PC0 latency and 3 times of
		 * latency for target_residency if the PC6
		 * is disabled in BIOS. This is consistent
		 * with how intel_idle driver uses _CST
		 * to set the target_residency.
		 */
		skx_cstates[2].exit_latency = 92;
		skx_cstates[2].target_residency = 276;
	}
}

/**
 * adl_idle_state_table_update - Adjust AlderLake idle states table.
 */
static void __init adl_idle_state_table_update(void)
{
	/* Check if user prefers C1 over C1E. */
	if (preferred_states_mask & BIT(1) && !(preferred_states_mask & BIT(2))) {
		cpuidle_state_table[0].flags &= ~CPUIDLE_FLAG_UNUSABLE;
		cpuidle_state_table[1].flags |= CPUIDLE_FLAG_UNUSABLE;

		/* Disable C1E by clearing the "C1E promotion" bit. */
		c1e_promotion = C1E_PROMOTION_DISABLE;
		return;
	}

	/* Make sure C1E is enabled by default */
	c1e_promotion = C1E_PROMOTION_ENABLE;
}

/**
 * spr_idle_state_table_update - Adjust Sapphire Rapids idle states table.
 */
static void __init spr_idle_state_table_update(void)
{
	unsigned long long msr;

	/*
	 * By default, the C6 state assumes the worst-case scenario of package
	 * C6. However, if PC6 is disabled, we update the numbers to match
	 * core C6.
	 */
	rdmsrq(MSR_PKG_CST_CONFIG_CONTROL, msr);

	/* Limit value 2 and above allow for PC6. */
	if ((msr & 0x7) < 2) {
		spr_cstates[2].exit_latency = 190;
		spr_cstates[2].target_residency = 600;
	}
}

/**
 * byt_cht_auto_demotion_disable - Disable Bay/Cherry Trail auto-demotion.
 */
static void __init byt_cht_auto_demotion_disable(void)
{
	wrmsrq(MSR_CC6_DEMOTION_POLICY_CONFIG, 0);
	wrmsrq(MSR_MC6_DEMOTION_POLICY_CONFIG, 0);
}

static bool __init intel_idle_verify_cstate(unsigned int mwait_hint)
{
	unsigned int mwait_cstate = (MWAIT_HINT2CSTATE(mwait_hint) + 1) &
					MWAIT_CSTATE_MASK;
	unsigned int num_substates = (mwait_substates >> mwait_cstate * 4) &
					MWAIT_SUBSTATE_MASK;

	/* Ignore the C-state if there are NO sub-states in CPUID for it. */
	if (num_substates == 0)
		return false;

	if (mwait_cstate > 2 && !boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
		mark_tsc_unstable("TSC halts in idle states deeper than C2");

	return true;
}

static void state_update_enter_method(struct cpuidle_state *state, int cstate)
{
	if (state->flags & CPUIDLE_FLAG_INIT_XSTATE) {
		/*
		 * Combining with XSTATE with IBRS or IRQ_ENABLE flags
		 * is not currently supported but this driver.
		 */
		WARN_ON_ONCE(state->flags & CPUIDLE_FLAG_IBRS);
		WARN_ON_ONCE(state->flags & CPUIDLE_FLAG_IRQ_ENABLE);
		state->enter = intel_idle_xstate;
		return;
	}

	if (cpu_feature_enabled(X86_FEATURE_KERNEL_IBRS) &&
			((state->flags & CPUIDLE_FLAG_IBRS) || ibrs_off)) {
		/*
		 * IBRS mitigation requires that C-states are entered
		 * with interrupts disabled.
		 */
		if (ibrs_off && (state->flags & CPUIDLE_FLAG_IRQ_ENABLE))
			state->flags &= ~CPUIDLE_FLAG_IRQ_ENABLE;
		WARN_ON_ONCE(state->flags & CPUIDLE_FLAG_IRQ_ENABLE);
		state->enter = intel_idle_ibrs;
		return;
	}

	if (state->flags & CPUIDLE_FLAG_IRQ_ENABLE) {
		state->enter = intel_idle_irq;
		return;
	}

	if (force_irq_on) {
		pr_info("forced intel_idle_irq for state %d\n", cstate);
		state->enter = intel_idle_irq;
	}
}

static void __init intel_idle_init_cstates_icpu(struct cpuidle_driver *drv)
{
	int cstate;

	switch (boot_cpu_data.x86_vfm) {
	case INTEL_IVYBRIDGE_X:
		ivt_idle_state_table_update();
		break;
	case INTEL_ATOM_GOLDMONT:
	case INTEL_ATOM_GOLDMONT_PLUS:
		bxt_idle_state_table_update();
		break;
	case INTEL_SKYLAKE:
		sklh_idle_state_table_update();
		break;
	case INTEL_SKYLAKE_X:
		skx_idle_state_table_update();
		break;
	case INTEL_SAPPHIRERAPIDS_X:
	case INTEL_EMERALDRAPIDS_X:
		spr_idle_state_table_update();
		break;
	case INTEL_ALDERLAKE:
	case INTEL_ALDERLAKE_L:
	case INTEL_ATOM_GRACEMONT:
		adl_idle_state_table_update();
		break;
	case INTEL_ATOM_SILVERMONT:
	case INTEL_ATOM_AIRMONT:
		byt_cht_auto_demotion_disable();
		break;
	}

	for (cstate = 0; cstate < CPUIDLE_STATE_MAX; ++cstate) {
		struct cpuidle_state *state;
		unsigned int mwait_hint;

		if (intel_idle_max_cstate_reached(cstate))
			break;

		if (!cpuidle_state_table[cstate].enter &&
		    !cpuidle_state_table[cstate].enter_s2idle)
			break;

		if (!cpuidle_state_table[cstate].enter_dead)
			cpuidle_state_table[cstate].enter_dead = intel_idle_enter_dead;

		/* If marked as unusable, skip this state. */
		if (cpuidle_state_table[cstate].flags & CPUIDLE_FLAG_UNUSABLE) {
			pr_debug("state %s is disabled\n",
				 cpuidle_state_table[cstate].name);
			continue;
		}

		mwait_hint = flg2MWAIT(cpuidle_state_table[cstate].flags);
		if (!intel_idle_verify_cstate(mwait_hint))
			continue;

		/* Structure copy. */
		drv->states[drv->state_count] = cpuidle_state_table[cstate];
		state = &drv->states[drv->state_count];

		state_update_enter_method(state, cstate);


		if ((disabled_states_mask & BIT(drv->state_count)) ||
		    ((icpu->use_acpi || force_use_acpi) &&
		     intel_idle_off_by_default(state->flags, mwait_hint) &&
		     !(state->flags & CPUIDLE_FLAG_ALWAYS_ENABLE)))
			state->flags |= CPUIDLE_FLAG_OFF;

		if (intel_idle_state_needs_timer_stop(state))
			state->flags |= CPUIDLE_FLAG_TIMER_STOP;

		drv->state_count++;
	}
}

/**
 * intel_idle_cpuidle_driver_init - Create the list of available idle states.
 * @drv: cpuidle driver structure to initialize.
 */
static void __init intel_idle_cpuidle_driver_init(struct cpuidle_driver *drv)
{
	cpuidle_poll_state_init(drv);

	if (disabled_states_mask & BIT(0))
		drv->states[0].flags |= CPUIDLE_FLAG_OFF;

	drv->state_count = 1;

	if (icpu && icpu->state_table)
		intel_idle_init_cstates_icpu(drv);
	else
		intel_idle_init_cstates_acpi(drv);
}

static void auto_demotion_disable(void)
{
	unsigned long long msr_bits;

	rdmsrq(MSR_PKG_CST_CONFIG_CONTROL, msr_bits);
	msr_bits &= ~auto_demotion_disable_flags;
	wrmsrq(MSR_PKG_CST_CONFIG_CONTROL, msr_bits);
}

static void c1e_promotion_enable(void)
{
	unsigned long long msr_bits;

	rdmsrq(MSR_IA32_POWER_CTL, msr_bits);
	msr_bits |= 0x2;
	wrmsrq(MSR_IA32_POWER_CTL, msr_bits);
}

static void c1e_promotion_disable(void)
{
	unsigned long long msr_bits;

	rdmsrq(MSR_IA32_POWER_CTL, msr_bits);
	msr_bits &= ~0x2;
	wrmsrq(MSR_IA32_POWER_CTL, msr_bits);
}

/**
 * intel_idle_cpu_init - Register the target CPU with the cpuidle core.
 * @cpu: CPU to initialize.
 *
 * Register a cpuidle device object for @cpu and update its MSRs in accordance
 * with the processor model flags.
 */
static int intel_idle_cpu_init(unsigned int cpu)
{
	struct cpuidle_device *dev;

	dev = per_cpu_ptr(intel_idle_cpuidle_devices, cpu);
	dev->cpu = cpu;

	if (cpuidle_register_device(dev)) {
		pr_debug("cpuidle_register_device %d failed!\n", cpu);
		return -EIO;
	}

	if (auto_demotion_disable_flags)
		auto_demotion_disable();

	if (c1e_promotion == C1E_PROMOTION_ENABLE)
		c1e_promotion_enable();
	else if (c1e_promotion == C1E_PROMOTION_DISABLE)
		c1e_promotion_disable();

	return 0;
}

static int intel_idle_cpu_online(unsigned int cpu)
{
	struct cpuidle_device *dev;

	if (!boot_cpu_has(X86_FEATURE_ARAT))
		tick_broadcast_enable();

	/*
	 * Some systems can hotplug a cpu at runtime after
	 * the kernel has booted, we have to initialize the
	 * driver in this case
	 */
	dev = per_cpu_ptr(intel_idle_cpuidle_devices, cpu);
	if (!dev->registered)
		return intel_idle_cpu_init(cpu);

	return 0;
}

/**
 * intel_idle_cpuidle_devices_uninit - Unregister all cpuidle devices.
 */
static void __init intel_idle_cpuidle_devices_uninit(void)
{
	int i;

	for_each_online_cpu(i)
		cpuidle_unregister_device(per_cpu_ptr(intel_idle_cpuidle_devices, i));
}

static void intel_c1_demotion_toggle(void *enable)
{
	unsigned long long msr_val;

	rdmsrl(MSR_PKG_CST_CONFIG_CONTROL, msr_val);
	/*
	 * Enable/disable C1 undemotion along with C1 demotion, as this is the
	 * most sensible configuration in general.
	 */
	if (enable)
		msr_val |= NHM_C1_AUTO_DEMOTE | SNB_C1_AUTO_UNDEMOTE;
	else
		msr_val &= ~(NHM_C1_AUTO_DEMOTE | SNB_C1_AUTO_UNDEMOTE);
	wrmsrl(MSR_PKG_CST_CONFIG_CONTROL, msr_val);
}

static ssize_t intel_c1_demotion_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&c1_demotion_mutex);
	/* Enable/disable C1 demotion on all CPUs */
	on_each_cpu(intel_c1_demotion_toggle, (void *)enable, 1);
	mutex_unlock(&c1_demotion_mutex);

	return count;
}

static ssize_t intel_c1_demotion_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	unsigned long long msr_val;

	/*
	 * Read the MSR value for a CPU and assume it is the same for all CPUs. Any other
	 * configuration would be a BIOS bug.
	 */
	rdmsrl(MSR_PKG_CST_CONFIG_CONTROL, msr_val);
	return sysfs_emit(buf, "%d\n", !!(msr_val & NHM_C1_AUTO_DEMOTE));
}
static DEVICE_ATTR_RW(intel_c1_demotion);

static int __init intel_idle_sysfs_init(void)
{
	int err;

	if (!c1_demotion_supported)
		return 0;

	sysfs_root = bus_get_dev_root(&cpu_subsys);
	if (!sysfs_root)
		return 0;

	err = sysfs_add_file_to_group(&sysfs_root->kobj,
				      &dev_attr_intel_c1_demotion.attr,
				      "cpuidle");
	if (err) {
		put_device(sysfs_root);
		return err;
	}

	return 0;
}

static void __init intel_idle_sysfs_uninit(void)
{
	if (!sysfs_root)
		return;

	sysfs_remove_file_from_group(&sysfs_root->kobj,
				     &dev_attr_intel_c1_demotion.attr,
				     "cpuidle");
	put_device(sysfs_root);
}

static int __init intel_idle_init(void)
{
	const struct x86_cpu_id *id;
	unsigned int eax, ebx, ecx;
	int retval;

	/* Do not load intel_idle at all for now if idle= is passed */
	if (boot_option_idle_override != IDLE_NO_OVERRIDE)
		return -ENODEV;

	if (max_cstate == 0) {
		pr_debug("disabled\n");
		return -EPERM;
	}

	id = x86_match_cpu(intel_idle_ids);
	if (id) {
		if (!boot_cpu_has(X86_FEATURE_MWAIT)) {
			pr_debug("Please enable MWAIT in BIOS SETUP\n");
			return -ENODEV;
		}
	} else {
		id = x86_match_cpu(intel_mwait_ids);
		if (!id)
			return -ENODEV;
	}

	cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &mwait_substates);

	if (!(ecx & CPUID5_ECX_EXTENSIONS_SUPPORTED) ||
	    !(ecx & CPUID5_ECX_INTERRUPT_BREAK) ||
	    !mwait_substates)
			return -ENODEV;

	pr_debug("MWAIT substates: 0x%x\n", mwait_substates);

	icpu = (const struct idle_cpu *)id->driver_data;
	if (icpu && ignore_native()) {
		pr_debug("ignoring native CPU idle states\n");
		icpu = NULL;
	}
	if (icpu) {
		if (icpu->state_table)
			cpuidle_state_table = icpu->state_table;
		else if (!intel_idle_acpi_cst_extract())
			return -ENODEV;

		auto_demotion_disable_flags = icpu->auto_demotion_disable_flags;
		if (icpu->disable_promotion_to_c1e)
			c1e_promotion = C1E_PROMOTION_DISABLE;
		if (icpu->c1_demotion_supported)
			c1_demotion_supported = true;
		if (icpu->use_acpi || force_use_acpi)
			intel_idle_acpi_cst_extract();
	} else if (!intel_idle_acpi_cst_extract()) {
		return -ENODEV;
	}

	pr_debug("v" INTEL_IDLE_VERSION " model 0x%X\n",
		 boot_cpu_data.x86_model);

	intel_idle_cpuidle_devices = alloc_percpu(struct cpuidle_device);
	if (!intel_idle_cpuidle_devices)
		return -ENOMEM;

	retval = intel_idle_sysfs_init();
	if (retval)
		pr_warn("failed to initialized sysfs");

	intel_idle_cpuidle_driver_init(&intel_idle_driver);

	retval = cpuidle_register_driver(&intel_idle_driver);
	if (retval) {
		struct cpuidle_driver *drv = cpuidle_get_driver();
		printk(KERN_DEBUG pr_fmt("intel_idle yielding to %s\n"),
		       drv ? drv->name : "none");
		goto init_driver_fail;
	}

	retval = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "idle/intel:online",
				   intel_idle_cpu_online, NULL);
	if (retval < 0)
		goto hp_setup_fail;

	pr_debug("Local APIC timer is reliable in %s\n",
		 boot_cpu_has(X86_FEATURE_ARAT) ? "all C-states" : "C1");

	arch_cpu_rescan_dead_smt_siblings();

	return 0;

hp_setup_fail:
	intel_idle_cpuidle_devices_uninit();
	cpuidle_unregister_driver(&intel_idle_driver);
init_driver_fail:
	intel_idle_sysfs_uninit();
	free_percpu(intel_idle_cpuidle_devices);
	return retval;

}
subsys_initcall_sync(intel_idle_init);

/*
 * We are not really modular, but we used to support that.  Meaning we also
 * support "intel_idle.max_cstate=..." at boot and also a read-only export of
 * it at /sys/module/intel_idle/parameters/max_cstate -- so using module_param
 * is the easiest way (currently) to continue doing that.
 */
module_param(max_cstate, int, 0444);
/*
 * The positions of the bits that are set in this number are the indices of the
 * idle states to be disabled by default (as reflected by the names of the
 * corresponding idle state directories in sysfs, "state0", "state1" ...
 * "state<i>" ..., where <i> is the index of the given state).
 */
module_param_named(states_off, disabled_states_mask, uint, 0444);
MODULE_PARM_DESC(states_off, "Mask of disabled idle states");
/*
 * Some platforms come with mutually exclusive C-states, so that if one is
 * enabled, the other C-states must not be used. Example: C1 and C1E on
 * Sapphire Rapids platform. This parameter allows for selecting the
 * preferred C-states among the groups of mutually exclusive C-states - the
 * selected C-states will be registered, the other C-states from the mutually
 * exclusive group won't be registered. If the platform has no mutually
 * exclusive C-states, this parameter has no effect.
 */
module_param_named(preferred_cstates, preferred_states_mask, uint, 0444);
MODULE_PARM_DESC(preferred_cstates, "Mask of preferred idle states");
/*
 * Debugging option that forces the driver to enter all C-states with
 * interrupts enabled. Does not apply to C-states with
 * 'CPUIDLE_FLAG_INIT_XSTATE' and 'CPUIDLE_FLAG_IBRS' flags.
 */
module_param(force_irq_on, bool, 0444);
/*
 * Force the disabling of IBRS when X86_FEATURE_KERNEL_IBRS is on and
 * CPUIDLE_FLAG_IRQ_ENABLE isn't set.
 */
module_param(ibrs_off, bool, 0444);
MODULE_PARM_DESC(ibrs_off, "Disable IBRS when idle");

// SPDX-License-Identifier: GPL-2.0
/*
 * EFI initialization
 *
 * Author: Jianmin Lv <lvjianmin@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */

#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/efi-bgrt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/memblock.h>
#include <linux/reboot.h>
#include <linux/screen_info.h>
#include <linux/uaccess.h>

#include <asm/early_ioremap.h>
#include <asm/efi.h>
#include <asm/loongson.h>

static unsigned long efi_nr_tables;
static unsigned long efi_config_table;

static unsigned long __initdata boot_memmap = EFI_INVALID_TABLE_ADDR;
static unsigned long __initdata fdt_pointer = EFI_INVALID_TABLE_ADDR;

static efi_system_table_t *efi_systab;
static efi_config_table_type_t arch_tables[] __initdata = {
	{LINUX_EFI_BOOT_MEMMAP_GUID,	&boot_memmap,	"MEMMAP" },
	{DEVICE_TREE_GUID,		&fdt_pointer,	"FDTPTR" },
	{},
};

void __init *efi_fdt_pointer(void)
{
	if (!efi_systab)
		return NULL;

	if (fdt_pointer == EFI_INVALID_TABLE_ADDR)
		return NULL;

	return early_memremap_ro(fdt_pointer, SZ_64K);
}

void __init efi_runtime_init(void)
{
	if (!efi_enabled(EFI_BOOT) || !efi_systab->runtime)
		return;

	if (efi_runtime_disabled()) {
		pr_info("EFI runtime services will be disabled.\n");
		return;
	}

	efi.runtime = (efi_runtime_services_t *)efi_systab->runtime;
	efi.runtime_version = (unsigned int)efi.runtime->hdr.revision;

	efi_native_runtime_setup();
	set_bit(EFI_RUNTIME_SERVICES, &efi.flags);
}

bool efi_poweroff_required(void)
{
	return efi_enabled(EFI_RUNTIME_SERVICES) &&
		(acpi_gbl_reduced_hardware || acpi_no_s5);
}

unsigned long __initdata screen_info_table = EFI_INVALID_TABLE_ADDR;

#if defined(CONFIG_SYSFB) || defined(CONFIG_EFI_EARLYCON)
struct screen_info screen_info __section(".data");
EXPORT_SYMBOL_GPL(screen_info);
#endif

static void __init init_screen_info(void)
{
	struct screen_info *si;

	if (screen_info_table == EFI_INVALID_TABLE_ADDR)
		return;

	si = early_memremap(screen_info_table, sizeof(*si));
	if (!si) {
		pr_err("Could not map screen_info config table\n");
		return;
	}
	screen_info = *si;
	memset(si, 0, sizeof(*si));
	early_memunmap(si, sizeof(*si));

	memblock_reserve(__screen_info_lfb_base(&screen_info), screen_info.lfb_size);
}

void __init efi_init(void)
{
	int size;
	void *config_tables;
	struct efi_boot_memmap *tbl;

	if (!efi_system_table)
		return;

	efi_systab = (efi_system_table_t *)early_memremap_ro(efi_system_table, sizeof(*efi_systab));
	if (!efi_systab) {
		pr_err("Can't find EFI system table.\n");
		return;
	}

	efi_systab_report_header(&efi_systab->hdr, efi_systab->fw_vendor);

	set_bit(EFI_64BIT, &efi.flags);
	efi_nr_tables	 = efi_systab->nr_tables;
	efi_config_table = (unsigned long)efi_systab->tables;

	size = sizeof(efi_config_table_t);
	config_tables = early_memremap(efi_config_table, efi_nr_tables * size);
	efi_config_parse_tables(config_tables, efi_systab->nr_tables, arch_tables);
	early_memunmap(config_tables, efi_nr_tables * size);

	set_bit(EFI_CONFIG_TABLES, &efi.flags);

	if (IS_ENABLED(CONFIG_EFI_EARLYCON) || IS_ENABLED(CONFIG_SYSFB))
		init_screen_info();

	if (boot_memmap == EFI_INVALID_TABLE_ADDR)
		return;

	tbl = early_memremap_ro(boot_memmap, sizeof(*tbl));
	if (tbl) {
		struct efi_memory_map_data data;

		data.phys_map		= boot_memmap + sizeof(*tbl);
		data.size		= tbl->map_size;
		data.desc_size		= tbl->desc_size;
		data.desc_version	= tbl->desc_ver;

		if (efi_memmap_init_early(&data) < 0)
			panic("Unable to map EFI memory map.\n");

		/*
		 * Reserve the physical memory region occupied by the EFI
		 * memory map table (header + descriptors). This is crucial
		 * for kdump, as the kdump kernel relies on this original
		 * memmap passed by the bootloader. Without reservation,
		 * this region could be overwritten by the primary kernel.
		 * Also, set the EFI_PRESERVE_BS_REGIONS flag to indicate that
		 * critical boot services code/data regions like this are preserved.
		 */
		memblock_reserve((phys_addr_t)boot_memmap, sizeof(*tbl) + data.size);
		set_bit(EFI_PRESERVE_BS_REGIONS, &efi.flags);

		early_memunmap(tbl, sizeof(*tbl));
	}

	efi_esrt_init();
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include "trace.h"

static DEFINE_IDA(mhi_controller_ida);

#undef mhi_ee
#undef mhi_ee_end

#define mhi_ee(a, b)		[MHI_EE_##a] = b,
#define mhi_ee_end(a, b)	[MHI_EE_##a] = b,

const char * const mhi_ee_str[MHI_EE_MAX] = {
	MHI_EE_LIST
};

#undef dev_st_trans
#undef dev_st_trans_end

#define dev_st_trans(a, b)	[DEV_ST_TRANSITION_##a] = b,
#define dev_st_trans_end(a, b)	[DEV_ST_TRANSITION_##a] = b,

const char * const dev_state_tran_str[DEV_ST_TRANSITION_MAX] = {
	DEV_ST_TRANSITION_LIST
};

#undef ch_state_type
#undef ch_state_type_end

#define ch_state_type(a, b)	[MHI_CH_STATE_TYPE_##a] = b,
#define ch_state_type_end(a, b)	[MHI_CH_STATE_TYPE_##a] = b,

const char * const mhi_ch_state_type_str[MHI_CH_STATE_TYPE_MAX] = {
	MHI_CH_STATE_TYPE_LIST
};

#undef mhi_pm_state
#undef mhi_pm_state_end

#define mhi_pm_state(a, b)	[MHI_PM_STATE_##a] = b,
#define mhi_pm_state_end(a, b)	[MHI_PM_STATE_##a] = b,

static const char * const mhi_pm_state_str[] = {
	MHI_PM_STATE_LIST
};

const char *to_mhi_pm_state_str(u32 state)
{
	int index;

	if (state)
		index = __fls(state);

	if (!state || index >= ARRAY_SIZE(mhi_pm_state_str))
		return "Invalid State";

	return mhi_pm_state_str[index];
}

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;

	return sysfs_emit(buf, "Serial Number: %u\n",
			mhi_cntrl->serial_number);
}
static DEVICE_ATTR_RO(serial_number);

static ssize_t oem_pk_hash_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	u32 hash_segment[MHI_MAX_OEM_PK_HASH_SEGMENTS];
	int i, cnt = 0, ret;

	for (i = 0; i < MHI_MAX_OEM_PK_HASH_SEGMENTS; i++) {
		ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_OEMPKHASH(i), &hash_segment[i]);
		if (ret) {
			dev_err(dev, "Could not capture OEM PK HASH\n");
			return ret;
		}
	}

	for (i = 0; i < MHI_MAX_OEM_PK_HASH_SEGMENTS; i++)
		cnt += sysfs_emit_at(buf, cnt, "OEMPKHASH[%d]: 0x%x\n", i, hash_segment[i]);

	return cnt;
}
static DEVICE_ATTR_RO(oem_pk_hash);

static ssize_t soc_reset_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;

	mhi_soc_reset(mhi_cntrl);
	return count;
}
static DEVICE_ATTR_WO(soc_reset);

static ssize_t trigger_edl_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (!val)
		return -EINVAL;

	ret = mhi_cntrl->edl_trigger(mhi_cntrl);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(trigger_edl);

static struct attribute *mhi_dev_attrs[] = {
	&dev_attr_serial_number.attr,
	&dev_attr_oem_pk_hash.attr,
	&dev_attr_soc_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mhi_dev);

/* MHI protocol requires the transfer ring to be aligned with ring length */
static int mhi_alloc_aligned_ring(struct mhi_controller *mhi_cntrl,
				  struct mhi_ring *ring,
				  u64 len)
{
	ring->alloc_size = len + (len - 1);
	ring->pre_aligned = dma_alloc_coherent(mhi_cntrl->cntrl_dev, ring->alloc_size,
					       &ring->dma_handle, GFP_KERNEL);
	if (!ring->pre_aligned)
		return -ENOMEM;

	ring->iommu_base = (ring->dma_handle + (len - 1)) & ~(len - 1);
	ring->base = ring->pre_aligned + (ring->iommu_base - ring->dma_handle);

	return 0;
}

static void mhi_deinit_free_irq(struct mhi_controller *mhi_cntrl)
{
	int i;
	struct mhi_event *mhi_event = mhi_cntrl->mhi_event;

	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		free_irq(mhi_cntrl->irq[mhi_event->irq], mhi_event);
	}

	free_irq(mhi_cntrl->irq[0], mhi_cntrl);
}

static int mhi_init_irq_setup(struct mhi_controller *mhi_cntrl)
{
	struct mhi_event *mhi_event = mhi_cntrl->mhi_event;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	unsigned long irq_flags = IRQF_SHARED | IRQF_NO_SUSPEND;
	int i, ret;

	/* if controller driver has set irq_flags, use it */
	if (mhi_cntrl->irq_flags)
		irq_flags = mhi_cntrl->irq_flags;

	/* Setup BHI_INTVEC IRQ */
	ret = request_threaded_irq(mhi_cntrl->irq[0], mhi_intvec_handler,
				   mhi_intvec_threaded_handler,
				   irq_flags,
				   "bhi", mhi_cntrl);
	if (ret)
		return ret;
	/*
	 * IRQs should be enabled during mhi_async_power_up(), so disable them explicitly here.
	 * Due to the use of IRQF_SHARED flag as default while requesting IRQs, we assume that
	 * IRQ_NOAUTOEN is not applicable.
	 */
	disable_irq(mhi_cntrl->irq[0]);

	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		if (mhi_event->irq >= mhi_cntrl->nr_irqs) {
			dev_err(dev, "irq %d not available for event ring\n",
				mhi_event->irq);
			ret = -EINVAL;
			goto error_request;
		}

		ret = request_irq(mhi_cntrl->irq[mhi_event->irq],
				  mhi_irq_handler,
				  irq_flags,
				  "mhi", mhi_event);
		if (ret) {
			dev_err(dev, "Error requesting irq:%d for ev:%d\n",
				mhi_cntrl->irq[mhi_event->irq], i);
			goto error_request;
		}

		disable_irq(mhi_cntrl->irq[mhi_event->irq]);
	}

	return 0;

error_request:
	for (--i, --mhi_event; i >= 0; i--, mhi_event--) {
		if (mhi_event->offload_ev)
			continue;

		free_irq(mhi_cntrl->irq[mhi_event->irq], mhi_event);
	}
	free_irq(mhi_cntrl->irq[0], mhi_cntrl);

	return ret;
}

static void mhi_deinit_dev_ctxt(struct mhi_controller *mhi_cntrl)
{
	int i;
	struct mhi_ctxt *mhi_ctxt = mhi_cntrl->mhi_ctxt;
	struct mhi_cmd *mhi_cmd;
	struct mhi_event *mhi_event;
	struct mhi_ring *ring;

	mhi_cmd = mhi_cntrl->mhi_cmd;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++) {
		ring = &mhi_cmd->ring;
		dma_free_coherent(mhi_cntrl->cntrl_dev, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
		ring->base = NULL;
		ring->iommu_base = 0;
	}

	dma_free_coherent(mhi_cntrl->cntrl_dev,
			  sizeof(*mhi_ctxt->cmd_ctxt) * NR_OF_CMD_RINGS,
			  mhi_ctxt->cmd_ctxt, mhi_ctxt->cmd_ctxt_addr);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		ring = &mhi_event->ring;
		dma_free_coherent(mhi_cntrl->cntrl_dev, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
		ring->base = NULL;
		ring->iommu_base = 0;
	}

	dma_free_coherent(mhi_cntrl->cntrl_dev, sizeof(*mhi_ctxt->er_ctxt) *
			  mhi_cntrl->total_ev_rings, mhi_ctxt->er_ctxt,
			  mhi_ctxt->er_ctxt_addr);

	dma_free_coherent(mhi_cntrl->cntrl_dev, sizeof(*mhi_ctxt->chan_ctxt) *
			  mhi_cntrl->max_chan, mhi_ctxt->chan_ctxt,
			  mhi_ctxt->chan_ctxt_addr);

	kfree(mhi_ctxt);
	mhi_cntrl->mhi_ctxt = NULL;
}

static int mhi_init_dev_ctxt(struct mhi_controller *mhi_cntrl)
{
	struct mhi_ctxt *mhi_ctxt;
	struct mhi_chan_ctxt *chan_ctxt;
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_cmd_ctxt *cmd_ctxt;
	struct mhi_chan *mhi_chan;
	struct mhi_event *mhi_event;
	struct mhi_cmd *mhi_cmd;
	u32 tmp;
	int ret = -ENOMEM, i;

	atomic_set(&mhi_cntrl->dev_wake, 0);
	atomic_set(&mhi_cntrl->pending_pkts, 0);

	mhi_ctxt = kzalloc(sizeof(*mhi_ctxt), GFP_KERNEL);
	if (!mhi_ctxt)
		return -ENOMEM;

	/* Setup channel ctxt */
	mhi_ctxt->chan_ctxt = dma_alloc_coherent(mhi_cntrl->cntrl_dev,
						 sizeof(*mhi_ctxt->chan_ctxt) *
						 mhi_cntrl->max_chan,
						 &mhi_ctxt->chan_ctxt_addr,
						 GFP_KERNEL);
	if (!mhi_ctxt->chan_ctxt)
		goto error_alloc_chan_ctxt;

	mhi_chan = mhi_cntrl->mhi_chan;
	chan_ctxt = mhi_ctxt->chan_ctxt;
	for (i = 0; i < mhi_cntrl->max_chan; i++, chan_ctxt++, mhi_chan++) {
		/* Skip if it is an offload channel */
		if (mhi_chan->offload_ch)
			continue;

		tmp = le32_to_cpu(chan_ctxt->chcfg);
		tmp &= ~CHAN_CTX_CHSTATE_MASK;
		tmp |= FIELD_PREP(CHAN_CTX_CHSTATE_MASK, MHI_CH_STATE_DISABLED);
		tmp &= ~CHAN_CTX_BRSTMODE_MASK;
		tmp |= FIELD_PREP(CHAN_CTX_BRSTMODE_MASK, mhi_chan->db_cfg.brstmode);
		tmp &= ~CHAN_CTX_POLLCFG_MASK;
		tmp |= FIELD_PREP(CHAN_CTX_POLLCFG_MASK, mhi_chan->db_cfg.pollcfg);
		chan_ctxt->chcfg = cpu_to_le32(tmp);

		chan_ctxt->chtype = cpu_to_le32(mhi_chan->type);
		chan_ctxt->erindex = cpu_to_le32(mhi_chan->er_index);

		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
		mhi_chan->tre_ring.db_addr = (void __iomem *)&chan_ctxt->wp;
	}

	/* Setup event context */
	mhi_ctxt->er_ctxt = dma_alloc_coherent(mhi_cntrl->cntrl_dev,
					       sizeof(*mhi_ctxt->er_ctxt) *
					       mhi_cntrl->total_ev_rings,
					       &mhi_ctxt->er_ctxt_addr,
					       GFP_KERNEL);
	if (!mhi_ctxt->er_ctxt)
		goto error_alloc_er_ctxt;

	er_ctxt = mhi_ctxt->er_ctxt;
	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, er_ctxt++,
		     mhi_event++) {
		struct mhi_ring *ring = &mhi_event->ring;

		/* Skip if it is an offload event */
		if (mhi_event->offload_ev)
			continue;

		tmp = le32_to_cpu(er_ctxt->intmod);
		tmp &= ~EV_CTX_INTMODC_MASK;
		tmp &= ~EV_CTX_INTMODT_MASK;
		tmp |= FIELD_PREP(EV_CTX_INTMODT_MASK, mhi_event->intmod);
		er_ctxt->intmod = cpu_to_le32(tmp);

		er_ctxt->ertype = cpu_to_le32(MHI_ER_TYPE_VALID);
		er_ctxt->msivec = cpu_to_le32(mhi_event->irq);
		mhi_event->db_cfg.db_mode = true;

		ring->el_size = sizeof(struct mhi_ring_element);
		ring->len = ring->el_size * ring->elements;
		ret = mhi_alloc_aligned_ring(mhi_cntrl, ring, ring->len);
		if (ret)
			goto error_alloc_er;

		/*
		 * If the read pointer equals to the write pointer, then the
		 * ring is empty
		 */
		ring->rp = ring->wp = ring->base;
		er_ctxt->rbase = cpu_to_le64(ring->iommu_base);
		er_ctxt->rp = er_ctxt->wp = er_ctxt->rbase;
		er_ctxt->rlen = cpu_to_le64(ring->len);
		ring->ctxt_wp = &er_ctxt->wp;
	}

	/* Setup cmd context */
	ret = -ENOMEM;
	mhi_ctxt->cmd_ctxt = dma_alloc_coherent(mhi_cntrl->cntrl_dev,
						sizeof(*mhi_ctxt->cmd_ctxt) *
						NR_OF_CMD_RINGS,
						&mhi_ctxt->cmd_ctxt_addr,
						GFP_KERNEL);
	if (!mhi_ctxt->cmd_ctxt)
		goto error_alloc_er;

	mhi_cmd = mhi_cntrl->mhi_cmd;
	cmd_ctxt = mhi_ctxt->cmd_ctxt;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++, cmd_ctxt++) {
		struct mhi_ring *ring = &mhi_cmd->ring;

		ring->el_size = sizeof(struct mhi_ring_element);
		ring->elements = CMD_EL_PER_RING;
		ring->len = ring->el_size * ring->elements;
		ret = mhi_alloc_aligned_ring(mhi_cntrl, ring, ring->len);
		if (ret)
			goto error_alloc_cmd;

		ring->rp = ring->wp = ring->base;
		cmd_ctxt->rbase = cpu_to_le64(ring->iommu_base);
		cmd_ctxt->rp = cmd_ctxt->wp = cmd_ctxt->rbase;
		cmd_ctxt->rlen = cpu_to_le64(ring->len);
		ring->ctxt_wp = &cmd_ctxt->wp;
	}

	mhi_cntrl->mhi_ctxt = mhi_ctxt;

	return 0;

error_alloc_cmd:
	for (--i, --mhi_cmd; i >= 0; i--, mhi_cmd--) {
		struct mhi_ring *ring = &mhi_cmd->ring;

		dma_free_coherent(mhi_cntrl->cntrl_dev, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
	}
	dma_free_coherent(mhi_cntrl->cntrl_dev,
			  sizeof(*mhi_ctxt->cmd_ctxt) * NR_OF_CMD_RINGS,
			  mhi_ctxt->cmd_ctxt, mhi_ctxt->cmd_ctxt_addr);
	i = mhi_cntrl->total_ev_rings;
	mhi_event = mhi_cntrl->mhi_event + i;

error_alloc_er:
	for (--i, --mhi_event; i >= 0; i--, mhi_event--) {
		struct mhi_ring *ring = &mhi_event->ring;

		if (mhi_event->offload_ev)
			continue;

		dma_free_coherent(mhi_cntrl->cntrl_dev, ring->alloc_size,
				  ring->pre_aligned, ring->dma_handle);
	}
	dma_free_coherent(mhi_cntrl->cntrl_dev, sizeof(*mhi_ctxt->er_ctxt) *
			  mhi_cntrl->total_ev_rings, mhi_ctxt->er_ctxt,
			  mhi_ctxt->er_ctxt_addr);

error_alloc_er_ctxt:
	dma_free_coherent(mhi_cntrl->cntrl_dev, sizeof(*mhi_ctxt->chan_ctxt) *
			  mhi_cntrl->max_chan, mhi_ctxt->chan_ctxt,
			  mhi_ctxt->chan_ctxt_addr);

error_alloc_chan_ctxt:
	kfree(mhi_ctxt);

	return ret;
}

int mhi_init_mmio(struct mhi_controller *mhi_cntrl)
{
	u32 val;
	int i, ret;
	struct mhi_chan *mhi_chan;
	struct mhi_event *mhi_event;
	void __iomem *base = mhi_cntrl->regs;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct {
		u32 offset;
		u32 val;
	} reg_info[] = {
		{
			CCABAP_HIGHER,
			upper_32_bits(mhi_cntrl->mhi_ctxt->chan_ctxt_addr),
		},
		{
			CCABAP_LOWER,
			lower_32_bits(mhi_cntrl->mhi_ctxt->chan_ctxt_addr),
		},
		{
			ECABAP_HIGHER,
			upper_32_bits(mhi_cntrl->mhi_ctxt->er_ctxt_addr),
		},
		{
			ECABAP_LOWER,
			lower_32_bits(mhi_cntrl->mhi_ctxt->er_ctxt_addr),
		},
		{
			CRCBAP_HIGHER,
			upper_32_bits(mhi_cntrl->mhi_ctxt->cmd_ctxt_addr),
		},
		{
			CRCBAP_LOWER,
			lower_32_bits(mhi_cntrl->mhi_ctxt->cmd_ctxt_addr),
		},
		{
			MHICTRLBASE_HIGHER,
			upper_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHICTRLBASE_LOWER,
			lower_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHIDATABASE_HIGHER,
			upper_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHIDATABASE_LOWER,
			lower_32_bits(mhi_cntrl->iova_start),
		},
		{
			MHICTRLLIMIT_HIGHER,
			upper_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHICTRLLIMIT_LOWER,
			lower_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHIDATALIMIT_HIGHER,
			upper_32_bits(mhi_cntrl->iova_stop),
		},
		{
			MHIDATALIMIT_LOWER,
			lower_32_bits(mhi_cntrl->iova_stop),
		},
		{0, 0}
	};

	dev_dbg(dev, "Initializing MHI registers\n");

	/* Read channel db offset */
	ret = mhi_get_channel_doorbell_offset(mhi_cntrl, &val);
	if (ret)
		return ret;

	if (val >= mhi_cntrl->reg_len - (8 * MHI_DEV_WAKE_DB)) {
		dev_err(dev, "CHDB offset: 0x%x is out of range: 0x%zx\n",
			val, mhi_cntrl->reg_len - (8 * MHI_DEV_WAKE_DB));
		return -ERANGE;
	}

	/* Setup wake db */
	mhi_cntrl->wake_db = base + val + (8 * MHI_DEV_WAKE_DB);
	mhi_cntrl->wake_set = false;

	/* Setup channel db address for each channel in tre_ring */
	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, val += 8, mhi_chan++)
		mhi_chan->tre_ring.db_addr = base + val;

	/* Read event ring db offset */
	ret = mhi_read_reg(mhi_cntrl, base, ERDBOFF, &val);
	if (ret) {
		dev_err(dev, "Unable to read ERDBOFF register\n");
		return -EIO;
	}

	if (val >= mhi_cntrl->reg_len - (8 * mhi_cntrl->total_ev_rings)) {
		dev_err(dev, "ERDB offset: 0x%x is out of range: 0x%zx\n",
			val, mhi_cntrl->reg_len - (8 * mhi_cntrl->total_ev_rings));
		return -ERANGE;
	}

	/* Setup event db address for each ev_ring */
	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, val += 8, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		mhi_event->ring.db_addr = base + val;
	}

	/* Setup DB register for primary CMD rings */
	mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING].ring.db_addr = base + CRDB_LOWER;

	/* Write to MMIO registers */
	for (i = 0; reg_info[i].offset; i++)
		mhi_write_reg(mhi_cntrl, base, reg_info[i].offset,
			      reg_info[i].val);

	ret = mhi_write_reg_field(mhi_cntrl, base, MHICFG, MHICFG_NER_MASK,
				  mhi_cntrl->total_ev_rings);
	if (ret) {
		dev_err(dev, "Unable to write MHICFG register\n");
		return ret;
	}

	ret = mhi_write_reg_field(mhi_cntrl, base, MHICFG, MHICFG_NHWER_MASK,
				  mhi_cntrl->hw_ev_rings);
	if (ret) {
		dev_err(dev, "Unable to write MHICFG register\n");
		return ret;
	}

	return 0;
}

void mhi_deinit_chan_ctxt(struct mhi_controller *mhi_cntrl,
			  struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring;
	struct mhi_ring *tre_ring;
	struct mhi_chan_ctxt *chan_ctxt;
	u32 tmp;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];

	if (!chan_ctxt->rbase) /* Already uninitialized */
		return;

	dma_free_coherent(mhi_cntrl->cntrl_dev, tre_ring->alloc_size,
			  tre_ring->pre_aligned, tre_ring->dma_handle);
	vfree(buf_ring->base);

	buf_ring->base = tre_ring->base = NULL;
	tre_ring->ctxt_wp = NULL;
	chan_ctxt->rbase = 0;
	chan_ctxt->rlen = 0;
	chan_ctxt->rp = 0;
	chan_ctxt->wp = 0;

	tmp = le32_to_cpu(chan_ctxt->chcfg);
	tmp &= ~CHAN_CTX_CHSTATE_MASK;
	tmp |= FIELD_PREP(CHAN_CTX_CHSTATE_MASK, MHI_CH_STATE_DISABLED);
	chan_ctxt->chcfg = cpu_to_le32(tmp);

	/* Update to all cores */
	smp_wmb();
}

int mhi_init_chan_ctxt(struct mhi_controller *mhi_cntrl,
		       struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring;
	struct mhi_ring *tre_ring;
	struct mhi_chan_ctxt *chan_ctxt;
	u32 tmp;
	int ret;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	tre_ring->el_size = sizeof(struct mhi_ring_element);
	tre_ring->len = tre_ring->el_size * tre_ring->elements;
	chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
	ret = mhi_alloc_aligned_ring(mhi_cntrl, tre_ring, tre_ring->len);
	if (ret)
		return -ENOMEM;

	buf_ring->el_size = sizeof(struct mhi_buf_info);
	buf_ring->len = buf_ring->el_size * buf_ring->elements;
	buf_ring->base = vzalloc(buf_ring->len);

	if (!buf_ring->base) {
		dma_free_coherent(mhi_cntrl->cntrl_dev, tre_ring->alloc_size,
				  tre_ring->pre_aligned, tre_ring->dma_handle);
		return -ENOMEM;
	}

	tmp = le32_to_cpu(chan_ctxt->chcfg);
	tmp &= ~CHAN_CTX_CHSTATE_MASK;
	tmp |= FIELD_PREP(CHAN_CTX_CHSTATE_MASK, MHI_CH_STATE_ENABLED);
	chan_ctxt->chcfg = cpu_to_le32(tmp);

	chan_ctxt->rbase = cpu_to_le64(tre_ring->iommu_base);
	chan_ctxt->rp = chan_ctxt->wp = chan_ctxt->rbase;
	chan_ctxt->rlen = cpu_to_le64(tre_ring->len);
	tre_ring->ctxt_wp = &chan_ctxt->wp;

	tre_ring->rp = tre_ring->wp = tre_ring->base;
	buf_ring->rp = buf_ring->wp = buf_ring->base;
	mhi_chan->db_cfg.db_mode = 1;

	/* Update to all cores */
	smp_wmb();

	return 0;
}

static int parse_ev_cfg(struct mhi_controller *mhi_cntrl,
			const struct mhi_controller_config *config)
{
	struct mhi_event *mhi_event;
	const struct mhi_event_config *event_cfg;
	struct device *dev = mhi_cntrl->cntrl_dev;
	int i, num;

	num = config->num_events;
	mhi_cntrl->total_ev_rings = num;
	mhi_cntrl->mhi_event = kcalloc(num, sizeof(*mhi_cntrl->mhi_event),
				       GFP_KERNEL);
	if (!mhi_cntrl->mhi_event)
		return -ENOMEM;

	/* Populate event ring */
	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < num; i++) {
		event_cfg = &config->event_cfg[i];

		mhi_event->er_index = i;
		mhi_event->ring.elements = event_cfg->num_elements;
		mhi_event->intmod = event_cfg->irq_moderation_ms;
		mhi_event->irq = event_cfg->irq;

		if (event_cfg->channel != U32_MAX) {
			/* This event ring has a dedicated channel */
			mhi_event->chan = event_cfg->channel;
			if (mhi_event->chan >= mhi_cntrl->max_chan) {
				dev_err(dev,
					"Event Ring channel not available\n");
				goto error_ev_cfg;
			}

			mhi_event->mhi_chan =
				&mhi_cntrl->mhi_chan[mhi_event->chan];
		}

		/* Priority is fixed to 1 for now */
		mhi_event->priority = 1;

		mhi_event->db_cfg.brstmode = event_cfg->mode;
		if (MHI_INVALID_BRSTMODE(mhi_event->db_cfg.brstmode))
			goto error_ev_cfg;

		if (mhi_event->db_cfg.brstmode == MHI_DB_BRST_ENABLE)
			mhi_event->db_cfg.process_db = mhi_db_brstmode;
		else
			mhi_event->db_cfg.process_db = mhi_db_brstmode_disable;

		mhi_event->data_type = event_cfg->data_type;

		switch (mhi_event->data_type) {
		case MHI_ER_DATA:
			mhi_event->process_event = mhi_process_data_event_ring;
			break;
		case MHI_ER_CTRL:
			mhi_event->process_event = mhi_process_ctrl_ev_ring;
			break;
		default:
			dev_err(dev, "Event Ring type not supported\n");
			goto error_ev_cfg;
		}

		mhi_event->hw_ring = event_cfg->hardware_event;
		if (mhi_event->hw_ring)
			mhi_cntrl->hw_ev_rings++;
		else
			mhi_cntrl->sw_ev_rings++;

		mhi_event->cl_manage = event_cfg->client_managed;
		mhi_event->offload_ev = event_cfg->offload_channel;
		mhi_event++;
	}

	return 0;

error_ev_cfg:

	kfree(mhi_cntrl->mhi_event);
	return -EINVAL;
}

static int parse_ch_cfg(struct mhi_controller *mhi_cntrl,
			const struct mhi_controller_config *config)
{
	const struct mhi_channel_config *ch_cfg;
	struct device *dev = mhi_cntrl->cntrl_dev;
	int i;
	u32 chan;

	mhi_cntrl->max_chan = config->max_channels;

	/*
	 * The allocation of MHI channels can exceed 32KB in some scenarios,
	 * so to avoid any memory possible allocation failures, vzalloc is
	 * used here
	 */
	mhi_cntrl->mhi_chan = vcalloc(mhi_cntrl->max_chan,
				      sizeof(*mhi_cntrl->mhi_chan));
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;

	INIT_LIST_HEAD(&mhi_cntrl->lpm_chans);

	/* Populate channel configurations */
	for (i = 0; i < config->num_channels; i++) {
		struct mhi_chan *mhi_chan;

		ch_cfg = &config->ch_cfg[i];

		chan = ch_cfg->num;
		if (chan >= mhi_cntrl->max_chan) {
			dev_err(dev, "Channel %d not available\n", chan);
			goto error_chan_cfg;
		}

		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		mhi_chan->name = ch_cfg->name;
		mhi_chan->chan = chan;

		mhi_chan->tre_ring.elements = ch_cfg->num_elements;
		if (!mhi_chan->tre_ring.elements)
			goto error_chan_cfg;

		/*
		 * For some channels, local ring length should be bigger than
		 * the transfer ring length due to internal logical channels
		 * in device. So host can queue much more buffers than transfer
		 * ring length. Example, RSC channels should have a larger local
		 * channel length than transfer ring length.
		 */
		mhi_chan->buf_ring.elements = ch_cfg->local_elements;
		if (!mhi_chan->buf_ring.elements)
			mhi_chan->buf_ring.elements = mhi_chan->tre_ring.elements;
		mhi_chan->er_index = ch_cfg->event_ring;
		mhi_chan->dir = ch_cfg->dir;

		/*
		 * For most channels, chtype is identical to channel directions.
		 * So, if it is not defined then assign channel direction to
		 * chtype
		 */
		mhi_chan->type = ch_cfg->type;
		if (!mhi_chan->type)
			mhi_chan->type = (enum mhi_ch_type)mhi_chan->dir;

		mhi_chan->ee_mask = ch_cfg->ee_mask;
		mhi_chan->db_cfg.pollcfg = ch_cfg->pollcfg;
		mhi_chan->lpm_notify = ch_cfg->lpm_notify;
		mhi_chan->offload_ch = ch_cfg->offload_channel;
		mhi_chan->db_cfg.reset_req = ch_cfg->doorbell_mode_switch;
		mhi_chan->pre_alloc = ch_cfg->auto_queue;
		mhi_chan->wake_capable = ch_cfg->wake_capable;

		/*
		 * If MHI host allocates buffers, then the channel direction
		 * should be DMA_FROM_DEVICE
		 */
		if (mhi_chan->pre_alloc && mhi_chan->dir != DMA_FROM_DEVICE) {
			dev_err(dev, "Invalid channel configuration\n");
			goto error_chan_cfg;
		}

		/*
		 * Bi-directional and direction less channel must be an
		 * offload channel
		 */
		if ((mhi_chan->dir == DMA_BIDIRECTIONAL ||
		     mhi_chan->dir == DMA_NONE) && !mhi_chan->offload_ch) {
			dev_err(dev, "Invalid channel configuration\n");
			goto error_chan_cfg;
		}

		if (!mhi_chan->offload_ch) {
			mhi_chan->db_cfg.brstmode = ch_cfg->doorbell;
			if (MHI_INVALID_BRSTMODE(mhi_chan->db_cfg.brstmode)) {
				dev_err(dev, "Invalid Door bell mode\n");
				goto error_chan_cfg;
			}
		}

		if (mhi_chan->db_cfg.brstmode == MHI_DB_BRST_ENABLE)
			mhi_chan->db_cfg.process_db = mhi_db_brstmode;
		else
			mhi_chan->db_cfg.process_db = mhi_db_brstmode_disable;

		mhi_chan->configured = true;

		if (mhi_chan->lpm_notify)
			list_add_tail(&mhi_chan->node, &mhi_cntrl->lpm_chans);
	}

	return 0;

error_chan_cfg:
	vfree(mhi_cntrl->mhi_chan);

	return -EINVAL;
}

static int parse_config(struct mhi_controller *mhi_cntrl,
			const struct mhi_controller_config *config)
{
	int ret;

	/* Parse MHI channel configuration */
	ret = parse_ch_cfg(mhi_cntrl, config);
	if (ret)
		return ret;

	/* Parse MHI event configuration */
	ret = parse_ev_cfg(mhi_cntrl, config);
	if (ret)
		goto error_ev_cfg;

	mhi_cntrl->timeout_ms = config->timeout_ms;
	if (!mhi_cntrl->timeout_ms)
		mhi_cntrl->timeout_ms = MHI_TIMEOUT_MS;

	mhi_cntrl->ready_timeout_ms = config->ready_timeout_ms;
	mhi_cntrl->bounce_buf = config->use_bounce_buf;
	mhi_cntrl->buffer_len = config->buf_len;
	if (!mhi_cntrl->buffer_len)
		mhi_cntrl->buffer_len = MHI_MAX_MTU;

	/* By default, host is allowed to ring DB in both M0 and M2 states */
	mhi_cntrl->db_access = MHI_PM_M0 | MHI_PM_M2;
	if (config->m2_no_db)
		mhi_cntrl->db_access &= ~MHI_PM_M2;

	return 0;

error_ev_cfg:
	vfree(mhi_cntrl->mhi_chan);

	return ret;
}

int mhi_register_controller(struct mhi_controller *mhi_cntrl,
			    const struct mhi_controller_config *config)
{
	struct mhi_event *mhi_event;
	struct mhi_chan *mhi_chan;
	struct mhi_cmd *mhi_cmd;
	struct mhi_device *mhi_dev;
	int ret, i;

	if (!mhi_cntrl || !mhi_cntrl->cntrl_dev || !mhi_cntrl->regs ||
	    !mhi_cntrl->runtime_get || !mhi_cntrl->runtime_put ||
	    !mhi_cntrl->status_cb || !mhi_cntrl->read_reg ||
	    !mhi_cntrl->write_reg || !mhi_cntrl->nr_irqs ||
	    !mhi_cntrl->irq || !mhi_cntrl->reg_len)
		return -EINVAL;

	ret = parse_config(mhi_cntrl, config);
	if (ret)
		return -EINVAL;

	mhi_cntrl->mhi_cmd = kcalloc(NR_OF_CMD_RINGS,
				     sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd) {
		ret = -ENOMEM;
		goto err_free_event;
	}

	INIT_LIST_HEAD(&mhi_cntrl->transition_list);
	mutex_init(&mhi_cntrl->pm_mutex);
	rwlock_init(&mhi_cntrl->pm_lock);
	spin_lock_init(&mhi_cntrl->transition_lock);
	spin_lock_init(&mhi_cntrl->wlock);
	INIT_WORK(&mhi_cntrl->st_worker, mhi_pm_st_worker);
	init_waitqueue_head(&mhi_cntrl->state_event);

	mhi_cntrl->hiprio_wq = alloc_ordered_workqueue("mhi_hiprio_wq", WQ_HIGHPRI);
	if (!mhi_cntrl->hiprio_wq) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to allocate workqueue\n");
		ret = -ENOMEM;
		goto err_free_cmd;
	}

	mhi_cmd = mhi_cntrl->mhi_cmd;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++)
		spin_lock_init(&mhi_cmd->lock);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		/* Skip for offload events */
		if (mhi_event->offload_ev)
			continue;

		mhi_event->mhi_cntrl = mhi_cntrl;
		spin_lock_init(&mhi_event->lock);
		if (mhi_event->data_type == MHI_ER_CTRL)
			tasklet_init(&mhi_event->task, mhi_ctrl_ev_task,
				     (ulong)mhi_event);
		else
			tasklet_init(&mhi_event->task, mhi_ev_task,
				     (ulong)mhi_event);
	}

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		mutex_init(&mhi_chan->mutex);
		init_completion(&mhi_chan->completion);
		rwlock_init(&mhi_chan->lock);

		/* used in setting bei field of TRE */
		mhi_event = &mhi_cntrl->mhi_event[mhi_chan->er_index];
		mhi_chan->intmod = mhi_event->intmod;
	}

	if (mhi_cntrl->bounce_buf) {
		mhi_cntrl->map_single = mhi_map_single_use_bb;
		mhi_cntrl->unmap_single = mhi_unmap_single_use_bb;
	} else {
		mhi_cntrl->map_single = mhi_map_single_no_bb;
		mhi_cntrl->unmap_single = mhi_unmap_single_no_bb;
	}

	mhi_cntrl->index = ida_alloc(&mhi_controller_ida, GFP_KERNEL);
	if (mhi_cntrl->index < 0) {
		ret = mhi_cntrl->index;
		goto err_destroy_wq;
	}

	ret = mhi_init_irq_setup(mhi_cntrl);
	if (ret)
		goto err_ida_free;

	/* Register controller with MHI bus */
	mhi_dev = mhi_alloc_device(mhi_cntrl);
	if (IS_ERR(mhi_dev)) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to allocate MHI device\n");
		ret = PTR_ERR(mhi_dev);
		goto error_setup_irq;
	}

	mhi_dev->dev_type = MHI_DEVICE_CONTROLLER;
	mhi_dev->mhi_cntrl = mhi_cntrl;
	dev_set_name(&mhi_dev->dev, "mhi%d", mhi_cntrl->index);
	mhi_dev->name = dev_name(&mhi_dev->dev);

	/* Init wakeup source */
	device_init_wakeup(&mhi_dev->dev, true);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		goto err_release_dev;

	if (mhi_cntrl->edl_trigger) {
		ret = sysfs_create_file(&mhi_dev->dev.kobj, &dev_attr_trigger_edl.attr);
		if (ret)
			goto err_release_dev;
	}

	mhi_cntrl->mhi_dev = mhi_dev;

	mhi_create_debugfs(mhi_cntrl);

	return 0;

err_release_dev:
	put_device(&mhi_dev->dev);
error_setup_irq:
	mhi_deinit_free_irq(mhi_cntrl);
err_ida_free:
	ida_free(&mhi_controller_ida, mhi_cntrl->index);
err_destroy_wq:
	destroy_workqueue(mhi_cntrl->hiprio_wq);
err_free_cmd:
	kfree(mhi_cntrl->mhi_cmd);
err_free_event:
	kfree(mhi_cntrl->mhi_event);
	vfree(mhi_cntrl->mhi_chan);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_register_controller);

void mhi_unregister_controller(struct mhi_controller *mhi_cntrl)
{
	struct mhi_device *mhi_dev = mhi_cntrl->mhi_dev;
	struct mhi_chan *mhi_chan = mhi_cntrl->mhi_chan;
	unsigned int i;

	mhi_deinit_free_irq(mhi_cntrl);
	mhi_destroy_debugfs(mhi_cntrl);

	if (mhi_cntrl->edl_trigger)
		sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_trigger_edl.attr);

	destroy_workqueue(mhi_cntrl->hiprio_wq);
	kfree(mhi_cntrl->mhi_cmd);
	kfree(mhi_cntrl->mhi_event);

	/* Drop the references to MHI devices created for channels */
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		if (!mhi_chan->mhi_dev)
			continue;

		put_device(&mhi_chan->mhi_dev->dev);
	}
	vfree(mhi_cntrl->mhi_chan);

	device_del(&mhi_dev->dev);
	put_device(&mhi_dev->dev);

	ida_free(&mhi_controller_ida, mhi_cntrl->index);
}
EXPORT_SYMBOL_GPL(mhi_unregister_controller);

struct mhi_controller *mhi_alloc_controller(void)
{
	struct mhi_controller *mhi_cntrl;

	mhi_cntrl = kzalloc(sizeof(*mhi_cntrl), GFP_KERNEL);

	return mhi_cntrl;
}
EXPORT_SYMBOL_GPL(mhi_alloc_controller);

void mhi_free_controller(struct mhi_controller *mhi_cntrl)
{
	kfree(mhi_cntrl);
}
EXPORT_SYMBOL_GPL(mhi_free_controller);

int mhi_prepare_for_power_up(struct mhi_controller *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 bhi_off, bhie_off;
	int ret;

	mutex_lock(&mhi_cntrl->pm_mutex);

	ret = mhi_init_dev_ctxt(mhi_cntrl);
	if (ret)
		goto error_dev_ctxt;

	ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, BHIOFF, &bhi_off);
	if (ret) {
		dev_err(dev, "Error getting BHI offset\n");
		goto error_reg_offset;
	}

	if (bhi_off >= mhi_cntrl->reg_len) {
		dev_err(dev, "BHI offset: 0x%x is out of range: 0x%zx\n",
			bhi_off, mhi_cntrl->reg_len);
		ret = -ERANGE;
		goto error_reg_offset;
	}
	mhi_cntrl->bhi = mhi_cntrl->regs + bhi_off;

	if (mhi_cntrl->fbc_download || mhi_cntrl->rddm_size || mhi_cntrl->seg_len) {
		ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->regs, BHIEOFF,
				   &bhie_off);
		if (ret) {
			dev_err(dev, "Error getting BHIE offset\n");
			goto error_reg_offset;
		}

		if (bhie_off >= mhi_cntrl->reg_len) {
			dev_err(dev,
				"BHIe offset: 0x%x is out of range: 0x%zx\n",
				bhie_off, mhi_cntrl->reg_len);
			ret = -ERANGE;
			goto error_reg_offset;
		}
		mhi_cntrl->bhie = mhi_cntrl->regs + bhie_off;
	}

	if (mhi_cntrl->rddm_size) {
		/*
		 * This controller supports RDDM, so we need to manually clear
		 * BHIE RX registers since POR values are undefined.
		 */
		memset_io(mhi_cntrl->bhie + BHIE_RXVECADDR_LOW_OFFS,
			  0, BHIE_RXVECSTATUS_OFFS - BHIE_RXVECADDR_LOW_OFFS +
			  4);
		/*
		 * Allocate RDDM table for debugging purpose if specified
		 */
		mhi_alloc_bhie_table(mhi_cntrl, &mhi_cntrl->rddm_image,
				     mhi_cntrl->rddm_size);
		if (mhi_cntrl->rddm_image) {
			ret = mhi_rddm_prepare(mhi_cntrl,
					       mhi_cntrl->rddm_image);
			if (ret) {
				mhi_free_bhie_table(mhi_cntrl,
						    mhi_cntrl->rddm_image);
				goto error_reg_offset;
			}
		}
	}

	mutex_unlock(&mhi_cntrl->pm_mutex);

	return 0;

error_reg_offset:
	mhi_deinit_dev_ctxt(mhi_cntrl);

error_dev_ctxt:
	mutex_unlock(&mhi_cntrl->pm_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_prepare_for_power_up);

void mhi_unprepare_after_power_down(struct mhi_controller *mhi_cntrl)
{
	if (mhi_cntrl->fbc_image) {
		mhi_free_bhie_table(mhi_cntrl, mhi_cntrl->fbc_image);
		mhi_cntrl->fbc_image = NULL;
	}

	if (mhi_cntrl->rddm_image) {
		mhi_free_bhie_table(mhi_cntrl, mhi_cntrl->rddm_image);
		mhi_cntrl->rddm_image = NULL;
	}

	mhi_cntrl->bhi = NULL;
	mhi_cntrl->bhie = NULL;

	mhi_deinit_dev_ctxt(mhi_cntrl);
}
EXPORT_SYMBOL_GPL(mhi_unprepare_after_power_down);

static void mhi_release_device(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);

	/*
	 * We need to set the mhi_chan->mhi_dev to NULL here since the MHI
	 * devices for the channels will only get created if the mhi_dev
	 * associated with it is NULL. This scenario will happen during the
	 * controller suspend and resume.
	 */
	if (mhi_dev->ul_chan)
		mhi_dev->ul_chan->mhi_dev = NULL;

	if (mhi_dev->dl_chan)
		mhi_dev->dl_chan->mhi_dev = NULL;

	kfree(mhi_dev);
}

struct mhi_device *mhi_alloc_device(struct mhi_controller *mhi_cntrl)
{
	struct mhi_device *mhi_dev;
	struct device *dev;

	mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	if (!mhi_dev)
		return ERR_PTR(-ENOMEM);

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_bus_type;
	dev->release = mhi_release_device;

	if (mhi_cntrl->mhi_dev) {
		/* for MHI client devices, parent is the MHI controller device */
		dev->parent = &mhi_cntrl->mhi_dev->dev;
	} else {
		/* for MHI controller device, parent is the bus device (e.g. pci device) */
		dev->parent = mhi_cntrl->cntrl_dev;
	}

	mhi_dev->mhi_cntrl = mhi_cntrl;
	mhi_dev->dev_wake = 0;

	return mhi_dev;
}

static int mhi_driver_probe(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct device_driver *drv = dev->driver;
	struct mhi_driver *mhi_drv = to_mhi_driver(drv);
	struct mhi_event *mhi_event;
	struct mhi_chan *ul_chan = mhi_dev->ul_chan;
	struct mhi_chan *dl_chan = mhi_dev->dl_chan;
	int ret;

	/* Bring device out of LPM */
	ret = mhi_device_get_sync(mhi_dev);
	if (ret)
		return ret;

	ret = -EINVAL;

	if (ul_chan) {
		/*
		 * If channel supports LPM notifications then status_cb should
		 * be provided
		 */
		if (ul_chan->lpm_notify && !mhi_drv->status_cb)
			goto exit_probe;

		/* For non-offload channels then xfer_cb should be provided */
		if (!ul_chan->offload_ch && !mhi_drv->ul_xfer_cb)
			goto exit_probe;

		ul_chan->xfer_cb = mhi_drv->ul_xfer_cb;
	}

	ret = -EINVAL;
	if (dl_chan) {
		/*
		 * If channel supports LPM notifications then status_cb should
		 * be provided
		 */
		if (dl_chan->lpm_notify && !mhi_drv->status_cb)
			goto exit_probe;

		/* For non-offload channels then xfer_cb should be provided */
		if (!dl_chan->offload_ch && !mhi_drv->dl_xfer_cb)
			goto exit_probe;

		mhi_event = &mhi_cntrl->mhi_event[dl_chan->er_index];

		/*
		 * If the channel event ring is managed by client, then
		 * status_cb must be provided so that the framework can
		 * notify pending data
		 */
		if (mhi_event->cl_manage && !mhi_drv->status_cb)
			goto exit_probe;

		dl_chan->xfer_cb = mhi_drv->dl_xfer_cb;
	}

	/* Call the user provided probe function */
	ret = mhi_drv->probe(mhi_dev, mhi_dev->id);
	if (ret)
		goto exit_probe;

	mhi_device_put(mhi_dev);

	return ret;

exit_probe:
	mhi_unprepare_from_transfer(mhi_dev);

	mhi_device_put(mhi_dev);

	return ret;
}

static int mhi_driver_remove(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_driver *mhi_drv = to_mhi_driver(dev->driver);
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;
	enum mhi_ch_state ch_state[] = {
		MHI_CH_STATE_DISABLED,
		MHI_CH_STATE_DISABLED
	};
	int dir;

	/* Skip if it is a controller device */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	/* Reset both channels */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		/* Wake all threads waiting for completion */
		write_lock_irq(&mhi_chan->lock);
		mhi_chan->ccs = MHI_EV_CC_INVALID;
		complete_all(&mhi_chan->completion);
		write_unlock_irq(&mhi_chan->lock);

		/* Set the channel state to disabled */
		mutex_lock(&mhi_chan->mutex);
		write_lock_irq(&mhi_chan->lock);
		ch_state[dir] = mhi_chan->ch_state;
		mhi_chan->ch_state = MHI_CH_STATE_SUSPENDED;
		write_unlock_irq(&mhi_chan->lock);

		/* Reset the non-offload channel */
		if (!mhi_chan->offload_ch)
			mhi_reset_chan(mhi_cntrl, mhi_chan);

		mutex_unlock(&mhi_chan->mutex);
	}

	mhi_drv->remove(mhi_dev);

	/* De-init channel if it was enabled */
	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		mutex_lock(&mhi_chan->mutex);

		if ((ch_state[dir] == MHI_CH_STATE_ENABLED ||
		     ch_state[dir] == MHI_CH_STATE_STOP) &&
		    !mhi_chan->offload_ch)
			mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);

		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;

		mutex_unlock(&mhi_chan->mutex);
	}

	while (mhi_dev->dev_wake)
		mhi_device_put(mhi_dev);

	return 0;
}

int __mhi_driver_register(struct mhi_driver *mhi_drv, struct module *owner)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	driver->bus = &mhi_bus_type;
	driver->owner = owner;
	driver->probe = mhi_driver_probe;
	driver->remove = mhi_driver_remove;

	return driver_register(driver);
}
EXPORT_SYMBOL_GPL(__mhi_driver_register);

void mhi_driver_unregister(struct mhi_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL_GPL(mhi_driver_unregister);

static int mhi_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct mhi_device *mhi_dev = to_mhi_device(dev);

	return add_uevent_var(env, "MODALIAS=" MHI_DEVICE_MODALIAS_FMT,
					mhi_dev->name);
}

static int mhi_match(struct device *dev, const struct device_driver *drv)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	const struct mhi_driver *mhi_drv = to_mhi_driver(drv);
	const struct mhi_device_id *id;

	/*
	 * If the device is a controller type then there is no client driver
	 * associated with it
	 */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	for (id = mhi_drv->id_table; id->chan[0]; id++)
		if (!strcmp(mhi_dev->name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

const struct bus_type mhi_bus_type = {
	.name = "mhi",
	.dev_name = "mhi",
	.match = mhi_match,
	.uevent = mhi_uevent,
	.dev_groups = mhi_dev_groups,
};

static int __init mhi_init(void)
{
	mhi_debugfs_init();
	return bus_register(&mhi_bus_type);
}

static void __exit mhi_exit(void)
{
	mhi_debugfs_exit();
	bus_unregister(&mhi_bus_type);
}

postcore_initcall(mhi_init);
module_exit(mhi_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Modem Host Interface");

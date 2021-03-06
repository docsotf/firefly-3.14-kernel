/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_dma.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>

#include "dmaengine.h"
#include "virt-dma.h"

/* ADM registers - calculated from channel number and security domain */
#define HI_CH_CMD_PTR(chan, ee)		(4*chan + 0x20800*ee)
#define HI_CH_RSLT(chan, ee)		(0x40 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE0(chan, ee)	(0x80 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE1(chan, ee)	(0xc0 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE2(chan, ee)	(0x100 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE3(chan, ee)	(0x140 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE4(chan, ee)	(0x180 + 4*chan + 0x20800*ee)
#define HI_CH_FLUSH_STATE5(chan, ee)	(0x1c0 + 4*chan + 0x20800*ee)
#define HI_CH_STATUS_SD(chan, ee)	(0x200 + 4*chan + 0x20800*ee)
#define HI_CH_CONF(chan)		(0x240 + 4*chan)
#define HI_CH_RSLT_CONF(chan, ee)	(0x300 + 4*chan + 0x20800*ee)
#define HI_SEC_DOMAIN_IRQ_STATUS(ee)	(0x380 + 0x20800*ee)
#define HI_CI_CONF(ci)			(0x390 + 4*ci)
#define HI_CRCI_CONF0			0x3d0
#define HI_CRCI_CONF1			0x3d4
#define HI_GP_CTL			0x3d8
#define HI_CRCI_CTL(crci, ee)		(0x400 + 0x4*crci + 0x20800*ee)

/* channel status */
#define CH_STATUS_VALID	BIT(1)

/* channel result */
#define CH_RSLT_VALID	BIT(31)
#define CH_RSLT_ERR	BIT(3)
#define CH_RSLT_FLUSH	BIT(2)
#define CH_RSLT_TPD	BIT(1)

/* channel conf */
#define CH_CONF_MPU_DISABLE	BIT(11)
#define CH_CONF_PERM_MPU_CONF	BIT(9)
#define CH_CONF_FLUSH_RSLT_EN	BIT(8)
#define CH_CONF_FORCE_RSLT_EN	BIT(7)
#define CH_CONF_IRQ_EN		BIT(6)

/* channel result conf */
#define CH_RSLT_CONF_FLUSH_EN	BIT(1)
#define CH_RSLT_CONF_IRQ_EN	BIT(0)

/* CRCI CTL */
#define CRCI_CTL_MUX_SEL	BIT(18)
#define CRCI_CTL_RST		BIT(17)

/* CI configuration */
#define CI_RANGE_END(x)		(x << 24)
#define CI_RANGE_START(x)	(x << 16)
#define CI_BURST_4_WORDS	0x4
#define CI_BURST_8_WORDS	0x8

/* GP CTL */
#define GP_CTL_LP_EN		BIT(12)
#define GP_CTL_LP_CNT(x)	(x << 8)

/* Command pointer list entry */
#define CPLE_LP			BIT(31)
#define CPLE_CMD_PTR_LIST	BIT(29)

/* Command list entry */
#define CMD_LC			BIT(31)
#define CMD_DST_CRCI(n)		(((n) & 0xf) << 7)
#define CMD_SRC_CRCI(n)		(((n) & 0xf) << 3)

#define CMD_TYPE_SINGLE		0x0
#define CMD_TYPE_BOX		0x3

#define ADM_DESC_ALIGN	8
#define ADM_MAX_XFER	(SZ_64K-1)
#define ADM_MAX_ROWS	(SZ_64K-1)

/* Command Pointer List Entry */
#define CMD_LP		BIT(31)
#define CMD_PT_MASK	(0x3 << 29)
#define CMD_ADDR_MASK	0x3fffffff

struct adm_desc_hw {
	u32 cmd;
	u32 src_addr;
	u32 dst_addr;
	u32 row_len;
	u32 num_rows;
	u32 row_offset;
};

struct adm_cmd_ptr_list {
	u32 cple;			/* command ptr list entry */
	struct adm_desc_hw desc[0];
};

struct adm_async_desc {
	struct virt_dma_desc vd;
	struct adm_device *adev;

	size_t length;
	enum dma_transfer_direction dir;
	dma_addr_t dma_addr;
	size_t dma_len;

	struct adm_cmd_ptr_list *cpl;
	u32 num_desc;
};

struct adm_chan {
	struct virt_dma_chan vc;
	struct adm_device *adev;

	/* parsed from DT */
	u32 id;			/* channel id */
	u32 crci_mux;		/* determines primary/secondary crci mux */
	u32 crci;		/* CRCI to be used for transfers */
	u32 blk_size;		/* block size for CRCI, default 16 byte */

	struct adm_async_desc *curr_txd;
	struct dma_slave_config slave;
	struct list_head node;

	int error;
	int initialized;
};

static inline struct adm_chan *to_adm_chan(struct dma_chan *common)
{
	return container_of(common, struct adm_chan, vc.chan);
}

struct adm_device {
	void __iomem *regs;
	struct device *dev;
	struct dma_device common;
	struct device_dma_parameters dma_parms;
	struct adm_chan *channels;
	u32 num_channels;

	u32 ee;

	struct clk *core_clk;
	struct clk *iface_clk;

	struct reset_control *clk_reset;
	struct reset_control *c0_reset;
	struct reset_control *c1_reset;
	struct reset_control *c2_reset;
	int irq;
};

/**
 * adm_alloc_chan - Allocates channel resources for DMA channel
 *
 * This function is effectively a stub, as we don't need to setup any resources
 */
static int adm_alloc_chan(struct dma_chan *chan)
{
	return 0;
}

/**
 * adm_free_chan - Frees dma resources associated with the specific channel
 *
 * Free all allocated descriptors associated with this channel
 *
 */
static void adm_free_chan(struct dma_chan *chan)
{
	/* free all queued descriptors */
	vchan_free_chan_resources(to_virt_chan(chan));
}

/**
 * adm_prep_slave_sg - Prep slave sg transaction
 *
 * @chan: dma channel
 * @sgl: scatter gather list
 * @sg_len: length of sg
 * @direction: DMA transfer direction
 * @flags: DMA flags
 * @context: transfer context (unused)
 */
static struct dma_async_tx_descriptor *adm_prep_slave_sg(struct dma_chan *chan,
	struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction, unsigned long flags,
	void *context)
{
	struct adm_chan *achan = to_adm_chan(chan);
	struct adm_device *adev = achan->adev;
	struct adm_async_desc *async_desc;
	struct scatterlist *sg;
	u32 i, rows, num_desc = 0, idx = 0, desc_offset;
	struct adm_desc_hw *desc;
	struct adm_cmd_ptr_list *cpl;
	u32 burst = ADM_MAX_XFER;


	if (!is_slave_direction(direction)) {
		dev_err(adev->dev, "invalid dma direction\n");
		return NULL;
	}

	/* if using CRCI flow control, validate burst settings */
	if (achan->slave.device_fc) {
		burst = (direction == DMA_MEM_TO_DEV) ?
			achan->slave.dst_maxburst :
			achan->slave.src_maxburst;

		if (!burst) {
			dev_err(adev->dev, "invalid burst value w/ crci: %d\n",
				burst);
			return ERR_PTR(-EINVAL);
		}
	}

	/* iterate through sgs and compute allocation size of structures */
	for_each_sg(sgl, sg, sg_len, i) {

		/* calculate boxes using burst */
		rows = DIV_ROUND_UP(sg_dma_len(sg), burst);
		num_desc += DIV_ROUND_UP(rows, ADM_MAX_ROWS);

		/* flow control requires length as a multiple of burst */
		if (achan->slave.device_fc && (sg_dma_len(sg) % burst)) {
			dev_err(adev->dev, "length is not multiple of burst\n");
			return ERR_PTR(-EINVAL);
		}
	}

	async_desc = kzalloc(sizeof(*async_desc), GFP_NOWAIT);
	if (!async_desc)
		return ERR_PTR(-ENOMEM);

	async_desc->dma_len = num_desc * sizeof(*desc) + sizeof(*cpl) +
				2*ADM_DESC_ALIGN;
	async_desc->cpl = dma_alloc_writecombine(adev->dev, async_desc->dma_len,
			&async_desc->dma_addr, GFP_NOWAIT);

	if (!async_desc->cpl) {
		kfree(async_desc);
		return ERR_PTR(-ENOMEM);
	}

	async_desc->num_desc = num_desc;
	async_desc->adev = adev;
	cpl = PTR_ALIGN(async_desc->cpl, ADM_DESC_ALIGN);
	desc = PTR_ALIGN(&cpl->desc[0], ADM_DESC_ALIGN);
	desc_offset = (u32)desc - (u32)async_desc->cpl;

	/* init cmd list */
	cpl->cple = CPLE_LP;
	cpl->cple |= (async_desc->dma_addr + desc_offset) >> 3;

	for_each_sg(sgl, sg, sg_len, i) {
		unsigned int remainder = sg_dma_len(sg);
		unsigned int curr_offset = 0;
		unsigned int row_len;

		do {
			desc[idx].cmd = CMD_TYPE_BOX;
			desc[idx].row_offset = 0;

			if (direction == DMA_DEV_TO_MEM) {
				desc[idx].dst_addr = sg_dma_address(sg) +
							curr_offset;
				desc[idx].src_addr = achan->slave.src_addr;
				desc[idx].cmd |= CMD_SRC_CRCI(achan->crci);
				desc[idx].row_offset = burst;
			} else {
				desc[idx].src_addr = sg_dma_address(sg) +
							curr_offset;
				desc[idx].dst_addr = achan->slave.dst_addr;
				desc[idx].cmd |= CMD_DST_CRCI(achan->crci);
				desc[idx].row_offset = burst << 16;
			}

			if (remainder < burst) {
				rows = 1;
				row_len = remainder;
			} else {
				rows = remainder / burst;
				rows = min_t(u32, rows, ADM_MAX_ROWS);
				row_len = burst;
			}

			desc[idx].num_rows = rows << 16 | rows;
			desc[idx].row_len = row_len << 16 | row_len;

			remainder -= row_len * rows;
			async_desc->length += row_len * rows;
			curr_offset += row_len * rows;

			idx++;
		} while (remainder > 0);
	}

	/* set last command flag */
	desc[idx - 1].cmd |= CMD_LC;

	/* reset channel error */
	achan->error = 0;

	return vchan_tx_prep(&achan->vc, &async_desc->vd, flags);
}

/**
 * adm_slave_config - set slave configuration for channel
 * @chan: dma channel
 * @cfg: slave configuration
 *
 * Sets slave configuration for channel
 *
 */
static int adm_slave_config(struct adm_chan *achan,
		struct dma_slave_config *cfg)
{
	int ret = 0;
	u32 burst;

	memcpy(&achan->slave, cfg, sizeof(*cfg));

	/* set channel CRCI burst, if applicable */
	if (achan->crci) {
		burst = max_t(u32, cfg->src_maxburst, cfg->dst_maxburst);

		switch (burst) {
		case 16:
			achan->blk_size = 0;
			break;
		case 32:
			achan->blk_size = 1;
			break;
		case 64:
			achan->blk_size = 2;
			break;
		case 128:
			achan->blk_size = 3;
			break;
		case 192:
			achan->blk_size = 4;
			break;
		case 256:
			achan->blk_size = 5;
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

/**
 * adm_terminate_all - terminate all transactions on a channel
 * @achan: adm dma channel
 *
 * Dequeues and frees all transactions, aborts current transaction
 * No callbacks are done
 *
 */
static void adm_terminate_all(struct adm_chan *achan)
{
	struct adm_device *adev = achan->adev;
	unsigned long flags;
	LIST_HEAD(head);

	/* send flush command to terminate current transaction */
	writel_relaxed(0x0,
		adev->regs + HI_CH_FLUSH_STATE0(achan->id, adev->ee));

	spin_lock_irqsave(&achan->vc.lock, flags);
	vchan_get_all_descriptors(&achan->vc, &head);
	spin_unlock_irqrestore(&achan->vc.lock, flags);

	vchan_dma_desc_free_list(&achan->vc, &head);
}

/**
 * adm_control - DMA device control
 * @chan: dma channel
 * @cmd: control cmd
 * @arg: cmd argument
 *
 * Perform DMA control command
 *
 */
static int adm_control(struct dma_chan *chan, enum dma_ctrl_cmd cmd,
	unsigned long arg)
{
	struct adm_chan *achan = to_adm_chan(chan);
	unsigned long flag;
	int ret = 0;

	switch (cmd) {
	case DMA_SLAVE_CONFIG:
		spin_lock_irqsave(&achan->vc.lock, flag);
		ret = adm_slave_config(achan, (struct dma_slave_config *)arg);
		spin_unlock_irqrestore(&achan->vc.lock, flag);
		break;

	case DMA_TERMINATE_ALL:
		adm_terminate_all(achan);
		break;

	default:
		ret = -ENXIO;
		break;
	};

	return ret;
}

/**
 * adm_start_dma - start next transaction
 * @achan - ADM dma channel
 */
static void adm_start_dma(struct adm_chan *achan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&achan->vc);
	struct adm_device *adev = achan->adev;
	struct adm_async_desc *async_desc;
	u32 val;

	lockdep_assert_held(&achan->vc.lock);

	if (!vd)
		return;

	list_del(&vd->node);

	/* write next command list out to the CMD FIFO */
	async_desc = container_of(vd, struct adm_async_desc, vd);
	achan->curr_txd = async_desc;

	if (!achan->initialized) {
		/* enable interrupts */
		writel(CH_CONF_IRQ_EN | CH_CONF_FLUSH_RSLT_EN |
			CH_CONF_FORCE_RSLT_EN | CH_CONF_PERM_MPU_CONF |
			CH_CONF_MPU_DISABLE,
			adev->regs + HI_CH_CONF(achan->id));

		writel(CH_RSLT_CONF_IRQ_EN | CH_RSLT_CONF_FLUSH_EN,
			adev->regs + HI_CH_RSLT_CONF(achan->id, adev->ee));

		achan->initialized = 1;
	}

	/* set the crci block size */
	if (achan->crci)
		writel(achan->crci_mux | achan->blk_size,
			adev->regs + HI_CRCI_CTL(achan->crci, adev->ee));

	/* make sure IRQ enable doesn't get reordered */
	wmb();

	val = ALIGN(async_desc->dma_addr, ADM_DESC_ALIGN) >> 3;
	val |= CPLE_CMD_PTR_LIST;

	/* write next command list out to the CMD FIFO */
	writel(val, adev->regs + HI_CH_CMD_PTR(achan->id, adev->ee));
}

/**
 * adm_dma_irq - irq handler for ADM controller
 * @irq: IRQ of interrupt
 * @data: callback data
 *
 * IRQ handler for the bam controller
 */
static irqreturn_t adm_dma_irq(int irq, void *data)
{
	struct adm_device *adev = data;
	u32 srcs, i;
	struct adm_async_desc *async_desc;
	unsigned long flags;

	srcs = readl_relaxed(adev->regs +
			HI_SEC_DOMAIN_IRQ_STATUS(adev->ee));

	for (i = 0; i < 16; i++) {
		struct adm_chan *achan = &adev->channels[i];
		u32 status, result;
		if (srcs & BIT(i)) {
			status = readl_relaxed(adev->regs +
				HI_CH_STATUS_SD(i, adev->ee));

			/* if no result present, skip */
			if (!(status & CH_STATUS_VALID))
				continue;

			result = readl_relaxed(adev->regs +
				HI_CH_RSLT(i, adev->ee));

			/* no valid results, skip */
			if (!(result & CH_RSLT_VALID))
				continue;

			/* flag error if transaction was flushed or failed */
			if (result & (CH_RSLT_ERR | CH_RSLT_FLUSH))
				achan->error = 1;

			spin_lock_irqsave(&achan->vc.lock, flags);
			async_desc = achan->curr_txd;

			achan->curr_txd = NULL;

			if (async_desc) {
				vchan_cookie_complete(&async_desc->vd);

				/* kick off next DMA */
				adm_start_dma(achan);
			}

			spin_unlock_irqrestore(&achan->vc.lock, flags);
		}
	}

	return IRQ_HANDLED;
}

/**
 * adm_tx_status - returns status of transaction
 * @chan: dma channel
 * @cookie: transaction cookie
 * @txstate: DMA transaction state
 *
 * Return status of dma transaction
 */
static enum dma_status adm_tx_status(struct dma_chan *chan, dma_cookie_t cookie,
	struct dma_tx_state *txstate)
{
	struct adm_chan *achan = to_adm_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;
	size_t residue = 0;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&achan->vc.lock, flags);

	vd = vchan_find_desc(&achan->vc, cookie);
	if (vd)
		residue = container_of(vd, struct adm_async_desc, vd)->length;

	spin_unlock_irqrestore(&achan->vc.lock, flags);

	/*
	 * residue is either the full length if it is in the issued list, or 0
	 * if it is in progress.  We have no reliable way of determining
	 * anything inbetween
	*/
	dma_set_residue(txstate, residue);

	if (achan->error)
		return DMA_ERROR;

	return ret;
}

static struct dma_chan *adm_dma_xlate(struct of_phandle_args *dma_spec,
	struct of_dma *of)
{
	struct adm_device *adev = container_of(of->of_dma_data,
			struct adm_device, common);
	struct adm_chan *achan;
	struct dma_chan *chan;
	unsigned int request;
	unsigned int crci;

	if (dma_spec->args_count != 2) {
		dev_err(adev->dev, "incorrect number of dma arguments\n");
		return NULL;
	}

	request = dma_spec->args[0];
	if (request >= adev->num_channels)
		return NULL;

	crci = dma_spec->args[1];

	chan = dma_get_slave_channel(&(adev->channels[request].vc.chan));

	if (!chan)
		return NULL;

	achan = to_adm_chan(chan);

	/* lower 4 bits denotes crci port, upper bits denote mux setting */
	achan->crci = crci & 0xf;
	achan->crci_mux = (crci >> 4) ? CRCI_CTL_MUX_SEL : 0;

	return chan;
}

/**
 * adm_issue_pending - starts pending transactions
 * @chan: dma channel
 *
 * Issues all pending transactions and starts DMA
 */
static void adm_issue_pending(struct dma_chan *chan)
{
	struct adm_chan *achan = to_adm_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&achan->vc.lock, flags);

	if (vchan_issue_pending(&achan->vc) && !achan->curr_txd)
		adm_start_dma(achan);
	spin_unlock_irqrestore(&achan->vc.lock, flags);
}

/**
 * adm_dma_free_desc - free descriptor memory
 * @vd: virtual descriptor
 *
 */
static void adm_dma_free_desc(struct virt_dma_desc *vd)
{
	struct adm_async_desc *async_desc = container_of(vd,
			struct adm_async_desc, vd);

	dma_free_writecombine(async_desc->adev->dev, async_desc->dma_len,
		async_desc->cpl, async_desc->dma_addr);
	kfree(async_desc);
}

static void adm_channel_init(struct adm_device *adev, struct adm_chan *achan,
	u32 index)
{
	achan->id = index;
	achan->adev = adev;

	vchan_init(&achan->vc, &adev->common);
	achan->vc.desc_free = adm_dma_free_desc;
}

static int adm_dma_probe(struct platform_device *pdev)
{
	struct adm_device *adev;
	struct resource *iores;
	int ret;
	u32 i;

	adev = devm_kzalloc(&pdev->dev, sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->dev = &pdev->dev;

	iores = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adev->regs = devm_ioremap_resource(&pdev->dev, iores);
	if (IS_ERR(adev->regs))
		return PTR_ERR(adev->regs);

	adev->irq = platform_get_irq(pdev, 0);
	if (adev->irq < 0)
		return adev->irq;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,ee", &adev->ee);
	if (ret) {
		dev_err(adev->dev, "Execution environment unspecified\n");
		return ret;
	}

	adev->core_clk = devm_clk_get(adev->dev, "core");
	if (IS_ERR(adev->core_clk))
		return PTR_ERR(adev->core_clk);

	ret = clk_prepare_enable(adev->core_clk);
	if (ret) {
		dev_err(adev->dev, "failed to prepare/enable core clock\n");
		return ret;
	}

	adev->iface_clk = devm_clk_get(adev->dev, "iface");
	if (IS_ERR(adev->iface_clk))
		return PTR_ERR(adev->iface_clk);

	ret = clk_prepare_enable(adev->iface_clk);
	if (ret) {
		dev_err(adev->dev, "failed to prepare/enable iface clock\n");
		return ret;
	}

	adev->clk_reset = devm_reset_control_get(&pdev->dev, "clk");
	if (IS_ERR(adev->clk_reset)) {
		dev_err(adev->dev, "failed to get ADM0 reset\n");
		return PTR_ERR(adev->clk_reset);
	}

	adev->c0_reset = devm_reset_control_get(&pdev->dev, "c0");
	if (IS_ERR(adev->c0_reset)) {
		dev_err(adev->dev, "failed to get ADM0 C0 reset\n");
		return PTR_ERR(adev->c0_reset);
	}

	adev->c1_reset = devm_reset_control_get(&pdev->dev, "c1");
	if (IS_ERR(adev->c1_reset)) {
		dev_err(adev->dev, "failed to get ADM0 C1 reset\n");
		return PTR_ERR(adev->c1_reset);
	}

	adev->c2_reset = devm_reset_control_get(&pdev->dev, "c2");
	if (IS_ERR(adev->c2_reset)) {
		dev_err(adev->dev, "failed to get ADM0 C2 reset\n");
		return PTR_ERR(adev->c2_reset);
	}

	reset_control_assert(adev->clk_reset);
	reset_control_assert(adev->c0_reset);
	reset_control_assert(adev->c1_reset);
	reset_control_assert(adev->c2_reset);

	reset_control_deassert(adev->clk_reset);
	reset_control_deassert(adev->c0_reset);
	reset_control_deassert(adev->c1_reset);
	reset_control_deassert(adev->c2_reset);

	adev->num_channels = 16;

	adev->channels = devm_kcalloc(adev->dev, adev->num_channels,
				sizeof(*adev->channels), GFP_KERNEL);

	if (!adev->channels) {
		ret = -ENOMEM;
		goto err_disable_clk;
	}

	/* allocate and initialize channels */
	INIT_LIST_HEAD(&adev->common.channels);

	for (i = 0; i < adev->num_channels; i++)
		adm_channel_init(adev, &adev->channels[i], i);

	/* reset CRCIs */
	for (i = 0; i < 16; i++)
		writel(CRCI_CTL_RST, adev->regs + HI_CRCI_CTL(i, adev->ee));

	/* configure client interfaces */
	writel(CI_RANGE_START(0x40) | CI_RANGE_END(0xb0) | CI_BURST_8_WORDS,
		adev->regs + HI_CI_CONF(0));
	writel(CI_RANGE_START(0x2a) | CI_RANGE_END(0x2c) | CI_BURST_8_WORDS,
		adev->regs + HI_CI_CONF(1));
	writel(CI_RANGE_START(0x12) | CI_RANGE_END(0x28) | CI_BURST_8_WORDS,
		adev->regs + HI_CI_CONF(2));
	writel(GP_CTL_LP_EN | GP_CTL_LP_CNT(0xf), adev->regs + HI_GP_CTL);

	ret = devm_request_irq(adev->dev, adev->irq, adm_dma_irq,
			0, "adm_dma", adev);
	if (ret)
		goto err_disable_clk;

	platform_set_drvdata(pdev, adev);

	adev->common.dev = adev->dev;
	adev->common.dev->dma_parms = &adev->dma_parms;

	/* set capabilities */
	dma_cap_zero(adev->common.cap_mask);
	dma_cap_set(DMA_SLAVE, adev->common.cap_mask);
	dma_cap_set(DMA_PRIVATE, adev->common.cap_mask);

	/* initialize dmaengine apis */
	adev->common.device_alloc_chan_resources = adm_alloc_chan;
	adev->common.device_free_chan_resources = adm_free_chan;
	adev->common.device_prep_slave_sg = adm_prep_slave_sg;
	adev->common.device_control = adm_control;
	adev->common.device_issue_pending = adm_issue_pending;
	adev->common.device_tx_status = adm_tx_status;

	ret = dma_async_device_register(&adev->common);
	if (ret) {
		dev_err(adev->dev, "failed to register dma async device\n");
		goto err_disable_clk;
	}

	ret = of_dma_controller_register(pdev->dev.of_node, adm_dma_xlate,
					&adev->common);
	if (ret)
		goto err_unregister_dma;

	return 0;

err_unregister_dma:
	dma_async_device_unregister(&adev->common);
err_disable_clk:
	clk_disable_unprepare(adev->core_clk);
	clk_disable_unprepare(adev->iface_clk);

	return ret;
}

static int adm_dma_remove(struct platform_device *pdev)
{
	struct adm_device *adev = platform_get_drvdata(pdev);
	struct adm_chan *achan;
	u32 i;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&adev->common);

	devm_free_irq(adev->dev, adev->irq, adev);

	for (i = 0; i < adev->num_channels; i++) {
		achan = &adev->channels[i];
		writel(CH_CONF_FLUSH_RSLT_EN,
			adev->regs + HI_CH_CONF(achan->id));
		writel(CH_RSLT_CONF_FLUSH_EN,
			adev->regs + HI_CH_RSLT_CONF(achan->id, adev->ee));

		adm_terminate_all(&adev->channels[i]);
	}

	clk_disable_unprepare(adev->core_clk);
	clk_disable_unprepare(adev->iface_clk);

	return 0;
}

static const struct of_device_id adm_of_match[] = {
	{ .compatible = "qcom,adm", },
	{}
};
MODULE_DEVICE_TABLE(of, adm_of_match);

static struct platform_driver adm_dma_driver = {
	.probe = adm_dma_probe,
	.remove = adm_dma_remove,
	.driver = {
		.name = "adm-dma-engine",
		.owner = THIS_MODULE,
		.of_match_table = adm_of_match,
	},
};

module_platform_driver(adm_dma_driver);

MODULE_AUTHOR("Andy Gross <agross@codeaurora.org>");
MODULE_DESCRIPTION("QCOM ADM DMA engine driver");
MODULE_LICENSE("GPL v2");

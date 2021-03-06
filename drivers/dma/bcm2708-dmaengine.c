/*
 * BCM2708 DMA engine support
 *
 * This driver only supports cyclic DMA transfers
 * as needed for the I2S module.
 *
 * Author:      Florian Meier <florian.meier@koalo.de>
 *              Copyright 2013
 *
 * Based on
 *	OMAP DMAengine support by Russell King
 *
 *	BCM2708 DMA Driver
 *	Copyright (C) 2010 Broadcom
 *
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	MARVELL MMP Peripheral DMA Driver
 *	Copyright 2012 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/irq.h>

#include "virt-dma.h"

#include <mach/dma.h>
#include <mach/irqs.h>

struct bcm2708_dmadev {
	struct dma_device ddev;
	spinlock_t lock;
	void __iomem *base;
	struct device_dma_parameters dma_parms;
};

struct bcm2708_chan {
	struct virt_dma_chan vc;
	struct list_head node;

	struct dma_slave_config	cfg;
	bool cyclic;

	int ch;
	struct bcm2708_desc *desc;

	void __iomem *chan_base;
	int irq_number;
};

struct bcm2708_desc {
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int control_block_size;
	struct bcm2708_dma_cb *control_block_base;
	dma_addr_t control_block_base_phys;

	unsigned frames;
	size_t size;
};

#define BCM2708_DMA_DATA_TYPE_S8	1
#define BCM2708_DMA_DATA_TYPE_S16	2
#define BCM2708_DMA_DATA_TYPE_S32	4
#define BCM2708_DMA_DATA_TYPE_S128	16

static inline struct bcm2708_dmadev *to_bcm2708_dma_dev(struct dma_device *d)
{
	return container_of(d, struct bcm2708_dmadev, ddev);
}

static inline struct bcm2708_chan *to_bcm2708_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct bcm2708_chan, vc.chan);
}

static inline struct bcm2708_desc *to_bcm2708_dma_desc(
		struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct bcm2708_desc, vd.tx);
}

static void bcm2708_dma_desc_free(struct virt_dma_desc *vd)
{
	struct bcm2708_desc *desc = container_of(vd, struct bcm2708_desc, vd);
	dma_free_coherent(desc->vd.tx.chan->device->dev,
			desc->control_block_size,
			desc->control_block_base,
			desc->control_block_base_phys);
	kfree(desc);
}

static void bcm2708_dma_start_desc(struct bcm2708_chan *c)
{
	struct virt_dma_desc *vd = vchan_next_desc(&c->vc);
	struct bcm2708_desc *d;

	if (!vd) {
		c->desc = NULL;
		return;
	}

	list_del(&vd->node);

	c->desc = d = to_bcm2708_dma_desc(&vd->tx);

	bcm_dma_start(c->chan_base, d->control_block_base_phys);
}

static irqreturn_t bcm2708_dma_callback(int irq, void *data)
{
	struct bcm2708_chan *c = data;
	struct bcm2708_desc *d;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);

	/* Acknowledge interrupt */
	writel(BCM2708_DMA_INT, c->chan_base + BCM2708_DMA_CS);

	d = c->desc;

	if (d) {
		/* TODO Only works for cyclic DMA */
		vchan_cyclic_callback(&d->vd);
	}

	/* Keep the DMA engine running */
	dsb(); /* ARM synchronization barrier */
	writel(BCM2708_DMA_ACTIVE, c->chan_base + BCM2708_DMA_CS);

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static int bcm2708_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);

	return request_irq(c->irq_number,
			bcm2708_dma_callback, 0, "DMA IRQ", c);
}

static void bcm2708_dma_free_chan_resources(struct dma_chan *chan)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);

	vchan_free_chan_resources(&c->vc);
	free_irq(c->irq_number, c);

	dev_dbg(c->vc.chan.device->dev, "Freeing DMA channel %u\n", c->ch);
}

static size_t bcm2708_dma_desc_size(struct bcm2708_desc *d)
{
	return d->size;
}

static size_t bcm2708_dma_desc_size_pos(struct bcm2708_desc *d, dma_addr_t addr)
{
	unsigned i;
	size_t size;

	for (size = i = 0; i < d->frames; i++) {
		struct bcm2708_dma_cb *control_block =
			&d->control_block_base[i];
		size_t this_size = control_block->length;
		dma_addr_t dma;

		if (d->dir == DMA_DEV_TO_MEM)
			dma = control_block->dst;
		else
			dma = control_block->src;

		if (size)
			size += this_size;
		else if (addr >= dma && addr < dma + this_size)
			size += dma + this_size - addr;
	}

	return size;
}

static enum dma_status bcm2708_dma_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&c->vc.lock, flags);
	vd = vchan_find_desc(&c->vc, cookie);
	if (vd) {
		txstate->residue =
			bcm2708_dma_desc_size(to_bcm2708_dma_desc(&vd->tx));
	} else if (c->desc && c->desc->vd.tx.cookie == cookie) {
		struct bcm2708_desc *d = c->desc;
		dma_addr_t pos;

		if (d->dir == DMA_MEM_TO_DEV)
			pos = readl(c->chan_base + BCM2708_DMA_SOURCE_AD);
		else if (d->dir == DMA_DEV_TO_MEM)
			pos = readl(c->chan_base + BCM2708_DMA_DEST_AD);
		else
			pos = 0;

		txstate->residue = bcm2708_dma_desc_size_pos(d, pos);
	} else {
		txstate->residue = 0;
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

static void bcm2708_dma_issue_pending(struct dma_chan *chan)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);
	unsigned long flags;

	c->cyclic = true; /* Nothing else is implemented */

	spin_lock_irqsave(&c->vc.lock, flags);
	if (vchan_issue_pending(&c->vc) && !c->desc)
		bcm2708_dma_start_desc(c);

	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static struct dma_async_tx_descriptor *bcm2708_dma_prep_dma_cyclic(
	struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
	size_t period_len, enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);
	enum dma_slave_buswidth dev_width;
	struct bcm2708_desc *d;
	dma_addr_t dev_addr;
	unsigned es, sync_type;
	unsigned frame;

	/* Grab configuration */
	if (direction == DMA_DEV_TO_MEM) {
		dev_addr = c->cfg.src_addr;
		dev_width = c->cfg.src_addr_width;
		sync_type = BCM2708_DMA_S_DREQ;
	} else if (direction == DMA_MEM_TO_DEV) {
		dev_addr = c->cfg.dst_addr;
		dev_width = c->cfg.dst_addr_width;
		sync_type = BCM2708_DMA_D_DREQ;
	} else {
		dev_err(chan->device->dev, "%s: bad direction?\n", __func__);
		return NULL;
	}

	/* Bus width translates to the element size (ES) */
	switch (dev_width) {
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		es = BCM2708_DMA_DATA_TYPE_S32;
		break;
	default:
		return NULL;
	}

	/* Now allocate and setup the descriptor. */
	d = kzalloc(sizeof(*d), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->dir = direction;
	d->frames = buf_len / period_len;

	/* Allocate memory for control blocks */
	d->control_block_size = d->frames * sizeof(struct bcm2708_dma_cb);
	d->control_block_base = dma_zalloc_coherent(chan->device->dev,
			d->control_block_size, &d->control_block_base_phys,
			GFP_NOWAIT);

	if (!d->control_block_base) {
		kfree(d);
		return NULL;
	}

	/*
	 * Iterate over all frames, create a control block
	 * for each frame and link them together.
	 */
	for (frame = 0; frame < d->frames; frame++) {
		struct bcm2708_dma_cb *control_block =
			&d->control_block_base[frame];

		/* Setup adresses */
		if (d->dir == DMA_DEV_TO_MEM) {
			control_block->info = BCM2708_DMA_D_INC;
			control_block->src = dev_addr;
			control_block->dst = buf_addr + frame * period_len;
		} else {
			control_block->info = BCM2708_DMA_S_INC;
			control_block->src = buf_addr + frame * period_len;
			control_block->dst = dev_addr;
		}

		/* Enable interrupt */
		control_block->info |= BCM2708_DMA_INT_EN;

		/* Setup synchronization */
		if (sync_type != 0)
			control_block->info |= sync_type;

		/* Setup DREQ channel */
		if (c->cfg.slave_id != 0)
			control_block->info |=
				BCM2708_DMA_PER_MAP(c->cfg.slave_id);

		/* Length of a frame */
		control_block->length = period_len;
		d->size += control_block->length;

		/*
		 * Next block is the next frame.
		 * This DMA engine driver currently only supports cyclic DMA.
		 * Therefore, wrap around at number of frames.
		 */
		control_block->next = d->control_block_base_phys +
			sizeof(struct bcm2708_dma_cb)
			* ((frame + 1) % d->frames);
	}

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static int bcm2708_dma_slave_config(struct bcm2708_chan *c,
		struct dma_slave_config *cfg)
{
	if ((cfg->direction == DMA_DEV_TO_MEM &&
	     cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES) ||
	    (cfg->direction == DMA_MEM_TO_DEV &&
	     cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES) ||
	    !is_slave_direction(cfg->direction)) {
		return -EINVAL;
	}

	c->cfg = *cfg;

	return 0;
}

static int bcm2708_dma_terminate_all(struct bcm2708_chan *c)
{
	struct bcm2708_dmadev *d = to_bcm2708_dma_dev(c->vc.chan.device);
	unsigned long flags;
	int timeout = 10000;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->vc.lock, flags);

	/* Prevent this channel being scheduled */
	spin_lock(&d->lock);
	list_del_init(&c->node);
	spin_unlock(&d->lock);

	/*
	 * Stop DMA activity: we assume the callback will not be called
	 * after bcm_dma_abort() returns (even if it does, it will see
	 * c->desc is NULL and exit.)
	 */
	if (c->desc) {
		c->desc = NULL;
		bcm_dma_abort(c->chan_base);

		/* Wait for stopping */
		while (timeout > 0) {
			timeout--;
			if (!(readl(c->chan_base + BCM2708_DMA_CS) &
						BCM2708_DMA_ACTIVE))
				break;

			cpu_relax();
		}

		if (timeout <= 0)
			dev_err(d->ddev.dev, "DMA transfer could not be terminated\n");
	}

	vchan_get_all_descriptors(&c->vc, &head);
	spin_unlock_irqrestore(&c->vc.lock, flags);
	vchan_dma_desc_free_list(&c->vc, &head);

	return 0;
}

static int bcm2708_dma_control(struct dma_chan *chan, enum dma_ctrl_cmd cmd,
	unsigned long arg)
{
	struct bcm2708_chan *c = to_bcm2708_dma_chan(chan);

	switch (cmd) {
	case DMA_SLAVE_CONFIG:
		return bcm2708_dma_slave_config(c,
				(struct dma_slave_config *)arg);

	case DMA_TERMINATE_ALL:
		return bcm2708_dma_terminate_all(c);

	default:
		return -ENXIO;
	}
}

static int bcm2708_dma_chan_init(struct bcm2708_dmadev *d, void __iomem* chan_base,
									int chan_id, int irq)
{
	struct bcm2708_chan *c;

	c = devm_kzalloc(d->ddev.dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->vc.desc_free = bcm2708_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);
	INIT_LIST_HEAD(&c->node);

	d->ddev.chancnt++;

	c->chan_base = chan_base;
	c->ch = chan_id;
	c->irq_number = irq;

	return 0;
}

static void bcm2708_dma_free(struct bcm2708_dmadev *od)
{
	while (!list_empty(&od->ddev.channels)) {
		struct bcm2708_chan *c = list_first_entry(&od->ddev.channels,
			struct bcm2708_chan, vc.chan.device_node);

		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}
}

static int bcm2708_dma_probe(struct platform_device *pdev)
{
	struct bcm2708_dmadev *od;
	int rc, i;

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	rc = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;
	dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));

	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	pdev->dev.dma_parms = &od->dma_parms;
	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);

	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bcm2708_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bcm2708_dma_free_chan_resources;
	od->ddev.device_tx_status = bcm2708_dma_tx_status;
	od->ddev.device_issue_pending = bcm2708_dma_issue_pending;
	od->ddev.device_prep_dma_cyclic = bcm2708_dma_prep_dma_cyclic;
	od->ddev.device_control = bcm2708_dma_control;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);
	spin_lock_init(&od->lock);

	platform_set_drvdata(pdev, od);

	for (i = 0; i < 16; i++) {
		void __iomem* chan_base;
		int chan_id, irq;

		chan_id = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST,
			&chan_base,
			&irq);

		if (chan_id < 0)
			break;

		rc = bcm2708_dma_chan_init(od, chan_base, chan_id, irq);
		if (rc) {
			bcm2708_dma_free(od);
			return rc;
		}
	}

	rc = dma_async_device_register(&od->ddev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to register slave DMA engine device: %d\n", rc);
		bcm2708_dma_free(od);
		return rc;
	}

	dev_dbg(&pdev->dev, "Load BCM2708 DMA engine driver\n");

	return rc;
}

static int bcm2708_dma_remove(struct platform_device *pdev)
{
	struct bcm2708_dmadev *od = platform_get_drvdata(pdev);

	dma_async_device_unregister(&od->ddev);
	bcm2708_dma_free(od);

	return 0;
}

static struct platform_driver bcm2708_dma_driver = {
	.probe	= bcm2708_dma_probe,
	.remove	= bcm2708_dma_remove,
	.driver = {
		.name = "bcm2708-dmaengine",
		.owner = THIS_MODULE,
	},
};

static struct platform_device *pdev;

static const struct platform_device_info bcm2708_dma_dev_info = {
	.name = "bcm2708-dmaengine",
	.id = -1,
};

static int bcm2708_dma_init(void)
{
	int rc = platform_driver_register(&bcm2708_dma_driver);

	if (rc == 0) {
		pdev = platform_device_register_full(&bcm2708_dma_dev_info);
		if (IS_ERR(pdev)) {
			platform_driver_unregister(&bcm2708_dma_driver);
			rc = PTR_ERR(pdev);
		}
	}

	return rc;
}
subsys_initcall(bcm2708_dma_init);

static void __exit bcm2708_dma_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&bcm2708_dma_driver);
}
module_exit(bcm2708_dma_exit);

MODULE_ALIAS("platform:bcm2708-dma");
MODULE_DESCRIPTION("BCM2708 DMA engine driver");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_LICENSE("GPL v2");

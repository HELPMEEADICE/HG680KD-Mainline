/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HI3798MV320_VDH_H
#define HI3798MV320_VDH_H

#include <linux/bitfield.h>
#include <linux/iopoll.h>

#define VDH_PXP0			0x20
#define VDH_IRQ_MASK			0x08
#define VDH_PXP_STATE			0x0c
#define VDH_IRQ_STATUS			0x10
#define VDH_IRQ_AUX_MASK		0x18
#define VDH_GLB_CLK_CTRL		0x04
#define VDH_GLB_RESET_REQ		0x0c
#define VDH_GLB_RESET_OK		0x14
#define VDH_GLB_CLK_SEL_MASK		GENMASK(13, 0)
#define VDH_GLB_CLK_SEL_FACTORY		0x2aaa
#define VDH_GLB_RESET_PXP		BIT(1)

#define SCD_IRQ_MASK			0x04
#define SCD_IRQ_CLEAR			0x08
#define SCD_IRQ_STATUS			0x0c
#define SMMU_CTRL			0x00
#define SMMU_IRQ_STATUS			0x28

static inline int hi3798mv320_vpu_reset(struct device *dev, void __iomem *global,
					 void __iomem *pxp)
{
	u32 mask = readl(pxp + VDH_IRQ_MASK);
	u32 aux_mask = readl(pxp + VDH_IRQ_AUX_MASK);
	u32 request = readl(global + VDH_GLB_RESET_REQ);
	u32 status, ack;
	int ret;

	if (request & VDH_GLB_RESET_PXP)
		return dev_err_probe(dev, -EBUSY, "PXP reset already pending\n");

	writel(request | VDH_GLB_RESET_PXP, global + VDH_GLB_RESET_REQ);
	ret = readl_poll_timeout(global + VDH_GLB_RESET_OK, ack,
				 ack & VDH_GLB_RESET_PXP, 30, 30000);
	writel(request, global + VDH_GLB_RESET_REQ);
	readl(global + VDH_GLB_RESET_REQ);
	if (ret)
		return dev_err_probe(dev, ret, "PXP reset acknowledgement %#08x\n",
				     ack);

	ret = readl_poll_timeout(global + VDH_GLB_RESET_OK, status,
				 !(status & VDH_GLB_RESET_PXP), 30, 30000);
	if (ret)
		return dev_err_probe(dev, ret, "PXP reset acknowledgement stuck at %#08x\n",
				     status);

	writel(mask, pxp + VDH_IRQ_MASK);
	writel(aux_mask, pxp + VDH_IRQ_AUX_MASK);
	dev_dbg(dev, "PXP reset ack=%08x deasserted-ack=%08x request-after=%08x\n",
		 ack, status, readl(global + VDH_GLB_RESET_REQ));
	return 0;
}

#endif

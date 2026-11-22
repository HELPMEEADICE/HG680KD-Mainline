// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "hi3798mv320-vdh.h"

static void hi3798mv320_vpu_snapshot(struct device *dev, void __iomem *pxp,
				    void __iomem *global, void __iomem *scd,
				    void __iomem *smmu, const char *phase)
{
	u32 clks = readl(global + VDH_GLB_CLK_CTRL);
	u32 reset = readl(global + VDH_GLB_RESET_REQ);
	u32 ack = readl(global + VDH_GLB_RESET_OK);
	u32 pxp_state = readl(pxp + VDH_PXP_STATE);
	u32 pxp_irq = readl(pxp + VDH_IRQ_STATUS);
	u32 scd_irq = readl(scd + SCD_IRQ_STATUS);
	u32 scd_mask = readl(scd + SCD_IRQ_MASK);
	u32 smmu_ctrl = readl(smmu + SMMU_CTRL);
	u32 smmu_irq = readl(smmu + SMMU_IRQ_STATUS);

	dev_info(dev, "%s: VDH clk=%08x reset=%08x ack=%08x PXP state=%08x irq=%08x SCD irq=%08x mask=%08x SMMU ctrl=%08x irq=%08x\n",
		 phase, clks, reset, ack, pxp_state, pxp_irq, scd_irq, scd_mask,
		 smmu_ctrl, smmu_irq);
}

static int hi3798mv320_vpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *vdh, *global, *scd, *smmu, *soc_id, *soc_feature;
	struct clk *clk;
	int pxp_irq, scd_irq, smmu_irq, ret;
	u32 val;

	vdh = devm_platform_ioremap_resource_byname(pdev, "vdh");
	if (IS_ERR(vdh))
		return PTR_ERR(vdh);
	global = devm_platform_ioremap_resource_byname(pdev, "vdh-global");
	if (IS_ERR(global))
		return PTR_ERR(global);
	scd = devm_platform_ioremap_resource_byname(pdev, "scd");
	if (IS_ERR(scd))
		return PTR_ERR(scd);
	smmu = devm_platform_ioremap_resource_byname(pdev, "smmu");
	if (IS_ERR(smmu))
		return PTR_ERR(smmu);
	soc_id = devm_platform_ioremap_resource_byname(pdev, "soc-id");
	if (IS_ERR(soc_id))
		return PTR_ERR(soc_id);
	soc_feature = devm_platform_ioremap_resource_byname(pdev, "soc-feature");
	if (IS_ERR(soc_feature))
		return PTR_ERR(soc_feature);

	pxp_irq = platform_get_irq_byname(pdev, "pxp");
	if (pxp_irq < 0)
		return pxp_irq;
	scd_irq = platform_get_irq_byname(pdev, "scd");
	if (scd_irq < 0)
		return scd_irq;
	smmu_irq = platform_get_irq_byname(pdev, "smmu");
	if (smmu_irq < 0)
		return smmu_irq;

	clk = devm_clk_get_enabled(dev, "vdec");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "could not enable VDEC CRG\n");

	dev_info(dev, "VDEC CRG enabled, Linux IRQs PXP=%d SCD=%d SMMU=%d\n",
		 pxp_irq, scd_irq, smmu_irq);
	dev_info(dev, "factory SoC identity=%08x feature=%08x\n",
		 readl(soc_id), readl(soc_feature));
	val = readl(global + VDH_GLB_CLK_CTRL);
	writel((val & ~VDH_GLB_CLK_SEL_MASK) | VDH_GLB_CLK_SEL_FACTORY,
	       global + VDH_GLB_CLK_CTRL);
	readl(global + VDH_GLB_CLK_CTRL);
	hi3798mv320_vpu_snapshot(dev, vdh + VDH_PXP0, global, scd, smmu,
				 "before reset");

	ret = hi3798mv320_vpu_reset(dev, global, vdh + VDH_PXP0);
	if (ret)
		return ret;

	/* Keep SCD idle and discard any stale interrupt. */
	writel(1, scd + SCD_IRQ_MASK);
	writel(1, scd + SCD_IRQ_CLEAR);
	hi3798mv320_vpu_snapshot(dev, vdh + VDH_PXP0, global, scd, smmu,
				 "after reset");
	return 0;
}

static const struct of_device_id hi3798mv320_vpu_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-vpu-probe" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv320_vpu_of_match);

static struct platform_driver hi3798mv320_vpu_driver = {
	.probe = hi3798mv320_vpu_probe,
	.driver = {
		.name = "hi3798mv320-vpu-probe",
		.of_match_table = hi3798mv320_vpu_of_match,
	},
};
module_platform_driver(hi3798mv320_vpu_driver);

MODULE_DESCRIPTION("Hi3798MV320 video decoder bring-up probe");
MODULE_LICENSE("GPL");

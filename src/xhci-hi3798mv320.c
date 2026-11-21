// SPDX-License-Identifier: GPL-2.0-only
/* Hi3798MV320 xHCI bring-up for the two USB2 PHYs on the host port. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "xhci-plat.h"

#define USB2_PHY0_CRG		0x368
#define USB2_PHY1_CRG		0x280
#define USB2_PHY_RESET		BIT(0)
#define USB2_PHY_PORT_RESET	BIT(1)
#define USB2_PHY_CLOCKS		GENMASK(5, 4)
#define USB2_PHY_CRG_MASK	(USB2_PHY_CLOCKS | USB2_PHY_PORT_RESET | USB2_PHY_RESET)
#define USB3_HOST_CRG		0x2d0
#define USB3_HOST_RESET		BIT(0)
#define USB3_HOST_CLOCK		BIT(18)
#define USB3_HOST_CRG_MASK	(USB3_HOST_CLOCK | USB3_HOST_RESET)
#define USB3_PERI_CTRL		0x210
#define USB3_PERI_MUX		0x220
#define USB3_PERI_STATUS	0x870
#define USB3_USB2_ROUTE		BIT(12)
#define USB3_MUX_MASK		GENMASK(9, 8)
#define DWC3_GUSB3PIPECTL0	0xc2c0
#define DWC3_SUSPEND_USB3_PHY	BIT(17)

struct hg680kd_xhci {
	struct regmap *crg;
	struct regmap *peri;
	unsigned int clock;
	unsigned int phy0;
	unsigned int phy1;
	unsigned int route;
	unsigned int mux;
};

static void hg680kd_host_restore(void *data)
{
	struct hg680kd_xhci *host = data;

	regmap_update_bits(host->crg, USB3_HOST_CRG, USB3_HOST_CRG_MASK, host->clock);
	regmap_update_bits(host->crg, USB2_PHY1_CRG, USB2_PHY_CRG_MASK, host->phy1);
	regmap_update_bits(host->crg, USB2_PHY0_CRG, USB2_PHY_CRG_MASK, host->phy0);
	regmap_update_bits(host->peri, USB3_PERI_CTRL, USB3_USB2_ROUTE, host->route);
	regmap_update_bits(host->peri, USB3_PERI_MUX, USB3_MUX_MASK, host->mux);
}

static int hg680kd_usb2_phy_start(struct regmap *crg, void __iomem *phy,
				unsigned int crg_reg, unsigned int trim, bool second)
{
	u32 value;
	int ret;

	ret = regmap_update_bits(crg, crg_reg, USB2_PHY_CRG_MASK, USB2_PHY_CRG_MASK);
	if (ret)
		return ret;
	usleep_range(20, 40);
	ret = regmap_clear_bits(crg, crg_reg, USB2_PHY_RESET);
	if (ret)
		return ret;
	usleep_range(20, 40);

	if (second)
		writel(readl(phy + 0x14) | GENMASK(1, 0), phy + 0x14);

	value = readl(phy + 0x08);
	if (trim >= 9 && trim <= 29)
		value = (value & ~GENMASK(12, 8)) | FIELD_PREP(GENMASK(12, 8), trim);
	writel((value & ~GENMASK(18, 16)) | FIELD_PREP(GENMASK(18, 16), 4), phy + 0x08);
	value = readl(phy);
	writel((value & ~0xf2f) | 0x82b, phy);
	usleep_range(500, 700);

	ret = regmap_clear_bits(crg, crg_reg, USB2_PHY_PORT_RESET);
	usleep_range(20, 40);
	return ret;
}

static int hg680kd_xhci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hg680kd_xhci *host;
	struct resource *resource;
	void __iomem *regs, *phy0, *phy1, *trim_reg;
	unsigned int status, trim;
	int ret;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(host->crg))
		return dev_err_probe(dev, PTR_ERR(host->crg), "failed to map CRG\n");
	host->peri = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,peri");
	if (IS_ERR(host->peri))
		return dev_err_probe(dev, PTR_ERR(host->peri), "failed to map peripheral control\n");

	phy0 = devm_platform_ioremap_resource_byname(pdev, "phy0");
	if (IS_ERR(phy0))
		return PTR_ERR(phy0);
	phy1 = devm_platform_ioremap_resource_byname(pdev, "phy1");
	if (IS_ERR(phy1))
		return PTR_ERR(phy1);
	trim_reg = devm_platform_ioremap_resource_byname(pdev, "trim");
	if (IS_ERR(trim_reg))
		return PTR_ERR(trim_reg);
	trim = FIELD_GET(GENMASK(6, 2), readl(trim_reg));

	ret = regmap_read(host->crg, USB3_HOST_CRG, &host->clock);
	ret = ret ?: regmap_read(host->crg, USB2_PHY0_CRG, &host->phy0);
	ret = ret ?: regmap_read(host->crg, USB2_PHY1_CRG, &host->phy1);
	ret = ret ?: regmap_read(host->peri, USB3_PERI_CTRL, &host->route);
	ret = ret ?: regmap_read(host->peri, USB3_PERI_MUX, &host->mux);
	ret = ret ?: regmap_read(host->peri, USB3_PERI_STATUS, &status);
	if (ret)
		return ret;

	/* The alternate combo-PHY route needs its own initialization. */
	if (!(status & BIT(2)) && FIELD_GET(USB3_MUX_MASK, host->mux) == 1)
		return dev_err_probe(dev, -EOPNOTSUPP, "combo-PHY route selected\n");

	ret = devm_add_action_or_reset(dev, hg680kd_host_restore, host);
	if (ret)
		return ret;
	if ((status & BIT(2)) && FIELD_GET(USB3_MUX_MASK, host->mux) == 1) {
		ret = regmap_clear_bits(host->peri, USB3_PERI_MUX, USB3_MUX_MASK);
		if (ret)
			return ret;
	}
	ret = hg680kd_usb2_phy_start(host->crg, phy0, USB2_PHY0_CRG, trim, false);
	ret = ret ?: hg680kd_usb2_phy_start(host->crg, phy1, USB2_PHY1_CRG, trim, true);
	ret = ret ?: regmap_set_bits(host->peri, USB3_PERI_CTRL, USB3_USB2_ROUTE);
	ret = ret ?: regmap_update_bits(host->crg, USB3_HOST_CRG, USB3_HOST_CRG_MASK,
				       USB3_HOST_RESET);
	if (ret)
		return ret;
	usleep_range(20, 40);
	ret = regmap_set_bits(host->crg, USB3_HOST_CRG, USB3_HOST_CLOCK);
	if (ret)
		return ret;
	usleep_range(20, 40);
	ret = regmap_clear_bits(host->crg, USB3_HOST_CRG, USB3_HOST_RESET);
	if (ret)
		return ret;
	usleep_range(20, 40);

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) < DWC3_GUSB3PIPECTL0 + sizeof(u32))
		return -EINVAL;
	regs = devm_ioremap(dev, resource->start, DWC3_GUSB3PIPECTL0 + sizeof(u32));
	if (!regs)
		return -ENOMEM;
	writel(readl(regs + DWC3_GUSB3PIPECTL0) & ~DWC3_SUSPEND_USB3_PHY,
	       regs + DWC3_GUSB3PIPECTL0);

	return xhci_plat_probe(pdev, dev, NULL);
}

static const struct of_device_id hg680kd_xhci_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-xhci" },
	{ }
};
MODULE_DEVICE_TABLE(of, hg680kd_xhci_of_match);

static struct platform_driver hg680kd_xhci_driver = {
	.probe = hg680kd_xhci_probe,
	.remove = xhci_plat_remove,
	.driver = {
		.name = "xhci-hi3798mv320",
		.of_match_table = hg680kd_xhci_of_match,
	},
};
module_platform_driver(hg680kd_xhci_driver);

MODULE_DESCRIPTION("Hi3798MV320 USB host bring-up");
MODULE_LICENSE("GPL");

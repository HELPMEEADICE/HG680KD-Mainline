// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only bring-up host for the Hi3798MV320 Nebula eMMC and SDIO controllers.
 * The stock bootloader selects the pad functions.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include "sdhci-pltfm.h"

#define NEBULA_EMMC_CONTROL	0x52c
#define NEBULA_EMMC_RESET	0x534
#define NEBULA_SAMPLE_CTRL	0x540
#define NEBULA_SAMPLE_PHASE	0x544
#define NEBULA_CARD_IS_EMMC	BIT(0)
#define NEBULA_ENHANCED_STROBE	BIT(8)
#define NEBULA_SAMPLE_SW_EN	BIT(4)
#define NEBULA_SAMPLE_PHASE_MASK	GENMASK(7, 0)

#define NEBULA_CRG_EMMC		0x288
#define NEBULA_CRG_EMMC_PHASE	0x290
#define NEBULA_CRG_SDIO		0x2a4
#define NEBULA_CRG_SDIO_DLL	0x2a8
#define NEBULA_CRG_SDIO_PHASE	0x2ac
#define NEBULA_CRG_RATE_SHIFT	24
#define NEBULA_CRG_RATE_SEL	GENMASK(26, 24)
#define NEBULA_CRG_DRV_PHASE_MASK	GENMASK(19, 15)
#define NEBULA_CRG_DRV_PHASE_LEGACY	(16 << 15)
#define NEBULA_CRG_SDIO_RESETS	GENMASK(18, 16)
#define NEBULA_CRG_SDIO_CLOCKS	GENMASK(1, 0)
#define NEBULA_CRG_SDIO_DLL_RESET BIT(1)

struct hi3798mv320_sdhci_priv {
	struct regmap *crg;
	unsigned int clock_reg;
	unsigned int phase_reg;
	bool emmc;
};

static unsigned int hi3798mv320_sdhci_get_min_clock(struct sdhci_host *host)
{
	return 400000;
}

static void hi3798mv320_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct hi3798mv320_sdhci_priv *priv = sdhci_pltfm_priv(pltfm);
	unsigned int sel, actual;
	u16 clk_ctrl;
	u32 val;

	host->mmc->actual_clock = 0;
	clk_ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk_ctrl &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk_ctrl, SDHCI_CLOCK_CONTROL);
	udelay(100);
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (!clock)
		return;

	if (clock >= 192000000) {
		sel = 6;
		actual = 192000000;
	} else if (clock >= 168000000) {
		sel = 5;
		actual = 168000000;
	} else if (clock >= 144000000) {
		sel = 4;
		actual = 144000000;
	} else if (clock >= 96000000) {
		sel = 3;
		actual = 96000000;
	} else if (clock >= 48000000) {
		sel = 2;
		actual = 48000000;
	} else if (clock >= 25000000) {
		sel = 1;
		actual = 25000000;
	} else {
		sel = 0;
		actual = 400000;
	}

	regmap_update_bits(priv->crg, priv->clock_reg, NEBULA_CRG_RATE_SEL,
			   sel << NEBULA_CRG_RATE_SHIFT);

	regmap_update_bits(priv->crg, priv->phase_reg, NEBULA_CRG_DRV_PHASE_MASK,
			   NEBULA_CRG_DRV_PHASE_LEGACY);

	val = sdhci_readl(host, NEBULA_SAMPLE_CTRL);
	val |= NEBULA_SAMPLE_SW_EN;
	sdhci_writel(host, val, NEBULA_SAMPLE_CTRL);

	val = sdhci_readl(host, NEBULA_SAMPLE_PHASE);
	val &= ~NEBULA_SAMPLE_PHASE_MASK;
	if (priv->emmc && (host->mmc->ios.timing == MMC_TIMING_MMC_HS ||
			   actual >= 48000000))
		val |= 0x04;
	sdhci_writel(host, val, NEBULA_SAMPLE_PHASE);
	udelay(25);

	sdhci_enable_clk(host, SDHCI_CLOCK_PLL_EN);
	host->mmc->actual_clock = actual;
}

static void hi3798mv320_sdhci_hw_reset(struct sdhci_host *host)
{
	u16 clock = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	u16 control = sdhci_readw(host, NEBULA_EMMC_CONTROL);

	/*
	 * The firmware leaves the card in HS400 Enhanced Strobe, where CMD
	 * responses can use DS. Reset it before sending legacy commands.
	 */
	sdhci_writew(host, clock & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	control |= NEBULA_CARD_IS_EMMC;
	control &= ~NEBULA_ENHANCED_STROBE;
	sdhci_writew(host, control, NEBULA_EMMC_CONTROL);
	sdhci_writel(host, 0, NEBULA_EMMC_RESET);
	usleep_range(1000, 1500);
	sdhci_writel(host, 1, NEBULA_EMMC_RESET);
	usleep_range(1000, 1500);
	sdhci_writew(host, clock, SDHCI_CLOCK_CONTROL);
}

static void hi3798mv320_sdhci_set_uhs_signaling(struct sdhci_host *host,
						unsigned int timing)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct hi3798mv320_sdhci_priv *priv = sdhci_pltfm_priv(pltfm);

	if (priv->emmc) {
		u16 control = sdhci_readw(host, NEBULA_EMMC_CONTROL);

		control |= NEBULA_CARD_IS_EMMC;
		sdhci_writew(host, control, NEBULA_EMMC_CONTROL);
	}
	sdhci_set_uhs_signaling(host, timing);
}

static const struct sdhci_ops hi3798mv320_sdhci_ops = {
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.get_min_clock = hi3798mv320_sdhci_get_min_clock,
	.set_clock = hi3798mv320_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.hw_reset = hi3798mv320_sdhci_hw_reset,
	.set_uhs_signaling = hi3798mv320_sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data hi3798mv320_sdhci_data = {
	.ops = &hi3798mv320_sdhci_ops,
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_BROKEN_HS200 | SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
};

static int hi3798mv320_sdhci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sdhci_pltfm_host *pltfm;
	struct hi3798mv320_sdhci_priv *priv;
	struct sdhci_host *host;
	struct regmap *crg;
	unsigned int value;
	unsigned int clock_reg, clock_mask;
	bool emmc;
	int ret;

	host = sdhci_pltfm_init(pdev, &hi3798mv320_sdhci_data, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm);
	pltfm->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(pltfm->clk))
		return dev_err_probe(dev, PTR_ERR(pltfm->clk), "failed to get source clock\n");

	crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(crg))
		return dev_err_probe(dev, PTR_ERR(crg), "failed to map CRG\n");
	emmc = of_device_is_compatible(dev->of_node, "hisilicon,hi3798mv320-emmc");
	clock_reg = emmc ? NEBULA_CRG_EMMC : NEBULA_CRG_SDIO;
	clock_mask = NEBULA_CRG_RATE_SEL;
	if (!emmc)
		clock_mask |= NEBULA_CRG_SDIO_RESETS;

	priv->crg = crg;
	priv->clock_reg = clock_reg;
	priv->phase_reg = emmc ? NEBULA_CRG_EMMC_PHASE : NEBULA_CRG_SDIO_PHASE;
	priv->emmc = emmc;

	if (!emmc) {
		ret = regmap_set_bits(crg, clock_reg,
				      NEBULA_CRG_SDIO_CLOCKS | NEBULA_CRG_SDIO_RESETS);
		if (ret)
			return ret;
		ret = regmap_set_bits(crg, NEBULA_CRG_SDIO_DLL, NEBULA_CRG_SDIO_DLL_RESET);
		if (ret)
			return ret;
		udelay(100);
	}
	ret = regmap_clear_bits(crg, clock_reg, clock_mask);
	if (ret)
		return ret;
	ret = regmap_read(crg, clock_reg, &value);
	if (ret)
		return ret;
	if (value & clock_mask)
		return dev_err_probe(dev, -EIO, "CRG %#x did not latch: %#x\n", clock_reg, value);
	if (!emmc)
		udelay(25);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;
	sdhci_get_property(pdev);

	if (emmc)
		host->mmc->caps2 |= MMC_CAP2_FORCE_READ_ONLY | MMC_CAP2_BOOTPART_NOACC;
	return sdhci_add_host(host);
}

static const struct of_device_id hi3798mv320_sdhci_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-emmc" },
	{ .compatible = "hisilicon,hi3798mv320-sdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv320_sdhci_of_match);

static struct platform_driver hi3798mv320_sdhci_driver = {
	.probe = hi3798mv320_sdhci_probe,
	.remove = sdhci_pltfm_remove,
	.driver = {
		.name = "sdhci-hi3798mv320",
		.of_match_table = hi3798mv320_sdhci_of_match,
	},
};
module_platform_driver(hi3798mv320_sdhci_driver);

MODULE_DESCRIPTION("Hi3798MV320 read-only eMMC bring-up");
MODULE_LICENSE("GPL");

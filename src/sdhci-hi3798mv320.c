// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only bring-up host for the Hi3798MV320 Nebula eMMC and SDIO controllers.
 * The stock bootloader selects the pad functions.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
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
#define NEBULA_CTRL_HS400	0x0007

#define NEBULA_CRG_EMMC		0x288
#define NEBULA_CRG_EMMC_DLL	0x28c
#define NEBULA_CRG_EMMC_PHASE	0x290
#define NEBULA_CRG_EMMC_STATUS	0x2a0
#define NEBULA_CRG_SDIO		0x2a4
#define NEBULA_CRG_SDIO_DLL	0x2a8
#define NEBULA_CRG_SDIO_PHASE	0x2ac
#define NEBULA_CRG_RATE_SHIFT	24
#define NEBULA_CRG_RATE_SEL	GENMASK(26, 24)
#define NEBULA_CRG_DRV_PHASE_MASK	GENMASK(19, 15)
#define NEBULA_CRG_DRV_PHASE_SHIFT	15
#define NEBULA_CRG_SDIO_RESETS	GENMASK(18, 16)
#define NEBULA_CRG_SDIO_CLOCKS	GENMASK(1, 0)
#define NEBULA_CRG_DLL_RESET	BIT(1)
#define NEBULA_CRG_P4_DLL_LOCK	BIT(9)
#define NEBULA_CRG_DS_DLL_READY	BIT(10)
#define NEBULA_CRG_SAMPL_DLL_RDY BIT(12)

#define NEBULA_IOCFG_DRIVE_MASK	0x000031f0
#define NEBULA_IOCFG_CLK	0x30
#define NEBULA_IOCFG_CMD	0x38
#define NEBULA_IOCFG_RST	0x3c
#define NEBULA_IOCFG_DS		0x40
#define NEBULA_NUM_PHASES	32

struct hi3798mv320_sdhci_priv {
	struct regmap *crg;
	struct regmap *iocfg;
	unsigned int clock_reg;
	unsigned int dll_reg;
	unsigned int phase_reg;
	unsigned int status_reg;
	unsigned int tuned_phase;
	bool emmc;
};

static const unsigned int nebula_iocfg_data_regs[8] = {
	0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c
};

static unsigned int hi3798mv320_sdhci_get_min_clock(struct sdhci_host *host)
{
	return 400000;
}

static void hi3798mv320_sdhci_wait_dll(struct sdhci_host *host,
				       struct hi3798mv320_sdhci_priv *priv,
				       u32 bit, const char *name)
{
	u32 val;
	int ret;

	if (!priv->status_reg)
		return;

	ret = regmap_read_poll_timeout(priv->crg, priv->status_reg, val,
				       val & bit, 1000, 20000);
	if (ret)
		dev_warn(mmc_dev(host->mmc), "%s DLL not ready (%#x)\n", name, val);
}

static void hi3798mv320_sdhci_set_iocfg(struct sdhci_host *host,
					struct hi3798mv320_sdhci_priv *priv,
					unsigned int timing)
{
	u32 clk_val, cmd_val, data_val, rst_val;
	int i;

	if (!priv->iocfg || !priv->emmc)
		return;

	if (timing == MMC_TIMING_MMC_HS400) {
		clk_val = 0x2020;
		cmd_val = 0x1020;
		data_val = 0x1020;
		rst_val = 0x10f0;
		regmap_update_bits(priv->iocfg, NEBULA_IOCFG_DS,
				   NEBULA_IOCFG_DRIVE_MASK, 0x20f0);
	} else if (timing == MMC_TIMING_MMC_HS200) {
		clk_val = 0x2010;
		cmd_val = 0x1020;
		data_val = 0x1020;
		rst_val = 0x10f0;
	} else {
		clk_val = 0x2020;
		cmd_val = 0x1070;
		data_val = 0x1070;
		rst_val = 0x10f0;
	}

	regmap_update_bits(priv->iocfg, NEBULA_IOCFG_CLK,
			   NEBULA_IOCFG_DRIVE_MASK, clk_val);
	regmap_update_bits(priv->iocfg, NEBULA_IOCFG_CMD,
			   NEBULA_IOCFG_DRIVE_MASK, cmd_val);
	for (i = 0; i < host->mmc->ios.bus_width && i < 8; i++)
		regmap_update_bits(priv->iocfg, nebula_iocfg_data_regs[i],
				   NEBULA_IOCFG_DRIVE_MASK, data_val);
	regmap_update_bits(priv->iocfg, NEBULA_IOCFG_RST,
			   NEBULA_IOCFG_DRIVE_MASK, rst_val);
}

static void hi3798mv320_sdhci_set_sample_phase(struct sdhci_host *host,
					       unsigned int phase)
{
	u32 val;

	val = sdhci_readl(host, NEBULA_SAMPLE_CTRL);
	val |= NEBULA_SAMPLE_SW_EN;
	sdhci_writel(host, val, NEBULA_SAMPLE_CTRL);

	val = sdhci_readl(host, NEBULA_SAMPLE_PHASE);
	val &= ~NEBULA_SAMPLE_PHASE_MASK;
	val |= phase & NEBULA_SAMPLE_PHASE_MASK;
	sdhci_writel(host, val, NEBULA_SAMPLE_PHASE);
}

static void hi3798mv320_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct hi3798mv320_sdhci_priv *priv = sdhci_pltfm_priv(pltfm);
	unsigned int timing = host->mmc->ios.timing;
	unsigned int sel, actual, drv_phase, sample_phase;
	u16 clk_ctrl;

	host->mmc->actual_clock = 0;
	clk_ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk_ctrl &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk_ctrl, SDHCI_CLOCK_CONTROL);
	udelay(25);
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

	hi3798mv320_sdhci_set_iocfg(host, priv, timing);

	regmap_update_bits(priv->crg, priv->clock_reg, NEBULA_CRG_RATE_SEL,
			   sel << NEBULA_CRG_RATE_SHIFT);

	if (priv->emmc && timing == MMC_TIMING_MMC_HS400) {
		drv_phase = 9;
		sample_phase = priv->tuned_phase;
	} else if (priv->emmc && timing == MMC_TIMING_MMC_HS200) {
		drv_phase = 20;
		sample_phase = priv->tuned_phase;
	} else if (priv->emmc && (timing == MMC_TIMING_MMC_HS || actual >= 48000000)) {
		drv_phase = 16;
		sample_phase = 4;
	} else {
		drv_phase = 16;
		sample_phase = 0;
	}

	regmap_update_bits(priv->crg, priv->phase_reg, NEBULA_CRG_DRV_PHASE_MASK,
			   drv_phase << NEBULA_CRG_DRV_PHASE_SHIFT);

	hi3798mv320_sdhci_set_sample_phase(host, sample_phase);
	udelay(25);

	clk_ctrl = SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk_ctrl, SDHCI_CLOCK_CONTROL);
	{
		ktime_t timeout = ktime_add_ms(ktime_get(), 20);

		while (!(sdhci_readw(host, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE)) {
			if (ktime_after(ktime_get(), timeout)) {
				dev_err(mmc_dev(host->mmc), "Internal clock never stabilised.\n");
				return;
			}
			udelay(10);
		}
	}

	if (priv->emmc &&
	    (timing == MMC_TIMING_MMC_HS200 || timing == MMC_TIMING_MMC_HS400)) {
		regmap_set_bits(priv->crg, priv->dll_reg, NEBULA_CRG_DLL_RESET);
		regmap_clear_bits(priv->crg, priv->dll_reg, NEBULA_CRG_DLL_RESET);
		hi3798mv320_sdhci_wait_dll(host, priv, NEBULA_CRG_P4_DLL_LOCK, "P4");
		if (timing == MMC_TIMING_MMC_HS400)
			hi3798mv320_sdhci_wait_dll(host, priv, NEBULA_CRG_DS_DLL_READY, "DS");
		hi3798mv320_sdhci_wait_dll(host, priv, NEBULA_CRG_SAMPL_DLL_RDY, "SAMPL");
	}

	clk_ctrl |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk_ctrl, SDHCI_CLOCK_CONTROL);
	udelay(75);
	host->mmc->actual_clock = actual;
}

static int hi3798mv320_sdhci_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct hi3798mv320_sdhci_priv *priv = sdhci_pltfm_priv(pltfm);
	u32 pass_mask = 0;
	int best_start = 0, best_len = 0;
	int cur_start = -1, cur_len = 0;
	int phase, best_phase;
	u16 clk_ctrl, ctrl2;

	for (phase = 0; phase < NEBULA_NUM_PHASES; phase++) {
		clk_ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		sdhci_writew(host, clk_ctrl & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
		hi3798mv320_sdhci_set_sample_phase(host, phase);
		hi3798mv320_sdhci_wait_dll(host, priv, NEBULA_CRG_SAMPL_DLL_RDY, "SAMPL");
		sdhci_writew(host, clk_ctrl | SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
		udelay(1);

		if (mmc_send_tuning(host->mmc, opcode, NULL) == 0)
			pass_mask |= BIT(phase);
	}

	if (!pass_mask) {
		dev_err(mmc_dev(host->mmc), "HS200 tuning failed: no passing phase\n");
		return -EIO;
	}

	/* Find longest contiguous run of passing phases (with wrap-around). */
	for (phase = 0; phase < NEBULA_NUM_PHASES * 2; phase++) {
		if (pass_mask & BIT(phase % NEBULA_NUM_PHASES)) {
			if (cur_start < 0)
				cur_start = phase;
			cur_len++;
			if (cur_len > NEBULA_NUM_PHASES)
				cur_len = NEBULA_NUM_PHASES;
			if (cur_len > best_len) {
				best_len = cur_len;
				best_start = cur_start;
			}
		} else {
			cur_start = -1;
			cur_len = 0;
		}
	}

	best_phase = (best_start + best_len / 2) % NEBULA_NUM_PHASES;
	priv->tuned_phase = best_phase;

	clk_ctrl = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	sdhci_writew(host, clk_ctrl & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	hi3798mv320_sdhci_set_sample_phase(host, best_phase);
	hi3798mv320_sdhci_wait_dll(host, priv, NEBULA_CRG_SAMPL_DLL_RDY, "SAMPL");
	sdhci_writew(host, clk_ctrl | SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	udelay(1);

	ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	ctrl2 |= SDHCI_CTRL_TUNED_CLK;
	sdhci_writew(host, ctrl2, SDHCI_HOST_CONTROL2);

	dev_info(mmc_dev(host->mmc), "tuning pass_mask=%#010x selected phase=%d\n",
		 pass_mask, best_phase);
	return 0;
}

static void hi3798mv320_sdhci_hw_reset(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct hi3798mv320_sdhci_priv *priv = sdhci_pltfm_priv(pltfm);
	u16 clock = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	u16 control = sdhci_readw(host, NEBULA_EMMC_CONTROL);

	/*
	 * The firmware leaves the card in HS400 Enhanced Strobe, where CMD
	 * responses can use DS. Reset it before sending legacy commands.
	 */
	priv->tuned_phase = 0;
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

	if (timing == MMC_TIMING_MMC_HS400) {
		u16 ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);

		ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
		ctrl_2 |= NEBULA_CTRL_HS400;
		sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
		return;
	}

	sdhci_set_uhs_signaling(host, timing);
}

static void hi3798mv320_sdhci_hs400_enhanced_strobe(struct mmc_host *mmc,
						    struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u16 control = sdhci_readw(host, NEBULA_EMMC_CONTROL);

	if (ios->enhanced_strobe)
		control |= NEBULA_ENHANCED_STROBE;
	else
		control &= ~NEBULA_ENHANCED_STROBE;
	sdhci_writew(host, control, NEBULA_EMMC_CONTROL);
}

static const struct sdhci_ops hi3798mv320_sdhci_ops = {
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.get_min_clock = hi3798mv320_sdhci_get_min_clock,
	.set_clock = hi3798mv320_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.hw_reset = hi3798mv320_sdhci_hw_reset,
	.set_uhs_signaling = hi3798mv320_sdhci_set_uhs_signaling,
	.platform_execute_tuning = hi3798mv320_sdhci_execute_tuning,
};

static const struct sdhci_pltfm_data hi3798mv320_sdhci_data = {
	.ops = &hi3798mv320_sdhci_ops,
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
};

static int hi3798mv320_sdhci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sdhci_pltfm_host *pltfm;
	struct hi3798mv320_sdhci_priv *priv;
	struct sdhci_host *host;
	struct regmap *crg, *iocfg = NULL;
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
	if (of_property_present(dev->of_node, "hisilicon,iocfg")) {
		iocfg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,iocfg");
		if (IS_ERR(iocfg))
			return dev_err_probe(dev, PTR_ERR(iocfg), "failed to map IOCFG\n");
	}

	emmc = of_device_is_compatible(dev->of_node, "hisilicon,hi3798mv320-emmc");
	clock_reg = emmc ? NEBULA_CRG_EMMC : NEBULA_CRG_SDIO;
	clock_mask = NEBULA_CRG_RATE_SEL;
	if (!emmc)
		clock_mask |= NEBULA_CRG_SDIO_RESETS;

	priv->crg = crg;
	priv->iocfg = iocfg;
	priv->clock_reg = clock_reg;
	priv->dll_reg = emmc ? NEBULA_CRG_EMMC_DLL : NEBULA_CRG_SDIO_DLL;
	priv->phase_reg = emmc ? NEBULA_CRG_EMMC_PHASE : NEBULA_CRG_SDIO_PHASE;
	priv->status_reg = emmc ? NEBULA_CRG_EMMC_STATUS : 0;
	priv->emmc = emmc;

	if (!emmc) {
		ret = regmap_set_bits(crg, clock_reg,
				      NEBULA_CRG_SDIO_CLOCKS | NEBULA_CRG_SDIO_RESETS);
		if (ret)
			return ret;
		ret = regmap_set_bits(crg, NEBULA_CRG_SDIO_DLL, NEBULA_CRG_DLL_RESET);
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

	if (emmc) {
		host->mmc_host_ops.hs400_enhanced_strobe =
			hi3798mv320_sdhci_hs400_enhanced_strobe;
		host->mmc->caps2 |= MMC_CAP2_FORCE_READ_ONLY | MMC_CAP2_BOOTPART_NOACC;
	}
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

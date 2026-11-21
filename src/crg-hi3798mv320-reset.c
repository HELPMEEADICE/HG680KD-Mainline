// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "reset.h"

#define CRG_GPU_CTRL		0x0d4
#define CRG_GPU_BUS_CLK_MASK	BIT(0)
#define CRG_GPU_CORE_CLK_MASK	(BIT(8) | BIT(9))
#define CRG_GPU_FREQ_SEL	0x124
#define CRG_GPU_FREQ_MASK	0x7

#define CRG_VDEC_CTRL		0x078
#define CRG_VDEC_DISABLE	BIT(0)
#define CRG_VDEC_INIT_MASK	(GENMASK(9, 8) | GENMASK(17, 12))

#define CRG_APLL_FRAC		0x000
#define CRG_APLL_DIV		0x004
#define CRG_CPU_CLK		0x048
#define CRG_PLL_LOCK		0x150
#define CRG_APLL_ADJ		0x1a4
#define CRG_APLL_ADJ_CTRL	0x1a8
#define CRG_APLL_ADJ_STEP	0x1ac
#define CRG_APLL_ADJ_FRAC	0x1b0
#define CRG_APLL_ADJ_STATUS	0x1b4
#define CRG_CPU_AUX_DIV		0x4b0

#define APLL_FRAC_MASK		GENMASK(23, 0)
#define APLL_POSTDIV1		GENMASK(26, 24)
#define APLL_POSTDIV2		GENMASK(30, 28)
#define APLL_FBDIV		GENMASK(11, 0)
#define APLL_REFDIV		GENMASK(17, 12)
#define APLL_INTEGER_MODE	BIT(25)
#define APLL_REF_HZ		24000000ULL
#define APLL_FRAC_SCALE		BIT_ULL(24)
#define CPU_MUX_MASK		(GENMASK(21, 19) | GENMASK(2, 0))
#define CPU_DIV_MASK		(GENMASK(23, 22) | GENMASK(4, 3))
#define CPU_SAFE_MUX		(BIT(21) | BIT(2))
#define CPU_SAFE_DIV		(BIT(22) | BIT(3))
#define CPU_SAFE_RATE		200000000

static const unsigned long hi3798mv320_cpu_rates[] = {
	CPU_SAFE_RATE, 400000000, 600000000, 800000000, 1000000000,
};

static const unsigned long hi3798mv320_gpu_rates[] = {
	540000000, 400000000, 712000000, 0,
	300000000, 675000000, 460000000, 200000000,
};

struct hi3798mv320_gpu_clk {
	struct clk_hw hw;
	void __iomem *base;
	spinlock_t *lock;
	u32 mask;
};

struct hi3798mv320_crg_priv {
	struct hisi_reset_controller *reset;
	void __iomem *base;
	spinlock_t lock;
	struct clk_hw_onecell_data *clk_data;
	struct hi3798mv320_gpu_clk clks[2];
	struct clk_hw cpu_clk;
	struct clk_hw vdec_clk;
};

static struct hi3798mv320_gpu_clk *to_gpu_clk(struct clk_hw *hw)
{
	return container_of(hw, struct hi3798mv320_gpu_clk, hw);
}

static int hi3798mv320_gpu_clk_enable(struct clk_hw *hw)
{
	struct hi3798mv320_gpu_clk *clk = to_gpu_clk(hw);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(clk->lock, flags);
	val = readl(clk->base + CRG_GPU_CTRL);
	val |= clk->mask;
	writel(val, clk->base + CRG_GPU_CTRL);
	spin_unlock_irqrestore(clk->lock, flags);
	return 0;
}

static void hi3798mv320_gpu_clk_disable(struct clk_hw *hw)
{
	struct hi3798mv320_gpu_clk *clk = to_gpu_clk(hw);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(clk->lock, flags);
	val = readl(clk->base + CRG_GPU_CTRL);
	val &= ~clk->mask;
	writel(val, clk->base + CRG_GPU_CTRL);
	spin_unlock_irqrestore(clk->lock, flags);
}

static int hi3798mv320_gpu_clk_is_enabled(struct clk_hw *hw)
{
	struct hi3798mv320_gpu_clk *clk = to_gpu_clk(hw);

	return (readl(clk->base + CRG_GPU_CTRL) & clk->mask) == clk->mask;
}

static unsigned long hi3798mv320_gpu_clk_recalc_rate(struct clk_hw *hw,
						     unsigned long parent_rate)
{
	struct hi3798mv320_gpu_clk *clk = to_gpu_clk(hw);
	u32 sel = readl(clk->base + CRG_GPU_FREQ_SEL) & CRG_GPU_FREQ_MASK;

	return hi3798mv320_gpu_rates[sel] ?: 200000000;
}

static const struct clk_ops hi3798mv320_gpu_clk_ops = {
	.enable = hi3798mv320_gpu_clk_enable,
	.disable = hi3798mv320_gpu_clk_disable,
	.is_enabled = hi3798mv320_gpu_clk_is_enabled,
	.recalc_rate = hi3798mv320_gpu_clk_recalc_rate,
};

static int hi3798mv320_gpu_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	if (req->rate != 200000000 && req->rate != 300000000)
		return -EINVAL;
	return 0;
}

static int hi3798mv320_gpu_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct hi3798mv320_gpu_clk *clk = to_gpu_clk(hw);
	u32 select;

	switch (rate) {
	case 200000000:
		select = 7;
		break;
	case 300000000:
		select = 4;
		break;
	default:
		return -EINVAL;
	}

	writel((readl(clk->base + CRG_GPU_FREQ_SEL) & ~CRG_GPU_FREQ_MASK) | select,
	       clk->base + CRG_GPU_FREQ_SEL);
	return 0;
}

static const struct clk_ops hi3798mv320_gpu_dvfs_ops = {
	.enable = hi3798mv320_gpu_clk_enable,
	.disable = hi3798mv320_gpu_clk_disable,
	.is_enabled = hi3798mv320_gpu_clk_is_enabled,
	.recalc_rate = hi3798mv320_gpu_clk_recalc_rate,
	.determine_rate = hi3798mv320_gpu_determine_rate,
	.set_rate = hi3798mv320_gpu_set_rate,
};

static void hi3798mv320_crg_update(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((readl(base + reg) & ~mask) | val, base + reg);
}

static int hi3798mv320_vdec_enable(struct clk_hw *hw)
{
	struct hi3798mv320_crg_priv *priv =
		container_of(hw, struct hi3798mv320_crg_priv, vdec_clk);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	hi3798mv320_crg_update(priv->base, CRG_VDEC_CTRL,
			       CRG_VDEC_DISABLE | CRG_VDEC_INIT_MASK, 0);
	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

static void hi3798mv320_vdec_disable(struct clk_hw *hw)
{
	struct hi3798mv320_crg_priv *priv =
		container_of(hw, struct hi3798mv320_crg_priv, vdec_clk);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	hi3798mv320_crg_update(priv->base, CRG_VDEC_CTRL,
			       CRG_VDEC_DISABLE, CRG_VDEC_DISABLE);
	spin_unlock_irqrestore(&priv->lock, flags);
}

static int hi3798mv320_vdec_is_enabled(struct clk_hw *hw)
{
	struct hi3798mv320_crg_priv *priv =
		container_of(hw, struct hi3798mv320_crg_priv, vdec_clk);

	return !(readl(priv->base + CRG_VDEC_CTRL) & CRG_VDEC_DISABLE);
}

static const struct clk_ops hi3798mv320_vdec_clk_ops = {
	.enable = hi3798mv320_vdec_enable,
	.disable = hi3798mv320_vdec_disable,
	.is_enabled = hi3798mv320_vdec_is_enabled,
};

static unsigned int hi3798mv320_apll_divisor(u32 frac, u32 div)
{
	return FIELD_GET(APLL_POSTDIV1, frac) * FIELD_GET(APLL_POSTDIV2, frac) *
	       FIELD_GET(APLL_REFDIV, div);
}

static unsigned long hi3798mv320_cpu_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct hi3798mv320_crg_priv *priv =
		container_of(hw, struct hi3798mv320_crg_priv, cpu_clk);
	void __iomem *base = priv->base;
	u32 mux = readl(base + CRG_CPU_CLK) & GENMASK(4, 0);
	u32 frac = readl(base + CRG_APLL_FRAC);
	u32 div = readl(base + CRG_APLL_DIV);
	unsigned int divisor = hi3798mv320_apll_divisor(frac, div);
	u64 multiplier = FIELD_GET(APLL_FBDIV, div) * APLL_FRAC_SCALE;

	if (mux == (BIT(2) | BIT(3)))
		return CPU_SAFE_RATE;
	if ((mux & GENMASK(2, 0)) || !divisor)
		return 0;
	if (!(div & APLL_INTEGER_MODE))
		multiplier += FIELD_GET(APLL_FRAC_MASK, frac);
	return DIV64_U64_ROUND_CLOSEST(APLL_REF_HZ * multiplier,
				       divisor * APLL_FRAC_SCALE);
}

static int hi3798mv320_cpu_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hi3798mv320_cpu_rates); i++)
		if (req->rate == hi3798mv320_cpu_rates[i])
			return 0;
	return -EINVAL;
}

static void hi3798mv320_cpu_park(void __iomem *base)
{
	hi3798mv320_crg_update(base, CRG_CPU_CLK, CPU_DIV_MASK, CPU_SAFE_DIV);
	udelay(1);
	hi3798mv320_crg_update(base, CRG_CPU_CLK, CPU_MUX_MASK, CPU_SAFE_MUX);
	udelay(1);
	hi3798mv320_crg_update(base, CRG_CPU_AUX_DIV, GENMASK(22, 20), 7 << 20);
	hi3798mv320_crg_update(base, CRG_CPU_CLK, GENMASK(8, 6), 5 << 6);
}

static int hi3798mv320_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct hi3798mv320_crg_priv *priv =
		container_of(hw, struct hi3798mv320_crg_priv, cpu_clk);
	void __iomem *base = priv->base;
	u32 frac = readl(base + CRG_APLL_FRAC);
	u32 div = readl(base + CRG_APLL_DIV);
	unsigned int divisor = hi3798mv320_apll_divisor(frac, div);
	u32 status, bus_div, hpm_mux;
	u64 multiplier;
	int ret;

	switch (rate) {
	case CPU_SAFE_RATE:
		hi3798mv320_cpu_park(base);
		return 0;
	case 400000000:
		bus_div = 0;
		hpm_mux = 5;
		break;
	case 600000000:
		bus_div = 3;
		hpm_mux = 7;
		break;
	case 800000000:
		bus_div = 2;
		hpm_mux = 7;
		break;
	case 1000000000:
		bus_div = 0;
		hpm_mux = 7;
		break;
	default:
		return -EINVAL;
	}

	if (!divisor || !(readl(base + CRG_PLL_LOCK) & BIT(0)))
		return -EIO;
	if (divisor > div64_u64(U64_MAX, rate * APLL_FRAC_SCALE))
		return -ERANGE;
	multiplier = DIV64_U64_ROUND_CLOSEST(rate * APLL_FRAC_SCALE * divisor,
					    APLL_REF_HZ);
	if ((multiplier >> 24) > FIELD_MAX(APLL_FBDIV) ||
	    ((div & APLL_INTEGER_MODE) && (multiplier & APLL_FRAC_MASK)))
		return -EINVAL;

	/* Keep the cluster on the factory 200 MHz source during PLL adjustment. */
	hi3798mv320_cpu_park(base);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ, BIT(16) | BIT(20), 0);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ_CTRL, GENMASK(9, 8), 0);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ_CTRL, BIT(4) | BIT(0), BIT(4));
	hi3798mv320_crg_update(base, CRG_APLL_ADJ_CTRL, BIT(0), BIT(0));
	hi3798mv320_crg_update(base, CRG_APLL_ADJ_CTRL, BIT(4), 0);
	writel(0, base + CRG_APLL_ADJ_STEP);
	writel(0xaaaaa, base + CRG_APLL_ADJ_FRAC);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ, GENMASK(3, 0), 2);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ, BIT(20), BIT(20));
	udelay(1);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ_CTRL, GENMASK(9, 8), BIT(8));
	hi3798mv320_crg_update(base, CRG_APLL_DIV, APLL_FBDIV, multiplier >> 24);
	if (!(div & APLL_INTEGER_MODE))
		hi3798mv320_crg_update(base, CRG_APLL_FRAC, APLL_FRAC_MASK,
				      multiplier & APLL_FRAC_MASK);
	hi3798mv320_crg_update(base, CRG_APLL_ADJ, BIT(16), BIT(16));
	ret = readl_poll_timeout(base + CRG_APLL_ADJ_STATUS, status,
				 status & BIT(0), 20, 400);
	if (ret || !(readl(base + CRG_PLL_LOCK) & BIT(0))) {
		pr_err_ratelimited("Hi3798MV320 APLL did not lock at %lu Hz\n", rate);
		return -ETIMEDOUT;
	}

	hi3798mv320_crg_update(base, CRG_CPU_CLK, CPU_MUX_MASK, 0);
	udelay(1);
	if (rate == 400000000)
		hi3798mv320_crg_update(base, CRG_CPU_AUX_DIV, GENMASK(22, 20), 6 << 20);
	if (hpm_mux == 7)
		hi3798mv320_crg_update(base, CRG_CPU_CLK, GENMASK(10, 9), bus_div << 9);
	udelay(1);
	hi3798mv320_crg_update(base, CRG_CPU_CLK, GENMASK(8, 6), hpm_mux << 6);
	return 0;
}

static const struct clk_ops hi3798mv320_cpu_clk_ops = {
	.recalc_rate = hi3798mv320_cpu_recalc_rate,
	.determine_rate = hi3798mv320_cpu_determine_rate,
	.set_rate = hi3798mv320_cpu_set_rate,
};

static int hi3798mv320_register_clks(struct platform_device *pdev,
				    struct hi3798mv320_crg_priv *priv)
{
	static const struct {
		const char *name;
		u32 mask;
	} defs[2] = {
		{ "gpu_bus", CRG_GPU_BUS_CLK_MASK },
		{ "gpu_core", CRG_GPU_CORE_CLK_MASK },
	};
	struct device *dev = &pdev->dev;
	bool dvfs = of_device_is_compatible(dev->of_node, "hisilicon,hi3798mv320-crg-dvfs");
	bool vdec = of_device_is_compatible(dev->of_node, "hisilicon,hi3798mv320-crg-vpu-probe");
	unsigned int num_clks = vdec ? 4 : dvfs ? 3 : 2;
	struct resource *res;
	int i, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	/* hisi_reset_init() has already requested this same device resource. */
	priv->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!priv->base)
		return -ENOMEM;

	priv->clk_data = devm_kzalloc(dev, struct_size(priv->clk_data, hws, num_clks),
				      GFP_KERNEL);
	if (!priv->clk_data)
		return -ENOMEM;
	priv->clk_data->num = num_clks;

	for (i = 0; i < 2; i++) {
		struct clk_init_data init = {
			.name = defs[i].name,
			.ops = dvfs && i == 1 ? &hi3798mv320_gpu_dvfs_ops :
					       &hi3798mv320_gpu_clk_ops,
		};

		priv->clks[i].base = priv->base;
		priv->clks[i].lock = &priv->lock;
		priv->clks[i].mask = defs[i].mask;
		priv->clks[i].hw.init = &init;

		ret = devm_clk_hw_register(dev, &priv->clks[i].hw);
		if (ret)
			return ret;
		priv->clk_data->hws[i] = &priv->clks[i].hw;
	}
	if (dvfs) {
		const struct clk_init_data init = {
			.name = "cpu_cluster",
			.ops = &hi3798mv320_cpu_clk_ops,
			.flags = CLK_GET_RATE_NOCACHE,
		};

		priv->cpu_clk.init = &init;
		ret = devm_clk_hw_register(dev, &priv->cpu_clk);
		if (ret)
			return ret;
		priv->clk_data->hws[2] = &priv->cpu_clk;
	}
	if (vdec) {
		const struct clk_init_data init = {
			.name = "vdec",
			.ops = &hi3798mv320_vdec_clk_ops,
		};

		priv->vdec_clk.init = &init;
		ret = devm_clk_hw_register(dev, &priv->vdec_clk);
		if (ret)
			return ret;
		priv->clk_data->hws[3] = &priv->vdec_clk;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, priv->clk_data);
}

static int hi3798mv320_reset_probe(struct platform_device *pdev)
{
	struct hi3798mv320_crg_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->lock);
	priv->reset = hisi_reset_init(pdev);
	if (!priv->reset)
		return -ENODEV;

	if (of_property_present(pdev->dev.of_node, "#clock-cells")) {
		ret = hi3798mv320_register_clks(pdev, priv);
		if (ret) {
			hisi_reset_exit(priv->reset);
			return ret;
		}
	}

	platform_set_drvdata(pdev, priv);
	return 0;
}

static void hi3798mv320_reset_remove(struct platform_device *pdev)
{
	struct hi3798mv320_crg_priv *priv = platform_get_drvdata(pdev);

	hisi_reset_exit(priv->reset);
}

static const struct of_device_id hi3798mv320_reset_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-crg" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv320_reset_of_match);

static struct platform_driver hi3798mv320_reset_driver = {
	.probe = hi3798mv320_reset_probe,
	.remove = hi3798mv320_reset_remove,
	.driver = {
		.name = "hi3798mv320-reset",
		.of_match_table = hi3798mv320_reset_of_match,
	},
};
module_platform_driver(hi3798mv320_reset_driver);

MODULE_DESCRIPTION("Hi3798MV320 reset and clock controller");
MODULE_LICENSE("GPL");

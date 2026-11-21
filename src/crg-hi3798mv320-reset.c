// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "reset.h"

#define CRG_GPU_CTRL		0x0d4
#define CRG_GPU_BUS_CLK_MASK	BIT(0)
#define CRG_GPU_CORE_CLK_MASK	(BIT(8) | BIT(9))
#define CRG_GPU_FREQ_SEL	0x124
#define CRG_GPU_FREQ_MASK	0x7

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

	switch (sel) {
	case 7:
		return 200000000;
	case 4:
		return 300000000;
	case 1:
		return 400000000;
	case 6:
		return 460000000;
	case 0:
		return 540000000;
	case 5:
		return 675000000;
	case 2:
		return 712000000;
	default:
		return 200000000;
	}
}

static const struct clk_ops hi3798mv320_gpu_clk_ops = {
	.enable = hi3798mv320_gpu_clk_enable,
	.disable = hi3798mv320_gpu_clk_disable,
	.is_enabled = hi3798mv320_gpu_clk_is_enabled,
	.recalc_rate = hi3798mv320_gpu_clk_recalc_rate,
};

static int hi3798mv320_register_gpu_clks(struct platform_device *pdev,
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
	int i, ret;

	priv->base = of_iomap(dev->of_node, 0);
	if (!priv->base)
		return -ENOMEM;

	priv->clk_data = devm_kzalloc(dev, struct_size(priv->clk_data, hws, 2),
				      GFP_KERNEL);
	if (!priv->clk_data) {
		iounmap(priv->base);
		priv->base = NULL;
		return -ENOMEM;
	}
	priv->clk_data->num = 2;

	for (i = 0; i < 2; i++) {
		struct clk_init_data init = {
			.name = defs[i].name,
			.ops = &hi3798mv320_gpu_clk_ops,
		};

		priv->clks[i].base = priv->base;
		priv->clks[i].lock = &priv->lock;
		priv->clks[i].mask = defs[i].mask;
		priv->clks[i].hw.init = &init;

		ret = devm_clk_hw_register(dev, &priv->clks[i].hw);
		if (ret) {
			iounmap(priv->base);
			priv->base = NULL;
			return ret;
		}
		priv->clk_data->hws[i] = &priv->clks[i].hw;
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					  priv->clk_data);
	if (ret) {
		iounmap(priv->base);
		priv->base = NULL;
	}
	return ret;
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
		ret = hi3798mv320_register_gpu_clks(pdev, priv);
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

	if (priv->base)
		iounmap(priv->base);
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

MODULE_DESCRIPTION("Hi3798MV320 reset and GPU clock controller");
MODULE_LICENSE("GPL");

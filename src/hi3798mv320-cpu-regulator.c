// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define CPU_PWM_DUTY		GENMASK(31, 16)
#define CPU_PWM_PERIOD		GENMASK(15, 0)
#define CPU_PWM_COUNTS		221
#define CPU_HPM			GENMASK(31, 16)
#define CPU_MIN_UV		750000
#define CPU_MAX_UV		1200000
#define CPU_LOW_OPP_UV		1050000
#define CPU_HIGH_OPP_UV		1100000
#define CPU_HPM_MIN		256

struct hi3798mv320_cpu_regulator {
	void __iomem *pwm;
	struct clk *clk;
};

static int hi3798mv320_pwm_voltage(u32 duty)
{
	return CPU_MAX_UV - (CPU_MAX_UV - CPU_MIN_UV) * duty / CPU_PWM_COUNTS;
}

static int hi3798mv320_get_voltage(struct regulator_dev *rdev)
{
	struct hi3798mv320_cpu_regulator *rail = rdev_get_drvdata(rdev);
	u32 pwm = readl(rail->pwm);
	u32 duty = FIELD_GET(CPU_PWM_DUTY, pwm);

	if (FIELD_GET(CPU_PWM_PERIOD, pwm) != CPU_PWM_COUNTS || duty > CPU_PWM_COUNTS)
		return -EIO;
	return hi3798mv320_pwm_voltage(duty);
}

static int hi3798mv320_cpu_list_voltage(struct regulator_dev *rdev, unsigned int sel)
{
	return hi3798mv320_pwm_voltage(CPU_PWM_COUNTS - sel);
}

static int hi3798mv320_cpu_set_voltage(struct regulator_dev *rdev,
				     int min_uv, int max_uv, unsigned int *selector)
{
	struct hi3798mv320_cpu_regulator *rail = rdev_get_drvdata(rdev);
	u32 pwm = readl(rail->pwm);
	u32 duty;
	unsigned long rate = clk_get_rate(rail->clk);
	int uv;

	if (min_uv < CPU_LOW_OPP_UV || max_uv > CPU_MAX_UV || min_uv > max_uv)
		return -EINVAL;
	if (FIELD_GET(CPU_PWM_PERIOD, pwm) != CPU_PWM_COUNTS ||
	    FIELD_GET(CPU_PWM_DUTY, pwm) > CPU_PWM_COUNTS)
		return -EIO;

	duty = (CPU_MAX_UV - min_uv) * CPU_PWM_COUNTS / (CPU_MAX_UV - CPU_MIN_UV);
	uv = hi3798mv320_pwm_voltage(duty);
	if (uv > max_uv)
		return -EINVAL;
	/* The CCF can discard set_rate errors. Check hardware before lowering. */
	if (uv < hi3798mv320_pwm_voltage(FIELD_GET(CPU_PWM_DUTY, pwm)) &&
	    (!rate || (rate > 600000000 && uv < CPU_HIGH_OPP_UV)))
		return -EBUSY;
	if (duty != FIELD_GET(CPU_PWM_DUTY, pwm)) {
		writel((pwm & CPU_PWM_PERIOD) | FIELD_PREP(CPU_PWM_DUTY, duty), rail->pwm);
		if (FIELD_GET(CPU_PWM_DUTY, readl(rail->pwm)) != duty)
			return -EIO;
		usleep_range(100, 150);
	}
	*selector = CPU_PWM_COUNTS - duty;
	return 0;
}

static const struct regulator_ops hi3798mv320_cpu_regulator_ops = {
	.get_voltage = hi3798mv320_get_voltage,
	.set_voltage = hi3798mv320_cpu_set_voltage,
	.list_voltage = hi3798mv320_cpu_list_voltage,
};

static const struct regulator_desc hi3798mv320_cpu_regulator_desc = {
	.name = "hi3798mv320-cpu",
	.ops = &hi3798mv320_cpu_regulator_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.n_voltages = CPU_PWM_COUNTS + 1,
};

static const struct regulator_ops hi3798mv320_dvdd_ops = {
	.get_voltage = hi3798mv320_get_voltage,
};

static const struct regulator_desc hi3798mv320_dvdd_desc = {
	.name = "hi3798mv320-dvdd",
	.ops = &hi3798mv320_dvdd_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
};

static int hi3798mv320_cpu_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi3798mv320_cpu_regulator *rail;
	struct regulator_config cfg = { .dev = dev, .of_node = dev->of_node };
	struct regulator_init_data *init_data;
	bool cpu = of_device_is_compatible(dev->of_node, "hisilicon,hi3798mv320-cpu-regulator");
	const struct regulator_desc *desc = cpu ? &hi3798mv320_cpu_regulator_desc :
						  &hi3798mv320_dvdd_desc;
	void __iomem *hpm;
	u32 pwm;

	rail = devm_kzalloc(dev, sizeof(*rail), GFP_KERNEL);
	if (!rail)
		return -ENOMEM;
	rail->pwm = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rail->pwm))
		return PTR_ERR(rail->pwm);
	pwm = readl(rail->pwm);
	if (FIELD_GET(CPU_PWM_PERIOD, pwm) != CPU_PWM_COUNTS ||
	    FIELD_GET(CPU_PWM_DUTY, pwm) > CPU_PWM_COUNTS)
		return dev_err_probe(dev, -ENODEV, "unexpected boot PWM state\n");
	cfg.driver_data = rail;
	init_data = of_get_regulator_init_data(dev, dev->of_node, desc);
	if (!init_data)
		return -EINVAL;
	if (cpu) {
		hpm = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(hpm))
			return PTR_ERR(hpm);
		if (FIELD_GET(CPU_HPM, readl(hpm)) < CPU_HPM_MIN)
			return dev_err_probe(dev, -ENODEV, "CPU HPM outside validated range\n");
		rail->clk = devm_clk_get(dev, NULL);
		if (IS_ERR(rail->clk))
			return dev_err_probe(dev, PTR_ERR(rail->clk), "cannot get CPU clock\n");
	} else {
		/* The GPU shares this rail with SoC logic. Only expose boot readback. */
		init_data->constraints.valid_ops_mask &= ~REGULATOR_CHANGE_VOLTAGE;
		init_data->constraints.apply_uV = false;
	}
	cfg.init_data = init_data;
	return PTR_ERR_OR_ZERO(devm_regulator_register(dev, desc, &cfg));
}

static const struct of_device_id hi3798mv320_cpu_regulator_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-cpu-regulator" },
	{ .compatible = "hisilicon,hi3798mv320-dvdd" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv320_cpu_regulator_of_match);

static struct platform_driver hi3798mv320_cpu_regulator_driver = {
	.probe = hi3798mv320_cpu_regulator_probe,
	.driver = {
		.name = "hi3798mv320-cpu-regulator",
		.of_match_table = hi3798mv320_cpu_regulator_of_match,
	},
};
module_platform_driver(hi3798mv320_cpu_regulator_driver);

MODULE_DESCRIPTION("Hi3798MV320 CPU PWM regulator and DVDD readback");
MODULE_LICENSE("GPL");

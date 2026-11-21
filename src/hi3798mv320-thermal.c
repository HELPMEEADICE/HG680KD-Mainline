// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/thermal.h>

#define TSENSOR_CTRL		0x00
#define TSENSOR_CONTINUOUS	0x72
#define TSENSOR_DATA		0x08
#define TSENSOR_DATA_WORDS	4
#define TSENSOR_ADC0		GENMASK(9, 0)
#define TSENSOR_ADC1		GENMASK(25, 16)

static int hi3798mv320_get_temp(struct thermal_zone_device *zone, int *temp)
{
	void __iomem *base = thermal_zone_device_priv(zone);
	int i, sum = 0;

	for (i = 0; i < TSENSOR_DATA_WORDS; i++) {
		u32 data = readl(base + TSENSOR_DATA + i * sizeof(u32));
		unsigned int adc0 = FIELD_GET(TSENSOR_ADC0, data);
		unsigned int adc1 = FIELD_GET(TSENSOR_ADC1, data);

		if (!adc0 || !adc1)
			return -EAGAIN;
		sum += adc0 + adc1;
	}

	/* The factory driver truncates the average and the degrees separately. */
	*temp = (((sum / 8 - 132) * 165) / 808 - 40) * 1000;
	return 0;
}

static const struct thermal_zone_device_ops hi3798mv320_thermal_ops = {
	.get_temp = hi3798mv320_get_temp,
};

static int hi3798mv320_thermal_resume(struct device *dev)
{
	void __iomem *base = dev_get_drvdata(dev);

	writel(TSENSOR_CONTINUOUS, base + TSENSOR_CTRL);
	return 0;
}

static int hi3798mv320_thermal_probe(struct platform_device *pdev)
{
	struct thermal_zone_device *zone;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	platform_set_drvdata(pdev, base);
	hi3798mv320_thermal_resume(&pdev->dev);
	zone = devm_thermal_of_zone_register(&pdev->dev, 0, base,
					    &hi3798mv320_thermal_ops);
	return PTR_ERR_OR_ZERO(zone);
}

static DEFINE_SIMPLE_DEV_PM_OPS(hi3798mv320_thermal_pm, NULL,
				 hi3798mv320_thermal_resume);

static const struct of_device_id hi3798mv320_thermal_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-tsensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv320_thermal_of_match);

static struct platform_driver hi3798mv320_thermal_driver = {
	.probe = hi3798mv320_thermal_probe,
	.driver = {
		.name = "hi3798mv320-tsensor",
		.of_match_table = hi3798mv320_thermal_of_match,
		.pm = pm_sleep_ptr(&hi3798mv320_thermal_pm),
	},
};
module_platform_driver(hi3798mv320_thermal_driver);

MODULE_DESCRIPTION("Hi3798MV320 temperature sensor");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <drm/drm_drv.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/of_platform.h>

#include "apq_drv.h"

u32 apq_readl(const void __iomem *addr)
{
	return readl(addr);
}

void __iomem *apq_ioremap(struct platform_device *pdev, const char *name)
{
	struct resource *res;
	void __iomem *mapped;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (IS_ERR(res)) {
		dev_err(&pdev->dev, "Failed to get IO %s resource\n", name);
		return ERR_PTR(-ENODEV);
	}

	mapped = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(mapped)) {
		dev_err(&pdev->dev, "Failed to map IO %s resource\n", name);
		return ERR_PTR(-ENODEV);
	}

	dev_dbg(&pdev->dev,
		 "IO:%s start = 0x%08llx - End = 0x%08llx mapped to 0x%p\n",
		 name, res->start, res->end, mapped);

	return mapped;
}

int apq_get_clk(struct platform_device *pdev, struct clk **clkp,
		const char *name)
{
	dev_dbg(&pdev->dev, "Get clk %s\n", name);

	*clkp = devm_clk_get(&pdev->dev, name);
	if (IS_ERR(*clkp))
		return PTR_ERR(*clkp);

	return 0;
}

static const struct drm_driver apq_driver = {
	.driver_features = DRIVER_MODESET,
	.name = "apq8016",
	.desc = "Understanding drm driver",
	.date = "20260102",
	.major = 0,
	.minor = 1,
};

static int apq_drm_init(struct platform_device *pdev)
{
	struct apq_drm_private *apq_priv;
	struct drm_device *ddev;
	int ret;

	apq_priv = devm_drm_dev_alloc(&pdev->dev, &apq_driver,
				      struct apq_drm_private, ddev);
	if (IS_ERR(apq_priv))
		return PTR_ERR(apq_priv);

	ddev = &apq_priv->ddev;

	platform_set_drvdata(pdev, ddev);

	apq_mdp5_get_kms(apq_priv);

	apq_modeset_init(apq_priv);

	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;

	ret = drm_dev_register(ddev, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(ddev);

	return 0;
}

static int apq_pdev_probe(struct platform_device *pdev)
{
	int ret;

	dev_info(&pdev->dev, "%s - %d\n", __func__, __LINE__);

	/*
	 * Create platform devices for child nodes like MDP5/DSI.
	 * Populate child devices, this will trigger calling probe of
	 * child devices.
	 */
	ret = of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to populate children: %d\n", ret);
		return ret;
	}

	ret = apq_drm_init(pdev);

	return ret;
}

static int apq_pdev_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);
	return 0;
}

static const struct of_device_id apq_dt_match[] = {
	{ .compatible = "qcom,mdss" },
	{},
};

static struct platform_driver apq_platform_driver = {
	.probe = apq_pdev_probe,
	.remove     = apq_pdev_remove,
	.driver = {
		.name = "apq",
		.of_match_table = apq_dt_match,
	},
};

static int __init apq_drm_register(void)
{
	pr_info("%s - %d\n", __func__, __LINE__);

	apq_mdp_register();

	return platform_driver_register(&apq_platform_driver);
}

static void __exit apq_drm_unregister(void)
{
	apq_mdp_unregister();
	platform_driver_unregister(&apq_platform_driver);
}

module_init(apq_drm_register);
module_exit(apq_drm_unregister);

MODULE_AUTHOR("Jagath Jog J");
MODULE_DESCRIPTION("DRM Understanding");
MODULE_LICENSE("GPL");

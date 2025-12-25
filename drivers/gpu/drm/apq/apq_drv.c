// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */


#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "apq_drv.h"

static int apq_pdev_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s - %d\n", __func__, __LINE__);
	dev_dbg(&pdev->dev, "%s - %d\n", __func__, __LINE__);
	return 0;
}

static int apq_pdev_remove(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s - %d\n", __func__, __LINE__);
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
	return platform_driver_register(&apq_platform_driver);
}

static void __exit apq_drm_unregister(void)
{
	platform_driver_unregister(&apq_platform_driver);
}

module_init(apq_drm_register);
module_exit(apq_drm_unregister);

MODULE_AUTHOR("Jagath Jog J");
MODULE_DESCRIPTION("DRM Understanding");
MODULE_LICENSE("GPL");

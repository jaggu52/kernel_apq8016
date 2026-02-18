// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_atomic_helper.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/of_platform.h>
#include <linux/atomic.h>

#include <drm/drm_mode_config.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include "apq_drv.h"

u32 apq_readl(const void __iomem *addr)
{
	return readl(addr);
}

void apq_writel(u32 data, const void __iomem *addr)
{
	writel(data, addr);
}

void __iomem *apq_ioremap(struct platform_device *pdev, const char *name,
			  phys_addr_t *psize)
{
	struct resource *res;
	void __iomem *mapped;
	unsigned long size;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (IS_ERR(res)) {
		dev_err(&pdev->dev, "Failed to get IO %s resource\n", name);
		return ERR_PTR(-ENODEV);
	}

	size = resource_size(res);

	mapped = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(mapped)) {
		dev_err(&pdev->dev, "Failed to map IO %s resource\n", name);
		return ERR_PTR(-ENODEV);
	}

	if (psize)
		*psize = size;

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

static const struct drm_mode_config_funcs apq_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};


static int apq_open(struct drm_device *ddev, struct drm_file *dfile)
{
	pr_info("%s - %d\n", __func__, __LINE__);

	return 0;
}

DEFINE_DRM_GEM_FOPS(fops);

static const struct drm_driver apq_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM,
	.name = "apq",
	.desc = "Understanding drm driver",
	.date = "20260102",
	.major = 0,
	.minor = 1,
	.open = apq_open,
	.fops = &fops,
	DRM_GEM_CMA_DRIVER_OPS,
};

static struct platform_device *apq_master_pdev;
static atomic_t apq_subdevs_probed = ATOMIC_INIT(0);
static atomic_t apq_drm_inited = ATOMIC_INIT(0);
#define APQ_SUBDEV_EXPECTED 2

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

	apq_mdss_init(apq_priv);

	//These are similar to mdp bind in msm driver
	apq_mdp5_get_kms(apq_priv);

	//These are similar to dsi bind in msm driver
	apq_get_dsi(apq_priv);

	/* Initialize @dev's mode_config structure.
	 *
	 * Since this initializes the modeset locks, no locking is possible. Which is no
	 * problem, since this should happen single threaded at init time. It is the
	 * driver's problem to ensure this guarantee.
	 * This will init all drm imp list and initilizes to 0
	 * Use this before starting modeset init
	 */
	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;

	ret = apq_mdp5_modeset_init(apq_priv);
	if (ret)
		return ret;

	ddev->mode_config.funcs = &apq_mode_config_funcs;
	ddev->mode_config.min_width = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width = 0xffff;
	ddev->mode_config.max_height = 0xffff;

	ret = drm_dev_register(ddev, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(ddev);

	return 0;
}

int apq_subdev_probe_done(struct device *dev)
{
	int count;
	int ret;

	if (!apq_master_pdev)
		return -ENODEV;

	count = atomic_inc_return(&apq_subdevs_probed);
	dev_dbg(dev, "apq subdevice probed (%d/%d)\n",
		count, APQ_SUBDEV_EXPECTED);

	if (count != APQ_SUBDEV_EXPECTED)
		return 0;

	if (atomic_xchg(&apq_drm_inited, 1))
		return 0;

	ret = apq_drm_init(apq_master_pdev);
	if (ret)
		dev_err(&apq_master_pdev->dev,
			"DRM init failed after subdevices: %d\n", ret);

	return ret;
}

static int apq_pdev_probe(struct platform_device *pdev)
{
	int ret;

	dev_info(&pdev->dev, "%s - %d\n", __func__, __LINE__);
	apq_master_pdev = pdev;
	atomic_set(&apq_subdevs_probed, 0);
	atomic_set(&apq_drm_inited, 0);

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

	msm_dsi_register();

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

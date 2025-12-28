// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"

int mdp5_enable(struct mdp5_kms *mdp5_kms)
{
	clk_prepare_enable(mdp5_kms->core_clk);
	clk_prepare_enable(mdp5_kms->ahb_clk);

	return 0;
}

static inline u32 mdp5_readl(struct mdp5_kms *mdp5_kms, u32 addr)
{
	return apq_readl(mdp5_kms->mmio + addr);
}

void mdp5_read_hw_rev(struct mdp5_kms *mdp5_kms, u32 *major, u32 *minor)
{
	u32 rev;

	rev = mdp5_readl(mdp5_kms, REG_MDP5_HW_VERSION);
	*major = FIELD_GET(MDP5_HW_VERSION_MAJOR__MASK, rev);
	*minor = FIELD_GET(MDP5_HW_VERSION_MINOR__MASK, rev);

	dev_dbg(mdp5_kms->dev, "MDP5 Rev Major = %d, Minor = %d\n",
		*major, *minor);
}

int mdp5_pdev_probe(struct platform_device *pdev)
{
	struct mdp5_kms *mdp5_kms;
	u32 major, minor;
	int ret;

	dev_info(&pdev->dev, "APQ8016 HW has MDP5!\n");

	mdp5_kms = devm_kmalloc(&pdev->dev, sizeof(mdp5_kms), GFP_KERNEL);
	if (!mdp5_kms)
		return -ENOMEM;

	mdp5_kms->dev = &pdev->dev;

	platform_set_drvdata(pdev, mdp5_kms);

	mdp5_kms->mmio = apq_ioremap(pdev, "mdp_phys");
	if (IS_ERR(mdp5_kms->mmio)) {
		ret = PTR_ERR(mdp5_kms->mmio);
		return ret;
	}

	ret = apq_get_clk(pdev, &mdp5_kms->ahb_clk, "iface");
	if (ret)
		return ret;

	ret = apq_get_clk(pdev, &mdp5_kms->core_clk, "core");
	if (ret)
		return ret;

	mdp5_enable(mdp5_kms);

	mdp5_read_hw_rev(mdp5_kms, &major, &minor);

	return 0;
}

int mdp5_pdev_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id mdp5_dt_match[] = {
	{ .compatible = "qcom,mdp5" },
	{},
};
MODULE_DEVICE_TABLE(of, mdp5_dt_match);

static struct platform_driver mdp5_driver = {
	.probe = mdp5_pdev_probe,
	.remove = mdp5_pdev_remove,
	.driver = {
		.name = "apq_mdp5",
		.of_match_table = mdp5_dt_match,
	},
};

void apq_mdp_register(void)
{
	platform_driver_register(&mdp5_driver);
}

void apq_mdp_unregister(void)
{
	platform_driver_unregister(&mdp5_driver);
}

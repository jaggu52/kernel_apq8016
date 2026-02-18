// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"
#include "mdp5_cfg.h"
#include "mdp5_plane.h"
#include "mdp5_crtc.h"

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

static inline void mdp5_write(struct mdp5_kms *mdp5_kms, u32 addr, u32 data)
{
	apq_writel(data, mdp5_kms->mmio + addr);
}

void mdp5_read_hw_rev(struct mdp5_kms *mdp5_kms, u32 *major, u32 *minor)
{
	u32 rev;

	rev = mdp5_readl(mdp5_kms, REG_MDP5_HW_VERSION);
	*major = FIELD_GET(MDP5_HW_VERSION_MAJOR__MASK, rev);
	*minor = FIELD_GET(MDP5_HW_VERSION_MINOR__MASK, rev);

	dev_info(mdp5_kms->dev, "MDP5 Rev Major = %d, Minor = %d\n",
		*major, *minor);
}

int mdp5_construct_encoder(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *ddev = mdp5_kms->ddev;
	struct apq_drm_private *apq_priv = container_of(ddev,
							struct apq_drm_private,
							ddev);
	int ret;
	struct drm_encoder *encoder;

	drm_info(&apq_priv->ddev, "%s - %d\n", __func__, __LINE__);
	encoder = mdp5_encoder_init(mdp5_kms);

	apq_priv->encoder = encoder;

	return 0;
}

int mdp5_init_intf_mipi(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *ddev = mdp5_kms->ddev;
	struct apq_drm_private *apq_priv = container_of(ddev,
							struct apq_drm_private,
							ddev);

	drm_info(&apq_priv->ddev, "%s - %d\n", __func__, __LINE__);
	msm_dsi_modeset_init(apq_priv->dsi, ddev, apq_priv->encoder);

	return 0;
}

int mdp5_init_intf(struct mdp5_kms *mdp5_kms)
{
	int ret;
	struct drm_device *ddev = mdp5_kms->ddev;
	struct apq_drm_private *apq_priv = container_of(ddev,
							struct apq_drm_private,
							ddev);

	drm_info(&apq_priv->ddev, "%s - %d\n", __func__, __LINE__);
	ret = mdp5_construct_encoder(mdp5_kms);
	if (ret)
		return ret;

	drm_info(&apq_priv->ddev, "%s - %d\n", __func__, __LINE__);
	ret = mdp5_init_intf_mipi(mdp5_kms);
	if (ret)
		return ret;

	return 0;
}

static irqreturn_t mdp5_irq_handler(int irq, void *data)
{
	pr_info("%s - %d\n", __func__, __LINE__);

	return IRQ_HANDLED;
}

int mdp5_irq_install(struct mdp5_kms *mdp5_kms)
{
	int ret;

	mdp5_write(mdp5_kms, REG_MDP5_INTR_CLEAR, 0xffffffff);
	mdp5_write(mdp5_kms, REG_MDP5_INTR_EN, 0xffffffff);

	ret = request_irq(mdp5_kms->irq, mdp5_irq_handler, IRQF_TRIGGER_NONE,
			  "apq_mdp5_irq", mdp5_kms->dev);
	if (ret)
		return ret;

	return 0;
}

int apq_mdp5_modeset_init(struct apq_drm_private *priv)
{
	int ret;

	drm_info(&priv->ddev, "%s - %d\n", __func__, __LINE__);

	ret = mdp5_init_intf(priv->mdp5_kms);
	if (ret) {
		drm_err(&priv->ddev, "Failed to init intf - %d\n", ret);
		return ret;
	}

	ret = mdp5_plane_init(priv);
	if (ret) {
		drm_err(&priv->ddev, "Failed to init plane - %d\n", ret);
		return ret;
	}

	ret = mdp5_crtc_init(priv);
	if (ret) {
		drm_err(&priv->ddev, "Failed to init crtc - %d\n", ret);
		return ret;
	}

	priv->encoder->possible_crtcs = 0x01;

	//install mdp5 irq here
	ret = mdp5_irq_install(priv->mdp5_kms);
	if (ret) {
		pr_err("mdp5 irq install failed\n");
		return ret;
	}

	return 0;
}

//remove this step, use component framework
struct mdp5_kms *mdp5_kms;
void apq_mdp5_get_kms(struct apq_drm_private *priv)
{
	mdp5_kms->ddev = &priv->ddev;
	priv->mdp5_kms = mdp5_kms;

	mdp5_kms->irq = irq_of_parse_and_map(mdp5_kms->dev->of_node, 0);
	pr_info("mdp5_kms->irq = %d\n", mdp5_kms->irq);
}

int mdp5_pdev_probe(struct platform_device *pdev)
{
	u32 major, minor;
	int ret;
	phys_addr_t size;

	pr_info("APQ8016 HW has MDP5!\n");
	pr_info("%s - %d\n", __func__, __LINE__);

	mdp5_kms = devm_kmalloc(&pdev->dev, sizeof(*mdp5_kms), GFP_KERNEL);
	if (!mdp5_kms)
		return -ENOMEM;

	mdp5_kms->dev = &pdev->dev;

	platform_set_drvdata(pdev, mdp5_kms);

	mdp5_kms->mmio = apq_ioremap(pdev, "mdp_phys", &size);
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

	pr_info("%s - %d\n", __func__, __LINE__);

	mdp5_enable(mdp5_kms);

	mdp5_read_hw_rev(mdp5_kms, &major, &minor);

	ret = mdp5_cfg_init(mdp5_kms);
	if (ret)
		return ret;

	return apq_subdev_probe_done(&pdev->dev);
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
	pr_info("%s - %d\n", __func__, __LINE__);
	platform_driver_register(&mdp5_driver);
}

void apq_mdp_unregister(void)
{
	platform_driver_unregister(&mdp5_driver);
}

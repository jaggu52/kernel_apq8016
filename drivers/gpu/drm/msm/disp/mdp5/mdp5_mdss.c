// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 */

#include <linux/irqdomain.h>
#include <linux/irq.h>

#include "msm_drv.h"
#include "mdp5_kms.h"

#define to_mdp5_mdss(x) container_of(x, struct mdp5_mdss, base)

struct mdp5_mdss {
	struct msm_mdss base;

	void __iomem *mmio, *vbif;

	struct regulator *vdd;

	struct clk *ahb_clk;
	struct clk *axi_clk;
	struct clk *vsync_clk;

	struct {
		volatile unsigned long enabled_mask;
		struct irq_domain *domain;
	} irqcontroller;
};

static inline void mdss_write(struct mdp5_mdss *mdp5_mdss, u32 reg, u32 data)
{
	MDP5_MDSS_DBG("write reg=0x%05x data=0x%08x", reg, data);
	msm_writel(data, mdp5_mdss->mmio + reg);
}

static inline u32 mdss_read(struct mdp5_mdss *mdp5_mdss, u32 reg)
{
	u32 val = msm_readl(mdp5_mdss->mmio + reg);
	MDP5_MDSS_DBG("read reg=0x%05x val=0x%08x", reg, val);
	return val;
}

static irqreturn_t mdss_irq(int irq, void *arg)
{
	struct mdp5_mdss *mdp5_mdss = arg;
	u32 intr;
	MSM_FUNC_ENTER("[MDSS] irq=%d", irq);

	intr = mdss_read(mdp5_mdss, REG_MDSS_HW_INTR_STATUS);

	VERB("intr=%08x", intr);
	MDP5_MDSS_DBG("irq intr=0x%08x", intr);

	while (intr) {
		irq_hw_number_t hwirq = fls(intr) - 1;
		MDP5_MDSS_DBG("irq dispatch hwirq=%lu", hwirq);

		generic_handle_domain_irq(mdp5_mdss->irqcontroller.domain, hwirq);
		intr &= ~(1 << hwirq);
	}

	if (!intr)
		MDP5_MDSS_DBG("irq done (no pending bits)");

	MSM_FUNC_EXIT("[MDSS] irq=%d", irq);
	return IRQ_HANDLED;
}

/*
 * interrupt-controller implementation, so sub-blocks (MDP/HDMI/eDP/DSI/etc)
 * can register to get their irq's delivered
 */

#define VALID_IRQS  (MDSS_HW_INTR_STATUS_INTR_MDP | \
		MDSS_HW_INTR_STATUS_INTR_DSI0 | \
		MDSS_HW_INTR_STATUS_INTR_DSI1 | \
		MDSS_HW_INTR_STATUS_INTR_HDMI | \
		MDSS_HW_INTR_STATUS_INTR_EDP)

static void mdss_hw_mask_irq(struct irq_data *irqd)
{
	struct mdp5_mdss *mdp5_mdss = irq_data_get_irq_chip_data(irqd);
	MSM_FUNC_ENTER("[MDSS] mask hwirq=%lu", irqd->hwirq);

	smp_mb__before_atomic();
	clear_bit(irqd->hwirq, &mdp5_mdss->irqcontroller.enabled_mask);
	smp_mb__after_atomic();
	MDP5_MDSS_DBG("mask_irq hwirq=%lu mask=0x%lx", irqd->hwirq,
		mdp5_mdss->irqcontroller.enabled_mask);
	MSM_FUNC_EXIT("[MDSS] mask hwirq=%lu", irqd->hwirq);
}

static void mdss_hw_unmask_irq(struct irq_data *irqd)
{
	struct mdp5_mdss *mdp5_mdss = irq_data_get_irq_chip_data(irqd);
	MSM_FUNC_ENTER("[MDSS] unmask hwirq=%lu", irqd->hwirq);

	smp_mb__before_atomic();
	set_bit(irqd->hwirq, &mdp5_mdss->irqcontroller.enabled_mask);
	smp_mb__after_atomic();
	MDP5_MDSS_DBG("unmask_irq hwirq=%lu mask=0x%lx", irqd->hwirq,
		mdp5_mdss->irqcontroller.enabled_mask);
	MSM_FUNC_EXIT("[MDSS] unmask hwirq=%lu", irqd->hwirq);
}

static struct irq_chip mdss_hw_irq_chip = {
	.name		= "mdss",
	.irq_mask	= mdss_hw_mask_irq,
	.irq_unmask	= mdss_hw_unmask_irq,
};

static int mdss_hw_irqdomain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	struct mdp5_mdss *mdp5_mdss = d->host_data;
	int ret = 0;

	MSM_FUNC_ENTER("[MDSS] irq=%u hwirq=%lu", irq, hwirq);

	if (!(VALID_IRQS & (1 << hwirq))) {
		ret = -EPERM;
		goto out;
	}

	irq_set_chip_and_handler(irq, &mdss_hw_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, mdp5_mdss);
	MDP5_MDSS_DBG("irqdomain_map irq=%u hwirq=%lu", irq, hwirq);

out:
	MSM_FUNC_EXIT("[MDSS] ret=%d", ret);
	return ret;
}

static const struct irq_domain_ops mdss_hw_irqdomain_ops = {
	.map = mdss_hw_irqdomain_map,
	.xlate = irq_domain_xlate_onecell,
};


static int mdss_irq_domain_init(struct mdp5_mdss *mdp5_mdss)
{
	struct device *dev = mdp5_mdss->base.dev->dev;
	struct irq_domain *d;
	int ret = 0;

	MSM_FUNC_ENTER("[MDSS] mdss=%p", mdp5_mdss);

	d = irq_domain_add_linear(dev->of_node, 32, &mdss_hw_irqdomain_ops,
				  mdp5_mdss);
	if (!d) {
		DRM_DEV_ERROR(dev, "mdss irq domain add failed\n");
		ret = -ENXIO;
		goto out;
	}

	mdp5_mdss->irqcontroller.enabled_mask = 0;
	mdp5_mdss->irqcontroller.domain = d;
	MDP5_MDSS_DBG("irq_domain_init done domain=%p", d);

out:
	MSM_FUNC_EXIT("[MDSS] ret=%d", ret);
	return ret;
}

static int mdp5_mdss_enable(struct msm_mdss *mdss)
{
	struct mdp5_mdss *mdp5_mdss = to_mdp5_mdss(mdss);
	DBG("");
	MDP5_MDSS_DBG("enable mdss=%p", mdss);
	MSM_FUNC_ENTER("[MDSS] mdss=%p", mdss);

	clk_prepare_enable(mdp5_mdss->ahb_clk);
	if (mdp5_mdss->axi_clk)
		clk_prepare_enable(mdp5_mdss->axi_clk);
	if (mdp5_mdss->vsync_clk)
		clk_prepare_enable(mdp5_mdss->vsync_clk);
	MSM_FUNC_EXIT("[MDSS] mdss=%p", mdss);
	return 0;
}

static int mdp5_mdss_disable(struct msm_mdss *mdss)
{
	struct mdp5_mdss *mdp5_mdss = to_mdp5_mdss(mdss);
	DBG("");
	MDP5_MDSS_DBG("disable mdss=%p", mdss);
	MSM_FUNC_ENTER("[MDSS] mdss=%p", mdss);

	if (mdp5_mdss->vsync_clk)
		clk_disable_unprepare(mdp5_mdss->vsync_clk);
	if (mdp5_mdss->axi_clk)
		clk_disable_unprepare(mdp5_mdss->axi_clk);
	clk_disable_unprepare(mdp5_mdss->ahb_clk);
	MDP5_MDSS_DBG("disable complete mdss=%p", mdss);
	MSM_FUNC_EXIT("[MDSS] mdss=%p", mdss);
	return 0;
}

static int msm_mdss_get_clocks(struct mdp5_mdss *mdp5_mdss)
{
	struct platform_device *pdev =
			to_platform_device(mdp5_mdss->base.dev->dev);
	int ret = 0;

	MSM_FUNC_ENTER("[MDSS] mdss=%p", mdp5_mdss);

	mdp5_mdss->ahb_clk = msm_clk_get(pdev, "iface");
	if (IS_ERR(mdp5_mdss->ahb_clk))
		mdp5_mdss->ahb_clk = NULL;

	mdp5_mdss->axi_clk = msm_clk_get(pdev, "bus");
	if (IS_ERR(mdp5_mdss->axi_clk))
		mdp5_mdss->axi_clk = NULL;

	mdp5_mdss->vsync_clk = msm_clk_get(pdev, "vsync");
	if (IS_ERR(mdp5_mdss->vsync_clk))
		mdp5_mdss->vsync_clk = NULL;

	MDP5_MDSS_DBG("get_clocks iface=%p bus=%p vsync=%p",
		mdp5_mdss->ahb_clk, mdp5_mdss->axi_clk, mdp5_mdss->vsync_clk);
	if (!mdp5_mdss->ahb_clk || !mdp5_mdss->axi_clk)
		MDP5_MDSS_DBG("clock warning missing iface=%d bus=%d",
			!!mdp5_mdss->ahb_clk, !!mdp5_mdss->axi_clk);

	MSM_FUNC_EXIT("[MDSS] iface=%p bus=%p vsync=%p", mdp5_mdss->ahb_clk,
		mdp5_mdss->axi_clk, mdp5_mdss->vsync_clk);
	return ret;
}

static void mdp5_mdss_destroy(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct mdp5_mdss *mdp5_mdss = to_mdp5_mdss(priv->mdss);

	if (!mdp5_mdss)
		return;
	MDP5_MDSS_DBG("destroy dev=%p", dev);
	MSM_FUNC_ENTER("[MDSS] dev=%p", dev);

	irq_domain_remove(mdp5_mdss->irqcontroller.domain);
	mdp5_mdss->irqcontroller.domain = NULL;

	regulator_disable(mdp5_mdss->vdd);

	pm_runtime_disable(dev->dev);
	MSM_FUNC_EXIT("[MDSS] dev=%p", dev);
}

static const struct msm_mdss_funcs mdss_funcs = {
	.enable	= mdp5_mdss_enable,
	.disable = mdp5_mdss_disable,
	.destroy = mdp5_mdss_destroy,
};

int mdp5_mdss_init(struct drm_device *dev)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	struct msm_drm_private *priv = dev->dev_private;
	struct mdp5_mdss *mdp5_mdss;
	int ret;

	DBG("");
	MSM_FUNC_ENTER("[MDSS] dev=%p", dev);

	if (!of_device_is_compatible(dev->dev->of_node, "qcom,mdss")) {
		MSM_FUNC_EXIT("[MDSS] dev=%p not compatible", dev);
		return 0;
	}

	mdp5_mdss = devm_kzalloc(dev->dev, sizeof(*mdp5_mdss), GFP_KERNEL);
	if (!mdp5_mdss) {
		ret = -ENOMEM;
		goto fail;
	}

	mdp5_mdss->base.dev = dev;

	mdp5_mdss->mmio = msm_ioremap(pdev, "mdss_phys", "MDSS");
	if (IS_ERR(mdp5_mdss->mmio)) {
		ret = PTR_ERR(mdp5_mdss->mmio);
		goto fail;
	}

	mdp5_mdss->vbif = msm_ioremap(pdev, "vbif_phys", "VBIF");
	if (IS_ERR(mdp5_mdss->vbif)) {
		ret = PTR_ERR(mdp5_mdss->vbif);
		goto fail;
	}

	ret = msm_mdss_get_clocks(mdp5_mdss);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to get clocks: %d\n", ret);
		goto fail;
	}

	/* Regulator to enable GDSCs in downstream kernels */
	mdp5_mdss->vdd = devm_regulator_get(dev->dev, "vdd");
	if (IS_ERR(mdp5_mdss->vdd)) {
		ret = PTR_ERR(mdp5_mdss->vdd);
		goto fail;
	}

	ret = regulator_enable(mdp5_mdss->vdd);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to enable regulator vdd: %d\n",
			ret);
		goto fail;
	}

	ret = devm_request_irq(dev->dev, platform_get_irq(pdev, 0),
			       mdss_irq, 0, "mdss_isr", mdp5_mdss);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to init irq: %d\n", ret);
		goto fail_irq;
	}

	ret = mdss_irq_domain_init(mdp5_mdss);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to init sub-block irqs: %d\n", ret);
		goto fail_irq;
	}

	mdp5_mdss->base.funcs = &mdss_funcs;
	priv->mdss = &mdp5_mdss->base;

	pm_runtime_enable(dev->dev);
	MSM_FUNC_EXIT("[MDSS] dev=%p ret=0", dev);
	return 0;
fail_irq:
	regulator_disable(mdp5_mdss->vdd);
fail:
	MSM_FUNC_EXIT("[MDSS] dev=%p ret=%d", dev, ret);
	return ret;
}

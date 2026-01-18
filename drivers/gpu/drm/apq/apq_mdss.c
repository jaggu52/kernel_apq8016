// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <drm/drm_drv.h>
#include "apq_drv.h"

struct apq_mdss {
	struct drm_device *ddev;
	void __iomem *mmio;
	int irq;
	struct {                                                           
                volatile unsigned long enabled_mask;                            
                struct irq_domain *domain;                                      
        } irqcontroller;
};

static irqreturn_t apq_mdss_irq(int irq, void *args)
{
	pr_info("%s - %d\n", __func__, __LINE__);

	return IRQ_HANDLED;
}

static struct irq_chip apq_mdss_irq_chip = {
	.name = "apq_mdss_irq_chip",
	//.irq_mask = 
	//.irq_unmask = 
};
int apq_mdss_irq_map(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw)
{
	struct apq_mdss *apq_mdss = d->host_data;

	pr_info("%s - %d\n", __func__, __LINE__);
	pr_info("hwirq = %d\n", hw);

	irq_set_chip_and_handler(virq, &apq_mdss_irq_chip, handle_level_irq);
	irq_set_chip_data(virq, apq_mdss);

	return 0;
}

static const struct irq_domain_ops mdss_irqdomain_ops = {
	.map = apq_mdss_irq_map,
	.xlate = irq_domain_xlate_onecell,
};

static int mdss_irq_domain_init(struct apq_mdss *apq_mdss)
{
	struct device *dev = apq_mdss->ddev->dev;
	struct irq_domain *domain;

	domain = irq_domain_add_linear(dev->of_node, 5, &mdss_irqdomain_ops,
					apq_mdss);

	apq_mdss->irqcontroller.domain = domain;

	return 0;
}

int apq_mdss_init(struct apq_drm_private *apq_priv)
{
	int ret;
	struct platform_device *pdev = to_platform_device(apq_priv->ddev.dev);
	struct apq_mdss *apq_mdss;

	apq_mdss = devm_kzalloc(apq_priv->ddev.dev, sizeof(struct apq_mdss),
				GFP_KERNEL);
	if (!apq_mdss)
		return -ENOMEM;

	apq_mdss->ddev = &apq_priv->ddev;
	apq_mdss->mmio = apq_ioremap(pdev, "mdss_phys", NULL);

	apq_mdss->irq = platform_get_irq(pdev, 0);
	pr_info("mdss irq number = %d\n", apq_mdss->irq);

	ret = devm_request_irq(apq_priv->ddev.dev, apq_mdss->irq,
				apq_mdss_irq, IRQF_SHARED, "apq_mdss_irq", apq_priv);
	if (ret) {
		pr_info("apq_mdss irq request failed\n");
		return ret;
	}

	ret = mdss_irq_domain_init(apq_mdss);
	if (ret) {
		pr_info("mdss irq domain fail\n");
		return ret;
	}

	return 0;
}




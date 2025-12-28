/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Author: Jagath Jog <jagathjog1996@gmail.com> 
 */

#ifndef __MSM_DRV_H__
#define __MSM_DRV_H__

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#
struct apq_drm_private {
	struct drm_device *dev;
};

void apq_mdp_register(void);
void apq_mdp_unregister(void);
int apq_get_clk(struct platform_device *pdev, struct clk **clkp,
		const char *name);
u32 apq_readl(const void __iomem *addr);
void __iomem *apq_ioremap(struct platform_device *pdev, const char *name);

#endif

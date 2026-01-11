/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Author: Jagath Jog <jagathjog1996@gmail.com> 
 */

#ifndef __MSM_DRV_H__
#define __MSM_DRV_H__

#include <drm/drm_print.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <drm/drm_device.h>
#include "dsi/dsi.h"

#define DBG(fmt, ...) DRM_INFO(fmt"\n", ##__VA_ARGS__)

#define FIELD(val, name) (((val) & name ## __MASK) >> name ## __SHIFT) 

struct apq_drm_private {
	struct drm_device ddev;
	struct mdp5_kms *mdp5_kms;
	struct drm_encoder *encoder;
	struct msm_dsi *dsi;

        unsigned int num_bridges;                                               
        struct drm_bridge *bridges;                                
                                                                                
        unsigned int num_connectors;                                            
        struct drm_connector *connectors;
};

void apq_mdp_register(void);
void apq_mdp_unregister(void);
int apq_get_clk(struct platform_device *pdev, struct clk **clkp,
		const char *name);
int msm_dsi_modeset_init(struct msm_dsi *msm_dsi, struct drm_device *dev,
			 struct drm_encoder *encoder);

static inline struct clk* apq_clk_get(struct platform_device *pdev,
				      const char *name)
{
	struct clk *clk;

	apq_get_clk(pdev, &clk, name);

	return clk;
}

u32 apq_readl(const void __iomem *addr);
void apq_writel(u32 data, const void __iomem *addr);
void __iomem *apq_ioremap(struct platform_device *pdev, const char *name,
			  phys_addr_t *psize);
static inline void __iomem *apq_ioremap_size(struct platform_device *pdev,
					     const char *name,
					     const char *dbgname,
					     phys_addr_t *psize)
{
	return apq_ioremap(pdev, name, psize);
}

void apq_mdp5_get_kms(struct apq_drm_private *priv);
int apq_modeset_init(struct apq_drm_private *priv);
void msm_dsi_register(void);
int apq_get_dsi(struct apq_drm_private *apq_priv);
int apq_subdev_probe_done(struct device *dev);

#endif

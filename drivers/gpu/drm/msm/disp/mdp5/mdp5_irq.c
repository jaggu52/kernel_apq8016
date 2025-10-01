// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/irq.h>

#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "msm_drv.h"
#include "mdp5_kms.h"

void mdp5_set_irqmask(struct mdp_kms *mdp_kms, uint32_t irqmask,
		uint32_t old_irqmask)
{
	MSM_FUNC_ENTER("irqmask=0x%08x old_irqmask=0x%08x", irqmask, old_irqmask);
	MDP5_DBG("set_irqmask new=0x%08x old=0x%08x", irqmask, old_irqmask);
	mdp5_write(to_mdp5_kms(mdp_kms), REG_MDP5_INTR_CLEAR,
		   irqmask ^ (irqmask & old_irqmask));
	mdp5_write(to_mdp5_kms(mdp_kms), REG_MDP5_INTR_EN, irqmask);
	MSM_FUNC_EXIT("");
}

static void mdp5_irq_error_handler(struct mdp_irq *irq, uint32_t irqstatus)
{
	struct mdp5_kms *mdp5_kms = container_of(irq, struct mdp5_kms, error_handler);
	static DEFINE_RATELIMIT_STATE(rs, 5*HZ, 1);
	extern bool dumpstate;

	MSM_FUNC_ENTER("irqstatus=0x%08x", irqstatus);
	MDP5_DBG("irq_error status=0x%08x", irqstatus);
	DRM_ERROR_RATELIMITED("errors: %08x\n", irqstatus);

	if (dumpstate && __ratelimit(&rs)) {
		struct drm_printer p = drm_info_printer(mdp5_kms->dev->dev);
		drm_state_dump(mdp5_kms->dev, &p);
		if (mdp5_kms->smp)
			mdp5_smp_dump(mdp5_kms->smp, &p);
	}
	MSM_FUNC_EXIT("");
}

void mdp5_irq_preinstall(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct device *dev = &mdp5_kms->pdev->dev;

	MSM_FUNC_ENTER("kms=%p", kms);
	MDP5_DBG("irq_preinstall");
	pm_runtime_get_sync(dev);
	mdp5_write(mdp5_kms, REG_MDP5_INTR_CLEAR, 0xffffffff);
	mdp5_write(mdp5_kms, REG_MDP5_INTR_EN, 0x00000000);
	pm_runtime_put_sync(dev);
	MSM_FUNC_EXIT("");
}

int mdp5_irq_postinstall(struct msm_kms *kms)
{
	struct mdp_kms *mdp_kms = to_mdp_kms(kms);
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(mdp_kms);
	struct device *dev = &mdp5_kms->pdev->dev;
	struct mdp_irq *error_handler = &mdp5_kms->error_handler;

	MSM_FUNC_ENTER("kms=%p", kms);
	MDP5_DBG("irq_postinstall");
	error_handler->irq = mdp5_irq_error_handler;
	error_handler->irqmask = MDP5_IRQ_INTF0_UNDER_RUN |
			MDP5_IRQ_INTF1_UNDER_RUN |
			MDP5_IRQ_INTF2_UNDER_RUN |
			MDP5_IRQ_INTF3_UNDER_RUN;

	pm_runtime_get_sync(dev);
	mdp_irq_register(mdp_kms, error_handler);
	pm_runtime_put_sync(dev);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

void mdp5_irq_uninstall(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct device *dev = &mdp5_kms->pdev->dev;

	MSM_FUNC_ENTER("kms=%p", kms);
	MDP5_DBG("irq_uninstall");
	pm_runtime_get_sync(dev);
	mdp5_write(mdp5_kms, REG_MDP5_INTR_EN, 0x00000000);
	pm_runtime_put_sync(dev);
	MSM_FUNC_EXIT("");
}

irqreturn_t mdp5_irq(struct msm_kms *kms)
{
	struct mdp_kms *mdp_kms = to_mdp_kms(kms);
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(mdp_kms);
	struct drm_device *dev = mdp5_kms->dev;
	struct msm_drm_private *priv = dev->dev_private;
	unsigned int id;
	uint32_t status, enable;

	MSM_FUNC_ENTER("kms=%p", kms);
	enable = mdp5_read(mdp5_kms, REG_MDP5_INTR_EN);
	status = mdp5_read(mdp5_kms, REG_MDP5_INTR_STATUS) & enable;
	mdp5_write(mdp5_kms, REG_MDP5_INTR_CLEAR, status);

	VERB("status=%08x", status);
	MDP5_DBG("irq status=0x%08x enable=0x%08x", status, enable);

	mdp_dispatch_irqs(mdp_kms, status);

	for (id = 0; id < priv->num_crtcs; id++)
		if (status & mdp5_crtc_vblank(priv->crtcs[id]))
			drm_handle_vblank(dev, id);
	MDP5_DBG("irq handled status=0x%08x", status);

	MSM_FUNC_EXIT("ret=%d", IRQ_HANDLED);
	return IRQ_HANDLED;
}

int mdp5_enable_vblank(struct msm_kms *kms, struct drm_crtc *crtc)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct device *dev = &mdp5_kms->pdev->dev;

	MSM_FUNC_ENTER("kms=%p crtc=%p", kms, crtc);
	MDP5_DBG("enable_vblank crtc=%p", crtc);
	pm_runtime_get_sync(dev);
	mdp_update_vblank_mask(to_mdp_kms(kms),
			mdp5_crtc_vblank(crtc), true);
	pm_runtime_put_sync(dev);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

void mdp5_disable_vblank(struct msm_kms *kms, struct drm_crtc *crtc)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct device *dev = &mdp5_kms->pdev->dev;

	MSM_FUNC_ENTER("kms=%p crtc=%p", kms, crtc);
	MDP5_DBG("disable_vblank crtc=%p", crtc);
	pm_runtime_get_sync(dev);
	mdp_update_vblank_mask(to_mdp_kms(kms),
			mdp5_crtc_vblank(crtc), false);
	pm_runtime_put_sync(dev);
	MSM_FUNC_EXIT("");
}

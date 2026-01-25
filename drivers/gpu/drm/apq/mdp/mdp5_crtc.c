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

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"
#include "mdp5_cfg.h"
#include "mdp5_crtc.h"

struct mdp5_crtc {
	struct drm_crtc base;
};

static void mdp5_crtc_destroy(struct drm_crtc *crtc)
{

}

const struct drm_crtc_funcs mdp5_crtc_funcs = {
	.destroy = mdp5_crtc_destroy,
};

int mdp5_crtc_init(struct apq_drm_private *priv)
{
	struct mdp5_crtc *mdp5_crtc;
	struct drm_crtc *crtc;
	int ret;

	mdp5_crtc = kzalloc(sizeof(struct mdp5_crtc), GFP_KERNEL);

	crtc = &mdp5_crtc->base;

	ret = drm_crtc_init_with_planes(&priv->ddev, crtc, priv->planes,
					NULL, &mdp5_crtc_funcs, "mdp5_crtc");

	return 0;
}

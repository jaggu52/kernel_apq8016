// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <drm/drm_encoder.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"
#include "mdp5_cfg.h"

struct mdp5_encoder {
	struct drm_encoder base;
};


void mdp5_encoder_enable(struct drm_encoder *encoder)
{

}

const struct drm_encoder_helper_funcs mdp5_encoder_helper = {
	.enable = mdp5_encoder_enable,
};

void mdp5_encoder_destroy(struct drm_encoder *encoder)
{

}

const struct drm_encoder_funcs mdp5_encoder_funcs = {
	.destroy = mdp5_encoder_destroy,
};

struct drm_encoder *mdp5_encoder_init(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *ddev = mdp5_kms->ddev;
	struct mdp5_encoder *mdp5_encoder;
	struct drm_encoder *encoder;
	u32 intf_type = DRM_MODE_ENCODER_DSI;
	int ret;

	mdp5_encoder = devm_kzalloc(mdp5_kms->ddev->dev,
				    sizeof(struct mdp5_encoder), GFP_KERNEL);

	encoder = &mdp5_encoder->base;

	drm_encoder_init(ddev, encoder, &mdp5_encoder_funcs, intf_type,
			       "apq_enc_dsi");

	drm_encoder_helper_add(encoder, &mdp5_encoder_helper);

	return encoder;
}

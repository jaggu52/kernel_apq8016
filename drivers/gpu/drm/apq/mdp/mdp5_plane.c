// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <drm/drm_plane.h>
#include <drm/drm_atomic_helper.h>

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"
#include "mdp5_cfg.h"
#include "mdp5_plane.h"

static const struct drm_plane_funcs mdp5_plane_funcs = {
	.reset = drm_atomic_helper_plane_reset,
	.update_plane = drm_atomic_helper_update_plane,
	.destroy = drm_plane_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static void apq_plane_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{

}

static const struct drm_plane_helper_funcs apq_plane_helper_funcs = {
	.atomic_update = apq_plane_atomic_update,
};

int mdp5_plane_init(struct apq_drm_private *priv)
{
	int ret;
	struct drm_plane *plane;
	struct mdp5_plane *mdp5_plane;

	mdp5_plane = kzalloc(sizeof(struct mdp5_plane), GFP_KERNEL);

	plane = &mdp5_plane->base;
	mdp5_plane->formats[0] = DRM_FORMAT_XRGB8888;
	mdp5_plane->nformats = 1;

	ret = drm_universal_plane_init(&priv->ddev, plane, 0xff,
				       &mdp5_plane_funcs, mdp5_plane->formats,
				       mdp5_plane->nformats, NULL,
				       DRM_PLANE_TYPE_PRIMARY, "mdp5_rgb_plane");
	if (ret) {
		pr_info("plane init failed\n");
		return ret;
	}

	priv->planes = plane;

	drm_plane_helper_add(plane, &apq_plane_helper_funcs);

	return 0;
}

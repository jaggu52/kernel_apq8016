// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#ifndef MDP5_PLANE_H
#define MDP5_PLANE_H

struct mdp5_plane {
	struct drm_plane base;
	uint32_t nformats;
	uint32_t formats[32];
};

int mdp5_plane_init(struct apq_drm_private *priv);

#endif

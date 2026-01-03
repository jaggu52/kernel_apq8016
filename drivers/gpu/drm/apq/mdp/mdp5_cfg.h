// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#ifndef MDP5_CFG_H
#define MDP5_CFG_H

struct mdp5_hw_cfg {
	char *name;

	//mdp
	u32 mdp_count;
	u32 mdp_base;
	u32 mdp_cap;

	//pipe
	u32 pipe_rgb_count;
	u32 pipe_rgb_base[4];
	u32 pipe_rgb_cap;

	//ctl
	u32 ctl;

	//intf
	u32 intf_mipi_base;
	u32 intf_id;

	u32 max_clk;
};

int mdp5_cfg_init(struct mdp5_kms *mdp5_kms);

#endif

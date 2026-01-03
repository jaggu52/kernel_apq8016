// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 
 * Author: Jagath Jog J <jagathjog1996@gmail.com>
 *
 * For understanding DRM framework
 */

#include "apq_drv.h"
#include "mdp5_kms.h"
#include "mdp5_xml.h"
#include "mdp5_cfg.h"

const struct mdp5_hw_cfg mdp5_hw_cfg = {
	.name = "apq8016",
	.mdp_count = 1,
	.mdp_base = 0x00,
	.ctl = 0,
	.pipe_rgb_count = 2,
	.pipe_rgb_base = {0x14000, 0x16000},
	.intf_mipi_base = 0x6a800,
	.max_clk = 320000000,
};

int mdp5_cfg_init(struct mdp5_kms *mdp5_kms)
{
	mdp5_kms->mdp5_hw = &mdp5_hw_cfg;

	return 0;
}

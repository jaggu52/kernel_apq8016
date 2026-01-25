/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Author: Jagath Jog <jagathjog1996@gmail.com> 
 */

#ifndef __MDP_CRTC_H
#define __MDP_CRTC_H

#include <drm/drm_print.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <drm/drm_device.h>
#include "dsi/dsi.h"

#include "apq_drv.h"

int mdp5_crtc_init(struct apq_drm_private *priv);

#endif

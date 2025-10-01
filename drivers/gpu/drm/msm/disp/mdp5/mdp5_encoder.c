// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <drm/drm_crtc.h>
#include <drm/drm_probe_helper.h>

#include "mdp5_kms.h"

static struct mdp5_kms *get_kms(struct drm_encoder *encoder)
{
	struct msm_drm_private *priv = encoder->dev->dev_private;
	struct mdp5_kms *mdp5_kms;

	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);
	mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	MSM_FUNC_EXIT("[ENC] kms=%p", mdp5_kms);
	return mdp5_kms;
}

static void mdp5_encoder_destroy(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);
	drm_encoder_cleanup(encoder);
	kfree(mdp5_encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static const struct drm_encoder_funcs mdp5_encoder_funcs = {
	.destroy = mdp5_encoder_destroy,
};

static void mdp5_vid_encoder_mode_set(struct drm_encoder *encoder,
				      struct drm_display_mode *mode,
				      struct drm_display_mode *adjusted_mode)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	struct drm_device *dev = encoder->dev;
	struct drm_connector *connector;
	int intf = mdp5_encoder->intf->num;
	uint32_t dtv_hsync_skew, vsync_period, vsync_len, ctrl_pol;
	uint32_t display_v_start, display_v_end;
	uint32_t hsync_start_x, hsync_end_x;
	uint32_t format = 0x2100;
	unsigned long flags;

	mode = adjusted_mode;

	DBG("set mode: " DRM_MODE_FMT, DRM_MODE_ARG(mode));
	MDP5_ENC_DBG("vid_mode_set encoder=%p intf=%d mode=%dx%d@%d", encoder,
		intf, mode->hdisplay, mode->vdisplay,
		drm_mode_vrefresh(mode));
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	ctrl_pol = 0;

	/* DSI controller cannot handle active-low sync signals. */
	if (mdp5_encoder->intf->type != INTF_DSI) {
		if (mode->flags & DRM_MODE_FLAG_NHSYNC)
			ctrl_pol |= MDP5_INTF_POLARITY_CTL_HSYNC_LOW;
		if (mode->flags & DRM_MODE_FLAG_NVSYNC)
			ctrl_pol |= MDP5_INTF_POLARITY_CTL_VSYNC_LOW;
	}
	/* probably need to get DATA_EN polarity from panel.. */

	dtv_hsync_skew = 0;  /* get this from panel? */

	/* Get color format from panel, default is 8bpc */
	list_for_each_entry(connector, &dev->mode_config.connector_list, head) {
		if (connector->encoder == encoder) {
			switch (connector->display_info.bpc) {
			case 4:
				format |= 0;
				break;
			case 5:
				format |= 0x15;
				break;
			case 6:
				format |= 0x2A;
				break;
			case 8:
			default:
				format |= 0x3F;
				break;
			}
			break;
		}
	}

	hsync_start_x = (mode->htotal - mode->hsync_start);
	hsync_end_x = mode->htotal - (mode->hsync_start - mode->hdisplay) - 1;

	vsync_period = mode->vtotal * mode->htotal;
	vsync_len = (mode->vsync_end - mode->vsync_start) * mode->htotal;
	display_v_start = (mode->vtotal - mode->vsync_start) * mode->htotal + dtv_hsync_skew;
	display_v_end = vsync_period - ((mode->vsync_start - mode->vdisplay) * mode->htotal) + dtv_hsync_skew - 1;

	/*
	 * For edp only:
	 * DISPLAY_V_START = (VBP * HCYCLE) + HBP
	 * DISPLAY_V_END = (VBP + VACTIVE) * HCYCLE - 1 - HFP
	 */
	if (mdp5_encoder->intf->type == INTF_eDP) {
		display_v_start += mode->htotal - mode->hsync_start;
		display_v_end -= mode->hsync_start - mode->hdisplay;
	}

	spin_lock_irqsave(&mdp5_encoder->intf_lock, flags);

	mdp5_write(mdp5_kms, REG_MDP5_INTF_HSYNC_CTL(intf),
			MDP5_INTF_HSYNC_CTL_PULSEW(mode->hsync_end - mode->hsync_start) |
			MDP5_INTF_HSYNC_CTL_PERIOD(mode->htotal));
	mdp5_write(mdp5_kms, REG_MDP5_INTF_VSYNC_PERIOD_F0(intf), vsync_period);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_VSYNC_LEN_F0(intf), vsync_len);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_DISPLAY_HCTL(intf),
			MDP5_INTF_DISPLAY_HCTL_START(hsync_start_x) |
			MDP5_INTF_DISPLAY_HCTL_END(hsync_end_x));
	mdp5_write(mdp5_kms, REG_MDP5_INTF_DISPLAY_VSTART_F0(intf), display_v_start);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_DISPLAY_VEND_F0(intf), display_v_end);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_BORDER_COLOR(intf), 0);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_UNDERFLOW_COLOR(intf), 0xff);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_HSYNC_SKEW(intf), dtv_hsync_skew);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_POLARITY_CTL(intf), ctrl_pol);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_ACTIVE_HCTL(intf),
			MDP5_INTF_ACTIVE_HCTL_START(0) |
			MDP5_INTF_ACTIVE_HCTL_END(0));
	mdp5_write(mdp5_kms, REG_MDP5_INTF_ACTIVE_VSTART_F0(intf), 0);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_ACTIVE_VEND_F0(intf), 0);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_PANEL_FORMAT(intf), format);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_FRAME_LINE_COUNT_EN(intf), 0x3);  /* frame+line? */

	spin_unlock_irqrestore(&mdp5_encoder->intf_lock, flags);

	mdp5_crtc_set_pipeline(encoder->crtc);
	MDP5_ENC_DBG("vid_mode_set done encoder=%p", encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static void mdp5_vid_encoder_disable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	struct mdp5_ctl *ctl = mdp5_encoder->ctl;
	struct mdp5_pipeline *pipeline = mdp5_crtc_get_pipeline(encoder->crtc);
	struct mdp5_hw_mixer *mixer = mdp5_crtc_get_mixer(encoder->crtc);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	int intfn = mdp5_encoder->intf->num;
	unsigned long flags;
	u32 flush_mask = mdp_ctl_flush_mask_encoder(intf);

	MDP5_ENC_DBG("vid_disable encoder=%p enabled=%d", encoder,
		mdp5_encoder->enabled);
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	if (WARN_ON(!mdp5_encoder->enabled))
		goto out;

	mdp5_ctl_set_encoder_state(ctl, pipeline, false);

	spin_lock_irqsave(&mdp5_encoder->intf_lock, flags);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_TIMING_ENGINE_EN(intfn), 0);
	spin_unlock_irqrestore(&mdp5_encoder->intf_lock, flags);
	mdp5_ctl_commit(ctl, pipeline, flush_mask, true);

	/*
	 * Wait for a vsync so we know the ENABLE=0 latched before
	 * the (connector) source of the vsync's gets disabled,
	 * otherwise we end up in a funny state if we re-enable
	 * before the disable latches, which results that some of
	 * the settings changes for the new modeset (like new
	 * scanout buffer) don't latch properly..
	 */
	mdp_irq_wait(&mdp5_kms->base, intf2vblank(mixer, intf));

	mdp5_encoder->enabled = false;
	MDP5_ENC_DBG("vid_disable complete encoder=%p", encoder);
	out:
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static void mdp5_vid_encoder_enable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	struct mdp5_ctl *ctl = mdp5_encoder->ctl;
	struct mdp5_interface *intf = mdp5_encoder->intf;
	struct mdp5_pipeline *pipeline = mdp5_crtc_get_pipeline(encoder->crtc);
	int intfn = intf->num;
	unsigned long flags;
	u32 flush_mask = mdp_ctl_flush_mask_encoder(intf);

	MDP5_ENC_DBG("vid_enable encoder=%p enabled=%d", encoder,
		mdp5_encoder->enabled);
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	if (WARN_ON(mdp5_encoder->enabled))
		goto out;

	spin_lock_irqsave(&mdp5_encoder->intf_lock, flags);
	mdp5_write(mdp5_kms, REG_MDP5_INTF_TIMING_ENGINE_EN(intfn), 1);
	spin_unlock_irqrestore(&mdp5_encoder->intf_lock, flags);
	mdp5_ctl_commit(ctl, pipeline, flush_mask, true);

	mdp5_ctl_set_encoder_state(ctl, pipeline, true);

	mdp5_encoder->enabled = true;
	MDP5_ENC_DBG("vid_enable complete encoder=%p", encoder);
out:
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static void mdp5_encoder_mode_set(struct drm_encoder *encoder,
				  struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	MDP5_ENC_DBG("mode_set mux encoder=%p intf_type=%d", encoder,
		intf->type);
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	if (intf->mode == MDP5_INTF_DSI_MODE_COMMAND)
		mdp5_cmd_encoder_mode_set(encoder, mode, adjusted_mode);
	else
		mdp5_vid_encoder_mode_set(encoder, mode, adjusted_mode);
	MDP5_ENC_DBG("mode_set mux complete encoder=%p", encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static void mdp5_encoder_disable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	if (intf->mode == MDP5_INTF_DSI_MODE_COMMAND)
		mdp5_cmd_encoder_disable(encoder);
	else
		mdp5_vid_encoder_disable(encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static void mdp5_encoder_enable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	/* this isn't right I think */
	struct drm_crtc_state *cstate = encoder->crtc->state;
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	mdp5_encoder_mode_set(encoder, &cstate->mode, &cstate->adjusted_mode);

	if (intf->mode == MDP5_INTF_DSI_MODE_COMMAND)
		mdp5_cmd_encoder_enable(encoder);
	else
		mdp5_vid_encoder_enable(encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
}

static int mdp5_encoder_atomic_check(struct drm_encoder *encoder,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_crtc_state *mdp5_cstate = to_mdp5_crtc_state(crtc_state);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	struct mdp5_ctl *ctl = mdp5_encoder->ctl;
	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);

	mdp5_cstate->ctl = ctl;
	mdp5_cstate->pipeline.intf = intf;

	/*
	 * This is a bit awkward, but we want to flush the CTL and hit the
	 * START bit at most once for an atomic update.  In the non-full-
	 * modeset case, this is done from crtc->atomic_flush(), but that
	 * is too early in the case of full modeset, in which case we
	 * defer to encoder->enable().  But we need to *know* whether
	 * encoder->enable() will be called to do this:
	 */
	if (drm_atomic_crtc_needs_modeset(crtc_state))
		mdp5_cstate->defer_start = true;
 	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
 	return 0;
}

static const struct drm_encoder_helper_funcs mdp5_encoder_helper_funcs = {
	.disable = mdp5_encoder_disable,
	.enable = mdp5_encoder_enable,
	.atomic_check = mdp5_encoder_atomic_check,
};

int mdp5_encoder_get_linecount(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	int intf = mdp5_encoder->intf->num;
	int ret;

	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);
	ret = mdp5_read(mdp5_kms, REG_MDP5_INTF_LINE_COUNT(intf));
	MSM_FUNC_EXIT("[ENC] encoder=%p line=%d", encoder, ret);
	return ret;
}

u32 mdp5_encoder_get_framecount(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	int intf = mdp5_encoder->intf->num;
	u32 ret;

	MSM_FUNC_ENTER("[ENC] encoder=%p", encoder);
	ret = mdp5_read(mdp5_kms, REG_MDP5_INTF_FRAME_COUNT(intf));
	MSM_FUNC_EXIT("[ENC] encoder=%p frame=%u", encoder, ret);
	return ret;
}

int mdp5_vid_encoder_set_split_display(struct drm_encoder *encoder,
				       struct drm_encoder *slave_encoder)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_encoder *mdp5_slave_enc = to_mdp5_encoder(slave_encoder);
	struct mdp5_kms *mdp5_kms;
	struct device *dev;
	int intf_num;
	u32 data = 0;
	int ret = 0;

	if (!encoder || !slave_encoder)
		return -EINVAL;

	MSM_FUNC_ENTER("[ENC] encoder=%p slave=%p", encoder, slave_encoder);

	mdp5_kms = get_kms(encoder);
	intf_num = mdp5_encoder->intf->num;

	/* Switch slave encoder's TimingGen Sync mode,
	 * to use the master's enable signal for the slave encoder.
	 */
	if (intf_num == 1)
		data |= MDP5_SPLIT_DPL_LOWER_INTF2_TG_SYNC;
	else if (intf_num == 2)
		data |= MDP5_SPLIT_DPL_LOWER_INTF1_TG_SYNC;
	else
		goto out_err;

	dev = &mdp5_kms->pdev->dev;
	/* Make sure clocks are on when connectors calling this function. */
	pm_runtime_get_sync(dev);

	/* Dumb Panel, Sync mode */
	mdp5_write(mdp5_kms, REG_MDP5_SPLIT_DPL_UPPER, 0);
	mdp5_write(mdp5_kms, REG_MDP5_SPLIT_DPL_LOWER, data);
	mdp5_write(mdp5_kms, REG_MDP5_SPLIT_DPL_EN, 1);

	mdp5_ctl_pair(mdp5_encoder->ctl, mdp5_slave_enc->ctl, true);

	pm_runtime_put_sync(dev);
	ret = 0;
	MSM_FUNC_EXIT("[ENC] encoder=%p ret=%d", encoder, ret);
	return ret;

out_err:
	ret = -EINVAL;
	MSM_FUNC_EXIT("[ENC] encoder=%p ret=%d", encoder, ret);
	return ret;
}

void mdp5_encoder_set_intf_mode(struct drm_encoder *encoder, bool cmd_mode)
{
	struct mdp5_encoder *mdp5_encoder = to_mdp5_encoder(encoder);
	struct mdp5_interface *intf = mdp5_encoder->intf;
	MSM_FUNC_ENTER("[ENC] encoder=%p cmd_mode=%d", encoder, cmd_mode);

	/* TODO: Expand this to set writeback modes too */
	if (cmd_mode) {
		WARN_ON(intf->type != INTF_DSI);
		intf->mode = MDP5_INTF_DSI_MODE_COMMAND;
	} else {
		if (intf->type == INTF_DSI)
			intf->mode = MDP5_INTF_DSI_MODE_VIDEO;
		else
			intf->mode = MDP5_INTF_MODE_NONE;
	}
	MSM_FUNC_EXIT("[ENC] encoder=%p mode=%d", encoder, intf->mode);
}

/* initialize encoder */
struct drm_encoder *mdp5_encoder_init(struct drm_device *dev,
				      struct mdp5_interface *intf,
				      struct mdp5_ctl *ctl)
{
	struct drm_encoder *encoder = NULL;
	struct mdp5_encoder *mdp5_encoder;
	int enc_type = (intf->type == INTF_DSI) ?
		DRM_MODE_ENCODER_DSI : DRM_MODE_ENCODER_TMDS;
	int ret;
	MSM_FUNC_ENTER("[ENC] dev=%p intf=%d", dev, intf->type);

	mdp5_encoder = kzalloc(sizeof(*mdp5_encoder), GFP_KERNEL);
	if (!mdp5_encoder) {
		ret = -ENOMEM;
		goto fail;
	}

	encoder = &mdp5_encoder->base;
	mdp5_encoder->ctl = ctl;
	mdp5_encoder->intf = intf;

	spin_lock_init(&mdp5_encoder->intf_lock);

	drm_encoder_init(dev, encoder, &mdp5_encoder_funcs, enc_type, NULL);

	drm_encoder_helper_add(encoder, &mdp5_encoder_helper_funcs);
	MSM_FUNC_EXIT("[ENC] encoder=%p", encoder);
	return encoder;

fail:
	if (encoder)
		mdp5_encoder_destroy(encoder);
	MSM_FUNC_EXIT("[ENC] encoder=%p ret=%d", encoder, ret);
	return ERR_PTR(ret);
}

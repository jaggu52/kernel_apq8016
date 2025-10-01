// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014-2015 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

#include "mdp5_kms.h"

struct mdp5_plane {
	struct drm_plane base;

	uint32_t nformats;
	uint32_t formats[32];
};
#define to_mdp5_plane(x) container_of(x, struct mdp5_plane, base)

static int mdp5_plane_mode_set(struct drm_plane *plane,
		struct drm_crtc *crtc, struct drm_framebuffer *fb,
		struct drm_rect *src, struct drm_rect *dest);

static struct mdp5_kms *get_kms(struct drm_plane *plane)
{
	struct msm_drm_private *priv = plane->dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	MDP5_PLANE_DBG("get_kms plane=%s kms=%p", plane->name, mdp5_kms);
	return mdp5_kms;
}

static bool plane_enabled(struct drm_plane_state *state)
{
	return state->visible;
}

static void mdp5_plane_destroy(struct drm_plane *plane)
{
	struct mdp5_plane *mdp5_plane = to_mdp5_plane(plane);

	MSM_FUNC_ENTER("plane=%s", plane->name);
	MDP5_PLANE_DBG("destroy plane=%s", plane->name);
	drm_plane_cleanup(plane);

	kfree(mdp5_plane);
	MSM_FUNC_EXIT("");
}

/* helper to install properties which are common to planes and crtcs */
static void mdp5_plane_install_properties(struct drm_plane *plane,
		struct drm_mode_object *obj)
{
	MSM_FUNC_ENTER("plane=%s", plane->name);
	drm_plane_create_rotation_property(plane,
					   DRM_MODE_ROTATE_0,
					   DRM_MODE_ROTATE_0 |
					   DRM_MODE_ROTATE_180 |
					   DRM_MODE_REFLECT_X |
					   DRM_MODE_REFLECT_Y);
	drm_plane_create_alpha_property(plane);
	drm_plane_create_blend_mode_property(plane,
			BIT(DRM_MODE_BLEND_PIXEL_NONE) |
			BIT(DRM_MODE_BLEND_PREMULTI) |
			BIT(DRM_MODE_BLEND_COVERAGE));
	drm_plane_create_zpos_property(plane, 1, 1, 255);
	MDP5_PLANE_DBG("install_properties plane=%s", plane->name);
	MSM_FUNC_EXIT("");
}

static void
mdp5_plane_atomic_print_state(struct drm_printer *p,
		const struct drm_plane_state *state)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(state);
	struct mdp5_kms *mdp5_kms = get_kms(state->plane);

	MSM_FUNC_ENTER("plane=%s", state->plane->name);
	drm_printf(p, "\thwpipe=%s\n", pstate->hwpipe ?
			pstate->hwpipe->name : "(null)");
	if (mdp5_kms->caps & MDP_CAP_SRC_SPLIT)
		drm_printf(p, "\tright-hwpipe=%s\n",
			   pstate->r_hwpipe ? pstate->r_hwpipe->name :
					      "(null)");
	drm_printf(p, "\tblend_mode=%u\n", pstate->base.pixel_blend_mode);
	drm_printf(p, "\tzpos=%u\n", pstate->base.zpos);
	drm_printf(p, "\tnormalized_zpos=%u\n", pstate->base.normalized_zpos);
	drm_printf(p, "\talpha=%u\n", pstate->base.alpha);
	drm_printf(p, "\tstage=%s\n", stage2name(pstate->stage));
	MDP5_PLANE_DBG("print_state plane=%s stage=%s", state->plane->name,
		stage2name(pstate->stage));
	MSM_FUNC_EXIT("");
}

static void mdp5_plane_reset(struct drm_plane *plane)
{
	struct mdp5_plane_state *mdp5_state;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	if (plane->state)
		__drm_atomic_helper_plane_destroy_state(plane->state);

	kfree(to_mdp5_plane_state(plane->state));
	mdp5_state = kzalloc(sizeof(*mdp5_state), GFP_KERNEL);
	MDP5_PLANE_DBG("reset plane=%s", plane->name);

	if (plane->type == DRM_PLANE_TYPE_PRIMARY)
		mdp5_state->base.zpos = STAGE_BASE;
	else
		mdp5_state->base.zpos = STAGE0 + drm_plane_index(plane);
	mdp5_state->base.normalized_zpos = mdp5_state->base.zpos;

	__drm_atomic_helper_plane_reset(plane, &mdp5_state->base);
	MSM_FUNC_EXIT("");
}

static struct drm_plane_state *
mdp5_plane_duplicate_state(struct drm_plane *plane)
{
	struct mdp5_plane_state *mdp5_state;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	if (WARN_ON(!plane->state)) {
		MSM_FUNC_EXIT("ret=NULL");
		return NULL;
	}

	mdp5_state = kmemdup(to_mdp5_plane_state(plane->state),
			sizeof(*mdp5_state), GFP_KERNEL);
	if (!mdp5_state) {
		MSM_FUNC_EXIT("ret=NULL");
		return NULL;
	}

	__drm_atomic_helper_plane_duplicate_state(plane, &mdp5_state->base);
	MDP5_PLANE_DBG("duplicate_state plane=%s", plane->name);

	MSM_FUNC_EXIT("ret=%p", &mdp5_state->base);
	return &mdp5_state->base;
}

static void mdp5_plane_destroy_state(struct drm_plane *plane,
		struct drm_plane_state *state)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(state);

	MSM_FUNC_ENTER("plane=%s state=%p", plane->name, state);
	if (state->fb)
		drm_framebuffer_put(state->fb);
	MDP5_PLANE_DBG("destroy_state plane=%s", plane->name);

	kfree(pstate);
	MSM_FUNC_EXIT("");
}

static const struct drm_plane_funcs mdp5_plane_funcs = {
		.update_plane = drm_atomic_helper_update_plane,
		.disable_plane = drm_atomic_helper_disable_plane,
		.destroy = mdp5_plane_destroy,
		.reset = mdp5_plane_reset,
		.atomic_duplicate_state = mdp5_plane_duplicate_state,
		.atomic_destroy_state = mdp5_plane_destroy_state,
		.atomic_print_state = mdp5_plane_atomic_print_state,
};

static void mdp5_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *old_state)
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct msm_kms *kms = &mdp5_kms->base.base;
	struct drm_framebuffer *fb = old_state->fb;

	MSM_FUNC_ENTER("plane=%s old_state=%p", plane->name, old_state);
	if (!fb)
		goto exit;

	DBG("%s: cleanup: FB[%u]", plane->name, fb->base.id);
	MDP5_PLANE_DBG("cleanup_fb plane=%s fb=%u", plane->name, fb->base.id);
	msm_framebuffer_cleanup(fb, kms->aspace);

exit:
	MSM_FUNC_EXIT("");
}

static int mdp5_plane_atomic_check_with_state(struct drm_crtc_state *crtc_state,
					      struct drm_plane_state *state)
{
	struct mdp5_plane_state *mdp5_state = to_mdp5_plane_state(state);
	struct drm_plane *plane = state->plane;
	struct drm_plane_state *old_state = plane->state;
	struct mdp5_cfg *config = mdp5_cfg_get_config(get_kms(plane)->cfg);
	bool new_hwpipe = false;
	bool need_right_hwpipe = false;
	uint32_t max_width, max_height;
	bool out_of_bounds = false;
	uint32_t caps = 0;
	int min_scale, max_scale;
	int ret;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	DBG("%s: check (%d -> %d)", plane->name,
			plane_enabled(old_state), plane_enabled(state));
	MDP5_PLANE_DBG("check plane=%s src=%dx%d dst=%dx%d", plane->name,
		drm_rect_width(&state->src) >> 16,
		drm_rect_height(&state->src) >> 16,
		drm_rect_width(&state->dst),
		drm_rect_height(&state->dst));

	max_width = config->hw->lm.max_width << 16;
	max_height = config->hw->lm.max_height << 16;

	/* Make sure source dimensions are within bounds. */
	if (state->src_h > max_height)
		out_of_bounds = true;

	if (state->src_w > max_width) {
		/* If source split is supported, we can go up to 2x
		 * the max LM width, but we'd need to stage another
		 * hwpipe to the right LM. So, the drm_plane would
		 * consist of 2 hwpipes.
		 */
		if (config->hw->mdp.caps & MDP_CAP_SRC_SPLIT &&
		    (state->src_w <= 2 * max_width))
			need_right_hwpipe = true;
		else
			out_of_bounds = true;
	}
	MDP5_PLANE_DBG("check caps plane=%s need_right=%d out_of_bounds=%d", plane->name,
		need_right_hwpipe, out_of_bounds);

	if (out_of_bounds) {
		struct drm_rect src = drm_plane_state_src(state);
		DBG("Invalid source size "DRM_RECT_FP_FMT,
				DRM_RECT_FP_ARG(&src));
		MDP5_PLANE_DBG("check out_of_bounds plane=%s", plane->name);
		ret = -ERANGE;
		goto exit;
	}

	min_scale = FRAC_16_16(1, 8);
	max_scale = FRAC_16_16(8, 1);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  min_scale, max_scale,
						  true, true);
	if (ret)
		goto exit;

	if (plane_enabled(state)) {
		unsigned int rotation;
		const struct mdp_format *format;
		struct mdp5_kms *mdp5_kms = get_kms(plane);
		uint32_t blkcfg = 0;

		format = to_mdp_format(msm_framebuffer_format(state->fb));
		if (MDP_FORMAT_IS_YUV(format))
			caps |= MDP_PIPE_CAP_SCALE | MDP_PIPE_CAP_CSC;

		if (((state->src_w >> 16) != state->crtc_w) ||
				((state->src_h >> 16) != state->crtc_h))
			caps |= MDP_PIPE_CAP_SCALE;

		rotation = drm_rotation_simplify(state->rotation,
						 DRM_MODE_ROTATE_0 |
						 DRM_MODE_REFLECT_X |
						 DRM_MODE_REFLECT_Y);

		if (rotation & DRM_MODE_REFLECT_X)
			caps |= MDP_PIPE_CAP_HFLIP;

		if (rotation & DRM_MODE_REFLECT_Y)
			caps |= MDP_PIPE_CAP_VFLIP;

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			caps |= MDP_PIPE_CAP_CURSOR;

		/* (re)allocate hw pipe if we don't have one or caps-mismatch: */
		if (!mdp5_state->hwpipe || (caps & ~mdp5_state->hwpipe->caps))
			new_hwpipe = true;

		/*
		 * (re)allocte hw pipe if we're either requesting for 2 hw pipes
		 * or we're switching from 2 hw pipes to 1 hw pipe because the
		 * new src_w can be supported by 1 hw pipe itself.
		 */
		if ((need_right_hwpipe && !mdp5_state->r_hwpipe) ||
		    (!need_right_hwpipe && mdp5_state->r_hwpipe))
			new_hwpipe = true;

		if (mdp5_kms->smp) {
			const struct mdp_format *format =
				to_mdp_format(msm_framebuffer_format(state->fb));

			blkcfg = mdp5_smp_calculate(mdp5_kms->smp, format,
					state->src_w >> 16, false);

			if (mdp5_state->hwpipe && (mdp5_state->hwpipe->blkcfg != blkcfg))
				new_hwpipe = true;
		}
		if (new_hwpipe)
			MDP5_PLANE_DBG("check new_hwpipe plane=%s caps=0x%08x blkcfg=0x%08x",
				plane->name, caps, blkcfg);

		/* (re)assign hwpipe if needed, otherwise keep old one: */
		if (new_hwpipe) {
			/* TODO maybe we want to re-assign hwpipe sometimes
			 * in cases when we no-longer need some caps to make
			 * it available for other planes?
			 */
			struct mdp5_hw_pipe *old_hwpipe = mdp5_state->hwpipe;
			struct mdp5_hw_pipe *old_right_hwpipe =
							  mdp5_state->r_hwpipe;
			struct mdp5_hw_pipe *new_hwpipe = NULL;
			struct mdp5_hw_pipe *new_right_hwpipe = NULL;

			ret = mdp5_pipe_assign(state->state, plane, caps,
					       blkcfg, &new_hwpipe,
					       need_right_hwpipe ?
					       &new_right_hwpipe : NULL);
			if (ret) {
				DBG("%s: failed to assign hwpipe(s)!",
				    plane->name);
				goto exit;
			}

			mdp5_state->hwpipe = new_hwpipe;
			if (need_right_hwpipe)
				mdp5_state->r_hwpipe = new_right_hwpipe;
			else
				/*
				 * set it to NULL so that the driver knows we
				 * don't have a right hwpipe when committing a
				 * new state
				 */
				mdp5_state->r_hwpipe = NULL;
			MDP5_PLANE_DBG("assign_hwpipe plane=%s left=%s right=%s", plane->name,
				mdp5_state->hwpipe ? mdp5_state->hwpipe->name : "(null)",
				mdp5_state->r_hwpipe ? mdp5_state->r_hwpipe->name : "(null)");


			mdp5_pipe_release(state->state, old_hwpipe);
			mdp5_pipe_release(state->state, old_right_hwpipe);
		}
	} else {
		mdp5_pipe_release(state->state, mdp5_state->hwpipe);
		mdp5_pipe_release(state->state, mdp5_state->r_hwpipe);
		mdp5_state->hwpipe = mdp5_state->r_hwpipe = NULL;
		MDP5_PLANE_DBG("release_hwpipe plane=%s", plane->name);
	}

	ret = 0;

exit:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int mdp5_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state,
										 plane);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int ret;

	MSM_FUNC_ENTER("plane=%s", plane->name);

	crtc = new_plane_state->crtc ? new_plane_state->crtc : old_plane_state->crtc;
	if (!crtc) {
		ret = 0;
		goto exit;
	}

	crtc_state = drm_atomic_get_existing_crtc_state(state,
							crtc);
	if (WARN_ON(!crtc_state)) {
		ret = -EINVAL;
		goto exit;
	}
	MDP5_PLANE_DBG("atomic_check plane=%s crtc=%s", plane->name,
		crtc->name);

	ret = mdp5_plane_atomic_check_with_state(crtc_state, new_plane_state);

exit:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void mdp5_plane_atomic_update(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);

	MSM_FUNC_ENTER("plane=%s", plane->name);
	DBG("%s: update", plane->name);
	MDP5_PLANE_DBG("atomic_update plane=%s visible=%d", plane->name,
		plane_enabled(new_state));

	if (plane_enabled(new_state)) {
		int ret;

		ret = mdp5_plane_mode_set(plane,
				new_state->crtc, new_state->fb,
				&new_state->src, &new_state->dst);
		/* atomic_check should have ensured that this doesn't fail */
		WARN_ON(ret < 0);
		if (ret)
			MDP5_PLANE_DBG("atomic_update plane=%s mode_set ret=%d", plane->name, ret);
	}
	MSM_FUNC_EXIT("");
}

static int mdp5_plane_atomic_async_check(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct mdp5_plane_state *mdp5_state = to_mdp5_plane_state(new_plane_state);
	struct drm_crtc_state *crtc_state;
	int min_scale, max_scale;
	int ret;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	crtc_state = drm_atomic_get_existing_crtc_state(state,
							new_plane_state->crtc);
	if (WARN_ON(!crtc_state)) {
		ret = -EINVAL;
		goto exit;
	}

	if (!crtc_state->active) {
		ret = -EINVAL;
		goto exit;
	}

	mdp5_state = to_mdp5_plane_state(new_plane_state);

	/* don't use fast path if we don't have a hwpipe allocated yet */
	if (!mdp5_state->hwpipe) {
		ret = -EINVAL;
		goto exit;
	}

	/* only allow changing of position(crtc x/y or src x/y) in fast path */
	if (plane->state->crtc != new_plane_state->crtc ||
	    plane->state->src_w != new_plane_state->src_w ||
	    plane->state->src_h != new_plane_state->src_h ||
	    plane->state->crtc_w != new_plane_state->crtc_w ||
	    plane->state->crtc_h != new_plane_state->crtc_h ||
	    !plane->state->fb ||
	    plane->state->fb != new_plane_state->fb) {
		ret = -EINVAL;
		goto exit;
	}

	min_scale = FRAC_16_16(1, 8);
	max_scale = FRAC_16_16(8, 1);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  min_scale, max_scale,
						  true, true);
	if (ret)
		goto exit;

	/*
	 * if the visibility of the plane changes (i.e, if the cursor is
	 * clipped out completely, we can't take the async path because
	 * we need to stage/unstage the plane from the Layer Mixer(s). We
	 * also assign/unassign the hwpipe(s) tied to the plane. We avoid
	 * taking the fast path for both these reasons.
	 */
	if (new_plane_state->visible != plane->state->visible) {
		ret = -EINVAL;
		goto exit;
	}
	MDP5_PLANE_DBG("async_check plane=%s pos=(%d,%d)", plane->name,
		new_plane_state->crtc_x, new_plane_state->crtc_y);

	ret = 0;

exit:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void mdp5_plane_atomic_async_update(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct drm_framebuffer *old_fb = plane->state->fb;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	plane->state->src_x = new_state->src_x;
	plane->state->src_y = new_state->src_y;
	plane->state->crtc_x = new_state->crtc_x;
	plane->state->crtc_y = new_state->crtc_y;
	MDP5_PLANE_DBG("async_update plane=%s -> src=(%d,%d) crtc=(%d,%d)",
		plane->name,
		plane->state->src_x >> 16, plane->state->src_y >> 16,
		plane->state->crtc_x, plane->state->crtc_y);

	if (plane_enabled(new_state)) {
		struct mdp5_ctl *ctl;
		struct mdp5_pipeline *pipeline =
					mdp5_crtc_get_pipeline(new_state->crtc);
		int ret;

		ret = mdp5_plane_mode_set(plane, new_state->crtc, new_state->fb,
				&new_state->src, &new_state->dst);
		WARN_ON(ret < 0);
		if (ret)
			MDP5_PLANE_DBG("async_update mode_set ret=%d", ret);

		ctl = mdp5_crtc_get_ctl(new_state->crtc);

		{
			u32 flush = mdp5_plane_get_flush(plane);
			mdp5_ctl_commit(ctl, pipeline, flush, true);
			MDP5_PLANE_DBG("async_commit plane=%s flush=0x%08x", plane->name, flush);
		}
	}

	*to_mdp5_plane_state(plane->state) =
	*to_mdp5_plane_state(new_state);
	MDP5_PLANE_DBG("async_update done plane=%s old_fb=%u", plane->name,
		old_fb ? old_fb->base.id : 0);

	new_state->fb = old_fb;
	MSM_FUNC_EXIT("");
}

static const struct drm_plane_helper_funcs mdp5_plane_helper_funcs = {
		.prepare_fb = msm_atomic_prepare_fb,
		.cleanup_fb = mdp5_plane_cleanup_fb,
		.atomic_check = mdp5_plane_atomic_check,
		.atomic_update = mdp5_plane_atomic_update,
		.atomic_async_check = mdp5_plane_atomic_async_check,
		.atomic_async_update = mdp5_plane_atomic_async_update,
};

static void set_scanout_locked(struct mdp5_kms *mdp5_kms,
			       enum mdp5_pipe pipe,
			       struct drm_framebuffer *fb)
{
	struct msm_kms *kms = &mdp5_kms->base.base;
	MDP5_PLANE_DBG("set_scanout pipe=%d fb=%u", pipe, fb->base.id);

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_STRIDE_A(pipe),
			MDP5_PIPE_SRC_STRIDE_A_P0(fb->pitches[0]) |
			MDP5_PIPE_SRC_STRIDE_A_P1(fb->pitches[1]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_STRIDE_B(pipe),
			MDP5_PIPE_SRC_STRIDE_B_P2(fb->pitches[2]) |
			MDP5_PIPE_SRC_STRIDE_B_P3(fb->pitches[3]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC0_ADDR(pipe),
			msm_framebuffer_iova(fb, kms->aspace, 0));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC1_ADDR(pipe),
			msm_framebuffer_iova(fb, kms->aspace, 1));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC2_ADDR(pipe),
			msm_framebuffer_iova(fb, kms->aspace, 2));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC3_ADDR(pipe),
			msm_framebuffer_iova(fb, kms->aspace, 3));
}

/* Note: mdp5_plane->pipe_lock must be locked */
static void csc_disable(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe)
{
	uint32_t value = mdp5_read(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe)) &
			 ~MDP5_PIPE_OP_MODE_CSC_1_EN;

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe), value);
	MDP5_PLANE_DBG("csc_disable pipe=%d", pipe);
}

/* Note: mdp5_plane->pipe_lock must be locked */
static void csc_enable(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe,
		struct csc_cfg *csc)
{
	uint32_t  i, mode = 0; /* RGB, no CSC */
	uint32_t *matrix;

	if (unlikely(!csc))
		return;

	if ((csc->type == CSC_YUV2RGB) || (CSC_YUV2YUV == csc->type))
		mode |= MDP5_PIPE_OP_MODE_CSC_SRC_DATA_FORMAT(DATA_FORMAT_YUV);
	if ((csc->type == CSC_RGB2YUV) || (CSC_YUV2YUV == csc->type))
		mode |= MDP5_PIPE_OP_MODE_CSC_DST_DATA_FORMAT(DATA_FORMAT_YUV);
	mode |= MDP5_PIPE_OP_MODE_CSC_1_EN;
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe), mode);
	MDP5_PLANE_DBG("csc_enable pipe=%d mode=0x%08x", pipe, mode);

	matrix = csc->matrix;
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_0(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_0_COEFF_11(matrix[0]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_0_COEFF_12(matrix[1]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_1(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_1_COEFF_13(matrix[2]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_1_COEFF_21(matrix[3]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_2(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_2_COEFF_22(matrix[4]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_2_COEFF_23(matrix[5]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_3(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_3_COEFF_31(matrix[6]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_3_COEFF_32(matrix[7]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_4(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_4_COEFF_33(matrix[8]));

	for (i = 0; i < ARRAY_SIZE(csc->pre_bias); i++) {
		uint32_t *pre_clamp = csc->pre_clamp;
		uint32_t *post_clamp = csc->post_clamp;

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_PRE_CLAMP(pipe, i),
			MDP5_PIPE_CSC_1_PRE_CLAMP_REG_HIGH(pre_clamp[2*i+1]) |
			MDP5_PIPE_CSC_1_PRE_CLAMP_REG_LOW(pre_clamp[2*i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_POST_CLAMP(pipe, i),
			MDP5_PIPE_CSC_1_POST_CLAMP_REG_HIGH(post_clamp[2*i+1]) |
			MDP5_PIPE_CSC_1_POST_CLAMP_REG_LOW(post_clamp[2*i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_PRE_BIAS(pipe, i),
			MDP5_PIPE_CSC_1_PRE_BIAS_REG_VALUE(csc->pre_bias[i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_POST_BIAS(pipe, i),
			MDP5_PIPE_CSC_1_POST_BIAS_REG_VALUE(csc->post_bias[i]));
	}
}

#define PHASE_STEP_SHIFT	21
#define DOWN_SCALE_RATIO_MAX	32	/* 2^(26-21) */

static int calc_phase_step(uint32_t src, uint32_t dst, uint32_t *out_phase)
{
	uint32_t unit;

	if (src == 0 || dst == 0)
		return -EINVAL;

	/*
	 * PHASE_STEP_X/Y is coded on 26 bits (25:0),
	 * where 2^21 represents the unity "1" in fixed-point hardware design.
	 * This leaves 5 bits for the integer part (downscale case):
	 *	-> maximum downscale ratio = 0b1_1111 = 31
	 */
	if (src > (dst * DOWN_SCALE_RATIO_MAX))
		return -EOVERFLOW;

	unit = 1 << PHASE_STEP_SHIFT;
	*out_phase = mult_frac(unit, src, dst);

	return 0;
}

static int calc_scalex_steps(struct drm_plane *plane,
		uint32_t pixel_format, uint32_t src, uint32_t dest,
		uint32_t phasex_steps[COMP_MAX])
{
	const struct drm_format_info *info = drm_format_info(pixel_format);
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct device *dev = mdp5_kms->dev->dev;
	uint32_t phasex_step;
	int ret;

	ret = calc_phase_step(src, dest, &phasex_step);
	if (ret) {
		DRM_DEV_ERROR(dev, "X scaling (%d->%d) failed: %d\n", src, dest, ret);
		return ret;
	}

	phasex_steps[COMP_0]   = phasex_step;
	phasex_steps[COMP_3]   = phasex_step;
	phasex_steps[COMP_1_2] = phasex_step / info->hsub;

	return 0;
}

static int calc_scaley_steps(struct drm_plane *plane,
		uint32_t pixel_format, uint32_t src, uint32_t dest,
		uint32_t phasey_steps[COMP_MAX])
{
	const struct drm_format_info *info = drm_format_info(pixel_format);
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct device *dev = mdp5_kms->dev->dev;
	uint32_t phasey_step;
	int ret;

	ret = calc_phase_step(src, dest, &phasey_step);
	if (ret) {
		DRM_DEV_ERROR(dev, "Y scaling (%d->%d) failed: %d\n", src, dest, ret);
		return ret;
	}

	phasey_steps[COMP_0]   = phasey_step;
	phasey_steps[COMP_3]   = phasey_step;
	phasey_steps[COMP_1_2] = phasey_step / info->vsub;

	return 0;
}

static uint32_t get_scale_config(const struct mdp_format *format,
		uint32_t src, uint32_t dst, bool horz)
{
	const struct drm_format_info *info = drm_format_info(format->base.pixel_format);
	bool scaling = format->is_yuv ? true : (src != dst);
	uint32_t sub;
	uint32_t ya_filter, uv_filter;
	bool yuv = format->is_yuv;

	if (!scaling)
		return 0;

	if (yuv) {
		sub = horz ? info->hsub : info->vsub;
		uv_filter = ((src / sub) <= dst) ?
				   SCALE_FILTER_BIL : SCALE_FILTER_PCMN;
	}
	ya_filter = (src <= dst) ? SCALE_FILTER_BIL : SCALE_FILTER_PCMN;

	if (horz)
		return  MDP5_PIPE_SCALE_CONFIG_SCALEX_EN |
			MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_0(ya_filter) |
			MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_3(ya_filter) |
			COND(yuv, MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_1_2(uv_filter));
	else
		return  MDP5_PIPE_SCALE_CONFIG_SCALEY_EN |
			MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_0(ya_filter) |
			MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_3(ya_filter) |
			COND(yuv, MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_1_2(uv_filter));
}

static void calc_pixel_ext(const struct mdp_format *format,
		uint32_t src, uint32_t dst, uint32_t phase_step[2],
		int pix_ext_edge1[COMP_MAX], int pix_ext_edge2[COMP_MAX],
		bool horz)
{
	bool scaling = format->is_yuv ? true : (src != dst);
	int i;

	/*
	 * Note:
	 * We assume here that:
	 *     1. PCMN filter is used for downscale
	 *     2. bilinear filter is used for upscale
	 *     3. we are in a single pipe configuration
	 */

	for (i = 0; i < COMP_MAX; i++) {
		pix_ext_edge1[i] = 0;
		pix_ext_edge2[i] = scaling ? 1 : 0;
	}
}

static void mdp5_write_pixel_ext(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe,
	const struct mdp_format *format,
	uint32_t src_w, int pe_left[COMP_MAX], int pe_right[COMP_MAX],
	uint32_t src_h, int pe_top[COMP_MAX], int pe_bottom[COMP_MAX])
{
	const struct drm_format_info *info = drm_format_info(format->base.pixel_format);
	uint32_t lr, tb, req;
	int i;

	for (i = 0; i < COMP_MAX; i++) {
		uint32_t roi_w = src_w;
		uint32_t roi_h = src_h;

		if (format->is_yuv && i == COMP_1_2) {
			roi_w /= info->hsub;
			roi_h /= info->vsub;
		}

		lr  = (pe_left[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_LR_LEFT_RPT(pe_left[i]) :
			MDP5_PIPE_SW_PIX_EXT_LR_LEFT_OVF(pe_left[i]);

		lr |= (pe_right[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_RPT(pe_right[i]) :
			MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_OVF(pe_right[i]);

		tb  = (pe_top[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_TB_TOP_RPT(pe_top[i]) :
			MDP5_PIPE_SW_PIX_EXT_TB_TOP_OVF(pe_top[i]);

		tb |= (pe_bottom[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_RPT(pe_bottom[i]) :
			MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_OVF(pe_bottom[i]);

		req  = MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_LEFT_RIGHT(roi_w +
				pe_left[i] + pe_right[i]);

		req |= MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_TOP_BOTTOM(roi_h +
				pe_top[i] + pe_bottom[i]);

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_LR(pipe, i), lr);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_TB(pipe, i), tb);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS(pipe, i), req);

		DBG("comp-%d (L/R): rpt=%d/%d, ovf=%d/%d, req=%d", i,
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_LEFT_RPT),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_RPT),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_LEFT_OVF),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_OVF),
			FIELD(req, MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_LEFT_RIGHT));

		DBG("comp-%d (T/B): rpt=%d/%d, ovf=%d/%d, req=%d", i,
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_TOP_RPT),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_RPT),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_TOP_OVF),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_OVF),
			FIELD(req, MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_TOP_BOTTOM));
	}
}

struct pixel_ext {
	int left[COMP_MAX];
	int right[COMP_MAX];
	int top[COMP_MAX];
	int bottom[COMP_MAX];
};

struct phase_step {
	u32 x[COMP_MAX];
	u32 y[COMP_MAX];
};

static void mdp5_hwpipe_mode_set(struct mdp5_kms *mdp5_kms,
				 struct mdp5_hw_pipe *hwpipe,
				 struct drm_framebuffer *fb,
				 struct phase_step *step,
				 struct pixel_ext *pe,
				 u32 scale_config, u32 hdecm, u32 vdecm,
				 bool hflip, bool vflip,
				 int crtc_x, int crtc_y,
				 unsigned int crtc_w, unsigned int crtc_h,
				 u32 src_img_w, u32 src_img_h,
				 u32 src_x, u32 src_y,
				 u32 src_w, u32 src_h)
{
	enum mdp5_pipe pipe = hwpipe->pipe;
	bool has_pe = hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT;
	const struct mdp_format *format =
			to_mdp_format(msm_framebuffer_format(fb));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_IMG_SIZE(pipe),
			MDP5_PIPE_SRC_IMG_SIZE_WIDTH(src_img_w) |
			MDP5_PIPE_SRC_IMG_SIZE_HEIGHT(src_img_h));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_SIZE(pipe),
			MDP5_PIPE_SRC_SIZE_WIDTH(src_w) |
			MDP5_PIPE_SRC_SIZE_HEIGHT(src_h));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_XY(pipe),
			MDP5_PIPE_SRC_XY_X(src_x) |
			MDP5_PIPE_SRC_XY_Y(src_y));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OUT_SIZE(pipe),
			MDP5_PIPE_OUT_SIZE_WIDTH(crtc_w) |
			MDP5_PIPE_OUT_SIZE_HEIGHT(crtc_h));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OUT_XY(pipe),
			MDP5_PIPE_OUT_XY_X(crtc_x) |
			MDP5_PIPE_OUT_XY_Y(crtc_y));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_FORMAT(pipe),
			MDP5_PIPE_SRC_FORMAT_A_BPC(format->bpc_a) |
			MDP5_PIPE_SRC_FORMAT_R_BPC(format->bpc_r) |
			MDP5_PIPE_SRC_FORMAT_G_BPC(format->bpc_g) |
			MDP5_PIPE_SRC_FORMAT_B_BPC(format->bpc_b) |
			COND(format->alpha_enable, MDP5_PIPE_SRC_FORMAT_ALPHA_ENABLE) |
			MDP5_PIPE_SRC_FORMAT_CPP(format->cpp - 1) |
			MDP5_PIPE_SRC_FORMAT_UNPACK_COUNT(format->unpack_count - 1) |
			COND(format->unpack_tight, MDP5_PIPE_SRC_FORMAT_UNPACK_TIGHT) |
			MDP5_PIPE_SRC_FORMAT_FETCH_TYPE(format->fetch_type) |
			MDP5_PIPE_SRC_FORMAT_CHROMA_SAMP(format->chroma_sample));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_UNPACK(pipe),
			MDP5_PIPE_SRC_UNPACK_ELEM0(format->unpack[0]) |
			MDP5_PIPE_SRC_UNPACK_ELEM1(format->unpack[1]) |
			MDP5_PIPE_SRC_UNPACK_ELEM2(format->unpack[2]) |
			MDP5_PIPE_SRC_UNPACK_ELEM3(format->unpack[3]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_OP_MODE(pipe),
			(hflip ? MDP5_PIPE_SRC_OP_MODE_FLIP_LR : 0) |
			(vflip ? MDP5_PIPE_SRC_OP_MODE_FLIP_UD : 0) |
			COND(has_pe, MDP5_PIPE_SRC_OP_MODE_SW_PIX_EXT_OVERRIDE) |
			MDP5_PIPE_SRC_OP_MODE_BWC(BWC_LOSSLESS));

	/* not using secure mode: */
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_ADDR_SW_STATUS(pipe), 0);

	if (hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT)
		mdp5_write_pixel_ext(mdp5_kms, pipe, format,
				src_w, pe->left, pe->right,
				src_h, pe->top, pe->bottom);

	if (hwpipe->caps & MDP_PIPE_CAP_SCALE) {
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_PHASE_STEP_X(pipe),
				step->x[COMP_0]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_PHASE_STEP_Y(pipe),
				step->y[COMP_0]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CR_PHASE_STEP_X(pipe),
				step->x[COMP_1_2]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CR_PHASE_STEP_Y(pipe),
				step->y[COMP_1_2]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_DECIMATION(pipe),
				MDP5_PIPE_DECIMATION_VERT(vdecm) |
				MDP5_PIPE_DECIMATION_HORZ(hdecm));
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CONFIG(pipe),
			   scale_config);
	}

	if (hwpipe->caps & MDP_PIPE_CAP_CSC) {
		if (MDP_FORMAT_IS_YUV(format))
			csc_enable(mdp5_kms, pipe,
					mdp_get_default_csc_cfg(CSC_YUV2RGB));
		else
			csc_disable(mdp5_kms, pipe);
	}

	set_scanout_locked(mdp5_kms, pipe, fb);
}

static int mdp5_plane_mode_set(struct drm_plane *plane,
		struct drm_crtc *crtc, struct drm_framebuffer *fb,
		struct drm_rect *src, struct drm_rect *dest)
{
	struct drm_plane_state *pstate = plane->state;
	struct mdp5_hw_pipe *hwpipe = to_mdp5_plane_state(pstate)->hwpipe;
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	enum mdp5_pipe pipe = hwpipe->pipe;
	struct mdp5_hw_pipe *right_hwpipe;
	const struct mdp_format *format;
	uint32_t nplanes, config = 0;
	struct phase_step step = { { 0 } };
	struct pixel_ext pe = { { 0 } };
	uint32_t hdecm = 0, vdecm = 0;
	uint32_t pix_format;
	unsigned int rotation;
	bool vflip, hflip;
	int crtc_x, crtc_y;
	unsigned int crtc_w, crtc_h;
	uint32_t src_x, src_y;
	uint32_t src_w, src_h;
	uint32_t src_img_w, src_img_h;
	int ret;

	MSM_FUNC_ENTER("plane=%s fb=%u", plane->name, fb ? fb->base.id : 0);
	nplanes = fb->format->num_planes;

	/* bad formats should already be rejected: */
	if (WARN_ON(nplanes > pipe2nclients(pipe))) {
		ret = -EINVAL;
		goto exit;
	}

	format = to_mdp_format(msm_framebuffer_format(fb));
	pix_format = format->base.pixel_format;

	src_x = src->x1;
	src_y = src->y1;
	src_w = drm_rect_width(src);
	src_h = drm_rect_height(src);

	crtc_x = dest->x1;
	crtc_y = dest->y1;
	crtc_w = drm_rect_width(dest);
	crtc_h = drm_rect_height(dest);

	/* src values are in Q16 fixed point, convert to integer: */
	src_x = src_x >> 16;
	src_y = src_y >> 16;
	src_w = src_w >> 16;
	src_h = src_h >> 16;

	src_img_w = min(fb->width, src_w);
	src_img_h = min(fb->height, src_h);

	DBG("%s: FB[%u] %u,%u,%u,%u -> CRTC[%u] %d,%d,%u,%u", plane->name,
			fb->base.id, src_x, src_y, src_w, src_h,
			crtc->base.id, crtc_x, crtc_y, crtc_w, crtc_h);
	MDP5_PLANE_DBG("mode_set plane=%s fb=%u hwpipe=%s right=%s", plane->name,
		fb->base.id, hwpipe->name,
		right_hwpipe ? right_hwpipe->name : "(null)");

	right_hwpipe = to_mdp5_plane_state(pstate)->r_hwpipe;
	if (right_hwpipe) {
		/*
		 * if the plane comprises of 2 hw pipes, assume that the width
		 * is split equally across them. The only parameters that varies
		 * between the 2 pipes are src_x and crtc_x
		 */
		crtc_w /= 2;
		src_w /= 2;
		src_img_w /= 2;
	}

	ret = calc_scalex_steps(plane, pix_format, src_w, crtc_w, step.x);
	if (ret)
		goto exit;

	ret = calc_scaley_steps(plane, pix_format, src_h, crtc_h, step.y);
	if (ret)
		goto exit;

	if (hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT) {
		calc_pixel_ext(format, src_w, crtc_w, step.x,
			       pe.left, pe.right, true);
		calc_pixel_ext(format, src_h, crtc_h, step.y,
			       pe.top, pe.bottom, false);
	}

	/* TODO calc hdecm, vdecm */

	/* SCALE is used to both scale and up-sample chroma components */
	config |= get_scale_config(format, src_w, crtc_w, true);
	config |= get_scale_config(format, src_h, crtc_h, false);
	DBG("scale config = %x", config);
	MDP5_PLANE_DBG("scale plane=%s config=0x%08x hdecm=%u vdecm=%u", plane->name,
		config, hdecm, vdecm);

	rotation = drm_rotation_simplify(pstate->rotation,
					 DRM_MODE_ROTATE_0 |
					 DRM_MODE_REFLECT_X |
					 DRM_MODE_REFLECT_Y);
	hflip = !!(rotation & DRM_MODE_REFLECT_X);
	vflip = !!(rotation & DRM_MODE_REFLECT_Y);

	mdp5_hwpipe_mode_set(mdp5_kms, hwpipe, fb, &step, &pe,
			     config, hdecm, vdecm, hflip, vflip,
			     crtc_x, crtc_y, crtc_w, crtc_h,
			     src_img_w, src_img_h,
			     src_x, src_y, src_w, src_h);
	if (right_hwpipe)
		mdp5_hwpipe_mode_set(mdp5_kms, right_hwpipe, fb, &step, &pe,
				     config, hdecm, vdecm, hflip, vflip,
				     crtc_x + crtc_w, crtc_y, crtc_w, crtc_h,
				     src_img_w, src_img_h,
				     src_x + src_w, src_y, src_w, src_h);

	ret = 0;

exit:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

/*
 * Use this func and the one below only after the atomic state has been
 * successfully swapped
 */
enum mdp5_pipe mdp5_plane_pipe(struct drm_plane *plane)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(plane->state);
	enum mdp5_pipe pipe_id;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	if (WARN_ON(!pstate->hwpipe)) {
		pipe_id = SSPP_NONE;
		goto exit;
	}
	pipe_id = pstate->hwpipe->pipe;
	MDP5_PLANE_DBG("plane_pipe plane=%s pipe=%d", plane->name, pipe_id);

exit:
	MSM_FUNC_EXIT("pipe=%d", pipe_id);
	return pipe_id;
}

enum mdp5_pipe mdp5_plane_right_pipe(struct drm_plane *plane)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(plane->state);
	enum mdp5_pipe pipe_id;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	if (!pstate->r_hwpipe) {
		pipe_id = SSPP_NONE;
		goto exit;
	}
	pipe_id = pstate->r_hwpipe->pipe;
	MDP5_PLANE_DBG("plane_right_pipe plane=%s pipe=%d", plane->name, pipe_id);

exit:
	MSM_FUNC_EXIT("pipe=%d", pipe_id);
	return pipe_id;
}

uint32_t mdp5_plane_get_flush(struct drm_plane *plane)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(plane->state);
	u32 mask;

	MSM_FUNC_ENTER("plane=%s", plane->name);
	if (WARN_ON(!pstate->hwpipe)) {
		mask = 0;
		goto exit;
	}

	mask = pstate->hwpipe->flush_mask;

	if (pstate->r_hwpipe)
		mask |= pstate->r_hwpipe->flush_mask;

exit:
	MSM_FUNC_EXIT("mask=0x%08x", mask);
	return mask;
}

/* initialize plane */
struct drm_plane *mdp5_plane_init(struct drm_device *dev,
				  enum drm_plane_type type)
{
	struct drm_plane *plane = NULL;
	struct mdp5_plane *mdp5_plane;
	int ret;

	MSM_FUNC_ENTER("type=%d", type);
	mdp5_plane = kzalloc(sizeof(*mdp5_plane), GFP_KERNEL);
	if (!mdp5_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	plane = &mdp5_plane->base;

	mdp5_plane->nformats = mdp_get_formats(mdp5_plane->formats,
		ARRAY_SIZE(mdp5_plane->formats), false);

	ret = drm_universal_plane_init(dev, plane, 0xff, &mdp5_plane_funcs,
			mdp5_plane->formats, mdp5_plane->nformats,
			NULL, type, NULL);
	if (ret)
		goto fail;

	drm_plane_helper_add(plane, &mdp5_plane_helper_funcs);

	mdp5_plane_install_properties(plane, &plane->base);

	drm_plane_enable_fb_damage_clips(plane);

	MSM_FUNC_EXIT("plane=%p", plane);
	return plane;

fail:
	if (plane)
		mdp5_plane_destroy(plane);

	MSM_FUNC_EXIT("err=%d", ret);
	return ERR_PTR(ret);
}

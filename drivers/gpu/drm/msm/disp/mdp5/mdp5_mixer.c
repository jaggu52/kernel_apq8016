// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 The Linux Foundation. All rights reserved.
 */

#include "mdp5_kms.h"

/*
 * As of now, there are only 2 combinations possible for source split:
 *
 * Left | Right
 * -----|------
 *  LM0 | LM1
 *  LM2 | LM5
 *
 */
static int lm_right_pair[] = { 1, -1, 5, -1, -1, -1 };

static int get_right_pair_idx(struct mdp5_kms *mdp5_kms, int lm)
{
	int i;
	int pair_lm;

	MSM_FUNC_ENTER("[MIXER] lm=%d", lm);
	pair_lm = lm_right_pair[lm];
	if (pair_lm < 0) {
		MSM_FUNC_EXIT("[MIXER] ret=%d", -EINVAL);
		return -EINVAL;
	}

	for (i = 0; i < mdp5_kms->num_hwmixers; i++) {
		struct mdp5_hw_mixer *mixer = mdp5_kms->hwmixers[i];

		if (mixer->lm == pair_lm) {
			MSM_FUNC_EXIT("[MIXER] ret=%d", mixer->idx);
			return mixer->idx;
		}
	}

	MSM_FUNC_EXIT("[MIXER] ret=%d", -1);
	return -1;
}

int mdp5_mixer_assign(struct drm_atomic_state *s, struct drm_crtc *crtc,
		      uint32_t caps, struct mdp5_hw_mixer **mixer,
		      struct mdp5_hw_mixer **r_mixer)
{
	struct msm_drm_private *priv = s->dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct mdp5_global_state *global_state = mdp5_get_global_state(s);
	struct mdp5_hw_mixer_state *new_state;
	struct mdp5_hw_mixer *best = NULL;
	struct mdp5_hw_mixer *best_right = NULL;
	int i;
	int ret = 0;

	MSM_FUNC_ENTER("[MIXER] crtc=%s caps=0x%08x", crtc->name, caps);
	if (IS_ERR(global_state)) {
		ret = PTR_ERR(global_state);
		goto out;
	}

	new_state = &global_state->hwmixer;

	for (i = 0; i < mdp5_kms->num_hwmixers; i++) {
		struct mdp5_hw_mixer *cur = mdp5_kms->hwmixers[i];
		struct mdp5_hw_mixer *candidate_right = NULL;
		int pair_idx;

		if (new_state->hwmixer_to_crtc[cur->idx] &&
		    new_state->hwmixer_to_crtc[cur->idx] != crtc)
			continue;

		if (caps & ~cur->caps)
			continue;

		if (r_mixer) {
			pair_idx = get_right_pair_idx(mdp5_kms, cur->lm);
			if (pair_idx < 0) {
				ret = -EINVAL;
				goto out;
			}
			if (new_state->hwmixer_to_crtc[pair_idx])
				continue;

			candidate_right = mdp5_kms->hwmixers[pair_idx];
		}

		if (!best || (cur->caps & MDP_LM_CAP_PAIR)) {
			best = cur;
			best_right = candidate_right;
		}
	}

	if (!best) {
		ret = -ENOMEM;
		goto out;
	}

	if (r_mixer && !best_right) {
		ret = -ENOMEM;
		goto out;
	}

	new_state->hwmixer_to_crtc[best->idx] = crtc;
	*mixer = best;
	DBG("assigning Layer Mixer %d to crtc %s", best->lm, crtc->name);

	if (r_mixer) {
		DBG("assigning Right Layer Mixer %d to crtc %s", best_right->lm, crtc->name);
		new_state->hwmixer_to_crtc[best_right->idx] = crtc;
		*r_mixer = best_right;
	}

	MSM_FUNC_EXIT("[MIXER] assigned mixer=%s", best->name);
	return 0;

out:
	MSM_FUNC_EXIT("[MIXER] ret=%d", ret);
	return ret;
}

void mdp5_mixer_release(struct drm_atomic_state *s, struct mdp5_hw_mixer *mixer)
{
	struct mdp5_global_state *global_state = mdp5_get_global_state(s);
	struct mdp5_hw_mixer_state *new_state = &global_state->hwmixer;

	MSM_FUNC_ENTER("[MIXER] mixer=%s", mixer ? mixer->name : "(null)");
	if (!mixer)
		goto out;

	if (WARN_ON(!new_state->hwmixer_to_crtc[mixer->idx]))
		goto out;

	DBG("%s: release from crtc %s", mixer->name,
	    new_state->hwmixer_to_crtc[mixer->idx]->name);

	new_state->hwmixer_to_crtc[mixer->idx] = NULL;
out:
	MSM_FUNC_EXIT("[MIXER] mixer=%s", mixer ? mixer->name : "(null)");
}

void mdp5_mixer_destroy(struct mdp5_hw_mixer *mixer)
{
	MSM_FUNC_ENTER("[MIXER] mixer=%p", mixer);
	kfree(mixer);
	MSM_FUNC_EXIT("[MIXER] mixer=%p", mixer);
}

static const char * const mixer_names[] = {
	"LM0", "LM1", "LM2", "LM3", "LM4", "LM5",
};

struct mdp5_hw_mixer *mdp5_mixer_init(const struct mdp5_lm_instance *lm)
{
	struct mdp5_hw_mixer *mixer;
	MSM_FUNC_ENTER("[MIXER] lm=%d", lm->id);

	mixer = kzalloc(sizeof(*mixer), GFP_KERNEL);
	if (!mixer)
		goto fail;

	mixer->name = mixer_names[lm->id];
	mixer->lm = lm->id;
	mixer->caps = lm->caps;
	mixer->pp = lm->pp;
	mixer->dspp = lm->dspp;
	mixer->flush_mask = mdp_ctl_flush_mask_lm(lm->id);
	MSM_FUNC_EXIT("[MIXER] mixer=%s", mixer->name);
	return mixer;

fail:
	MSM_FUNC_EXIT("[MIXER] mixer=NULL ret=-ENOMEM");
	return ERR_PTR(-ENOMEM);
}

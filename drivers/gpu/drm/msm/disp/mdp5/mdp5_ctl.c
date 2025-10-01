// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2015 The Linux Foundation. All rights reserved.
 */

#include "mdp5_kms.h"
#include "mdp5_ctl.h"

/*
 * CTL - MDP Control Pool Manager
 *
 * Controls are shared between all display interfaces.
 *
 * They are intended to be used for data path configuration.
 * The top level register programming describes the complete data path for
 * a specific data path ID - REG_MDP5_CTL_*(<id>, ...)
 *
 * Hardware capabilities determine the number of concurrent data paths
 *
 * In certain use cases (high-resolution dual pipe), one single CTL can be
 * shared across multiple CRTCs.
 */

#define CTL_STAT_BUSY		0x1
#define CTL_STAT_BOOKED	0x2

struct mdp5_ctl {
	struct mdp5_ctl_manager *ctlm;

	u32 id;

	/* CTL status bitmask */
	u32 status;

	bool encoder_enabled;

	/* pending flush_mask bits */
	u32 flush_mask;

	/* REG_MDP5_CTL_*(<id>) registers access info + lock: */
	spinlock_t hw_lock;
	u32 reg_offset;

	/* when do CTL registers need to be flushed? (mask of trigger bits) */
	u32 pending_ctl_trigger;

	bool cursor_on;

	/* True if the current CTL has FLUSH bits pending for single FLUSH. */
	bool flush_pending;

	struct mdp5_ctl *pair; /* Paired CTL to be flushed together */
};

struct mdp5_ctl_manager {
	struct drm_device *dev;

	/* number of CTL / Layer Mixers in this hw config: */
	u32 nlm;
	u32 nctl;

	/* to filter out non-present bits in the current hardware config */
	u32 flush_hw_mask;

	/* status for single FLUSH */
	bool single_flush_supported;
	u32 single_flush_pending_mask;

	/* pool of CTLs + lock to protect resource allocation (ctls[i].busy) */
	spinlock_t pool_lock;
	struct mdp5_ctl ctls[MAX_CTL];
};

static inline
struct mdp5_kms *get_kms(struct mdp5_ctl_manager *ctl_mgr)
{
	struct msm_drm_private *priv = ctl_mgr->dev->dev_private;
	MDP5_CTL_DBG("get_kms ctl_mgr=%p kms=%p", ctl_mgr, priv->kms);

	return to_mdp5_kms(to_mdp_kms(priv->kms));
}

static inline
void ctl_write(struct mdp5_ctl *ctl, u32 reg, u32 data)
{
	struct mdp5_kms *mdp5_kms = get_kms(ctl->ctlm);
	MDP5_CTL_DBG("ctl_write ctl=%u reg=0x%08x data=0x%08x", ctl->id, reg, data);

	(void)ctl->reg_offset; /* TODO use this instead of mdp5_write */
	mdp5_write(mdp5_kms, reg, data);
}

static inline
u32 ctl_read(struct mdp5_ctl *ctl, u32 reg)
{
	struct mdp5_kms *mdp5_kms = get_kms(ctl->ctlm);

	(void)ctl->reg_offset; /* TODO use this instead of mdp5_write */
	u32 val = mdp5_read(mdp5_kms, reg);
	MDP5_CTL_DBG("ctl_read ctl=%u reg=0x%08x val=0x%08x", ctl->id, reg, val);
	return val;
}

static void set_display_intf(struct mdp5_kms *mdp5_kms,
		struct mdp5_interface *intf)
{
	unsigned long flags;
	u32 intf_sel;

	MDP5_CTL_DBG("set_display_intf num=%d type=%d mode=%d", intf->num,
		intf->type, intf->mode);

	spin_lock_irqsave(&mdp5_kms->resource_lock, flags);
	intf_sel = mdp5_read(mdp5_kms, REG_MDP5_DISP_INTF_SEL);

	switch (intf->num) {
	case 0:
		intf_sel &= ~MDP5_DISP_INTF_SEL_INTF0__MASK;
		intf_sel |= MDP5_DISP_INTF_SEL_INTF0(intf->type);
		break;
	case 1:
		intf_sel &= ~MDP5_DISP_INTF_SEL_INTF1__MASK;
		intf_sel |= MDP5_DISP_INTF_SEL_INTF1(intf->type);
		break;
	case 2:
		intf_sel &= ~MDP5_DISP_INTF_SEL_INTF2__MASK;
		intf_sel |= MDP5_DISP_INTF_SEL_INTF2(intf->type);
		break;
	case 3:
		intf_sel &= ~MDP5_DISP_INTF_SEL_INTF3__MASK;
		intf_sel |= MDP5_DISP_INTF_SEL_INTF3(intf->type);
		break;
	default:
		BUG();
		break;
	}

	mdp5_write(mdp5_kms, REG_MDP5_DISP_INTF_SEL, intf_sel);
	spin_unlock_irqrestore(&mdp5_kms->resource_lock, flags);
	MDP5_CTL_DBG("set_display_intf sel=0x%08x", intf_sel);
}

static void set_ctl_op(struct mdp5_ctl *ctl, struct mdp5_pipeline *pipeline)
{
	unsigned long flags;
	struct mdp5_interface *intf = pipeline->intf;
	u32 ctl_op = 0;

	MDP5_CTL_DBG("set_ctl_op ctl=%u intf_type=%d num=%d r_mixer=%p", ctl->id,
		intf->type, intf->num, pipeline->r_mixer);

	if (!mdp5_cfg_intf_is_virtual(intf->type))
		ctl_op |= MDP5_CTL_OP_INTF_NUM(INTF0 + intf->num);

	switch (intf->type) {
	case INTF_DSI:
		if (intf->mode == MDP5_INTF_DSI_MODE_COMMAND)
			ctl_op |= MDP5_CTL_OP_CMD_MODE;
		break;

	case INTF_WB:
		if (intf->mode == MDP5_INTF_WB_MODE_LINE)
			ctl_op |= MDP5_CTL_OP_MODE(MODE_WB_2_LINE);
		break;

	default:
		break;
	}

	if (pipeline->r_mixer)
		ctl_op |= MDP5_CTL_OP_PACK_3D_ENABLE |
			  MDP5_CTL_OP_PACK_3D(1);

	spin_lock_irqsave(&ctl->hw_lock, flags);
	ctl_write(ctl, REG_MDP5_CTL_OP(ctl->id), ctl_op);
	spin_unlock_irqrestore(&ctl->hw_lock, flags);
	MDP5_CTL_DBG("set_ctl_op ctl=%u ctl_op=0x%08x", ctl->id, ctl_op);
}

int mdp5_ctl_set_pipeline(struct mdp5_ctl *ctl, struct mdp5_pipeline *pipeline)
{
	struct mdp5_kms *mdp5_kms = get_kms(ctl->ctlm);
	struct mdp5_interface *intf = pipeline->intf;

	MSM_FUNC_ENTER("ctl=%u intf=%d", ctl->id, intf->num);
	MDP5_CTL_DBG("set_pipeline ctl=%u intf=%d r_mixer=%p", ctl->id,
		intf->num, pipeline->r_mixer);

	/* Virtual interfaces need not set a display intf (e.g.: Writeback) */
	if (!mdp5_cfg_intf_is_virtual(intf->type))
		set_display_intf(mdp5_kms, intf);

	set_ctl_op(ctl, pipeline);

	MDP5_CTL_DBG("set_pipeline ctl=%u complete", ctl->id);
	MSM_FUNC_EXIT("ret=0");
	return 0;
}

static bool start_signal_needed(struct mdp5_ctl *ctl,
				struct mdp5_pipeline *pipeline)
{
	struct mdp5_interface *intf = pipeline->intf;
	bool needed;

	if (!ctl->encoder_enabled)
		return false;

	switch (intf->type) {
	case INTF_WB:
		needed = true;
		break;
	case INTF_DSI:
		needed = intf->mode == MDP5_INTF_DSI_MODE_COMMAND;
		break;
	default:
		needed = false;
		break;
	}
	MDP5_CTL_DBG("start_signal_needed ctl=%u type=%d needed=%d", ctl->id,
		intf->type, needed);
	return needed;
}

/*
 * send_start_signal() - Overlay Processor Start Signal
 *
 * For a given control operation (display pipeline), a START signal needs to be
 * executed in order to kick off operation and activate all layers.
 * e.g.: DSI command mode, Writeback
 */
static void send_start_signal(struct mdp5_ctl *ctl)
{
	unsigned long flags;
	MDP5_CTL_DBG("send_start_signal ctl=%u", ctl->id);

	spin_lock_irqsave(&ctl->hw_lock, flags);
	ctl_write(ctl, REG_MDP5_CTL_START(ctl->id), 1);
	spin_unlock_irqrestore(&ctl->hw_lock, flags);
	MDP5_CTL_DBG("send_start_signal ctl=%u done", ctl->id);
}

/**
 * mdp5_ctl_set_encoder_state() - set the encoder state
 *
 * @ctl:      the CTL instance
 * @pipeline: the encoder's INTF + MIXER configuration
 * @enabled:  true, when encoder is ready for data streaming; false, otherwise.
 *
 * Note:
 * This encoder state is needed to trigger START signal (data path kickoff).
 */
int mdp5_ctl_set_encoder_state(struct mdp5_ctl *ctl,
			       struct mdp5_pipeline *pipeline,
			       bool enabled)
{
	struct mdp5_interface *intf = pipeline->intf;

	MSM_FUNC_ENTER("ctl=%u enabled=%d", ctl->id, enabled);
	MDP5_CTL_DBG("set_encoder_state ctl=%u intf=%d enabled=%d", ctl->id,
		intf->num, enabled);

	if (WARN_ON(!ctl))
		return -EINVAL;

	ctl->encoder_enabled = enabled;
	DBG("intf_%d: %s", intf->num, enabled ? "on" : "off");

	if (start_signal_needed(ctl, pipeline)) {
		send_start_signal(ctl);
	}

	MDP5_CTL_DBG("set_encoder_state ctl=%u complete", ctl->id);
	MSM_FUNC_EXIT("ret=0");
	return 0;
}

/*
 * Note:
 * CTL registers need to be flushed after calling this function
 * (call mdp5_ctl_commit() with mdp_ctl_flush_mask_ctl() mask)
 */
int mdp5_ctl_set_cursor(struct mdp5_ctl *ctl, struct mdp5_pipeline *pipeline,
			int cursor_id, bool enable)
{
	struct mdp5_ctl_manager *ctl_mgr = ctl->ctlm;
	unsigned long flags;
	u32 blend_cfg;
	struct mdp5_hw_mixer *mixer = pipeline->mixer;

	MSM_FUNC_ENTER("ctl=%u cursor_id=%d enable=%d", ctl->id, cursor_id, enable);
	MDP5_CTL_DBG("set_cursor ctl=%u cursor_id=%d enable=%d", ctl->id,
		cursor_id, enable);

	if (WARN_ON(!mixer)) {
		DRM_DEV_ERROR(ctl_mgr->dev->dev, "CTL %d cannot find LM",
			ctl->id);
		return -EINVAL;
	}

	if (pipeline->r_mixer) {
		DRM_DEV_ERROR(ctl_mgr->dev->dev, "unsupported configuration");
		return -EINVAL;
	}

	spin_lock_irqsave(&ctl->hw_lock, flags);

	blend_cfg = ctl_read(ctl, REG_MDP5_CTL_LAYER_REG(ctl->id, mixer->lm));

	if (enable)
		blend_cfg |=  MDP5_CTL_LAYER_REG_CURSOR_OUT;
	else
		blend_cfg &= ~MDP5_CTL_LAYER_REG_CURSOR_OUT;

	ctl_write(ctl, REG_MDP5_CTL_LAYER_REG(ctl->id, mixer->lm), blend_cfg);
	ctl->cursor_on = enable;

	spin_unlock_irqrestore(&ctl->hw_lock, flags);

	ctl->pending_ctl_trigger = mdp_ctl_flush_mask_cursor(cursor_id);
	MDP5_CTL_DBG("set_cursor ctl=%u blend_cfg=0x%08x pending=0x%08x", ctl->id,
		blend_cfg, ctl->pending_ctl_trigger);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

static u32 mdp_ctl_blend_mask(enum mdp5_pipe pipe,
		enum mdp_mixer_stage_id stage)
{
	u32 mask;

	switch (pipe) {
	case SSPP_VIG0: mask = MDP5_CTL_LAYER_REG_VIG0(stage); break;
	case SSPP_VIG1: mask = MDP5_CTL_LAYER_REG_VIG1(stage); break;
	case SSPP_VIG2: mask = MDP5_CTL_LAYER_REG_VIG2(stage); break;
	case SSPP_RGB0: mask = MDP5_CTL_LAYER_REG_RGB0(stage); break;
	case SSPP_RGB1: mask = MDP5_CTL_LAYER_REG_RGB1(stage); break;
	case SSPP_RGB2: mask = MDP5_CTL_LAYER_REG_RGB2(stage); break;
	case SSPP_DMA0: mask = MDP5_CTL_LAYER_REG_DMA0(stage); break;
	case SSPP_DMA1: mask = MDP5_CTL_LAYER_REG_DMA1(stage); break;
	case SSPP_VIG3: mask = MDP5_CTL_LAYER_REG_VIG3(stage); break;
	case SSPP_RGB3: mask = MDP5_CTL_LAYER_REG_RGB3(stage); break;
	case SSPP_CURSOR0:
	case SSPP_CURSOR1:
	default:
		mask = 0;
		break;
	}
	MDP5_CTL_DBG("blend_mask pipe=%d stage=%d mask=0x%08x", pipe, stage, mask);
	return mask;
}

static u32 mdp_ctl_blend_ext_mask(enum mdp5_pipe pipe,
		enum mdp_mixer_stage_id stage)
{
	u32 mask = 0;
	if (stage < STAGE6 && (pipe != SSPP_CURSOR0 && pipe != SSPP_CURSOR1))
		goto out;

	switch (pipe) {
	case SSPP_VIG0: mask = MDP5_CTL_LAYER_EXT_REG_VIG0_BIT3; break;
	case SSPP_VIG1: mask = MDP5_CTL_LAYER_EXT_REG_VIG1_BIT3; break;
	case SSPP_VIG2: mask = MDP5_CTL_LAYER_EXT_REG_VIG2_BIT3; break;
	case SSPP_RGB0: mask = MDP5_CTL_LAYER_EXT_REG_RGB0_BIT3; break;
	case SSPP_RGB1: mask = MDP5_CTL_LAYER_EXT_REG_RGB1_BIT3; break;
	case SSPP_RGB2: mask = MDP5_CTL_LAYER_EXT_REG_RGB2_BIT3; break;
	case SSPP_DMA0: mask = MDP5_CTL_LAYER_EXT_REG_DMA0_BIT3; break;
	case SSPP_DMA1: mask = MDP5_CTL_LAYER_EXT_REG_DMA1_BIT3; break;
	case SSPP_VIG3: mask = MDP5_CTL_LAYER_EXT_REG_VIG3_BIT3; break;
	case SSPP_RGB3: mask = MDP5_CTL_LAYER_EXT_REG_RGB3_BIT3; break;
	case SSPP_CURSOR0: mask = MDP5_CTL_LAYER_EXT_REG_CURSOR0(stage); break;
	case SSPP_CURSOR1: mask = MDP5_CTL_LAYER_EXT_REG_CURSOR1(stage); break;
	default:
		mask = 0;
		break;
	}

out:
	MDP5_CTL_DBG("blend_ext_mask pipe=%d stage=%d mask=0x%08x", pipe, stage,
		mask);
	return mask;
}

static void mdp5_ctl_reset_blend_regs(struct mdp5_ctl *ctl)
{
	unsigned long flags;
	struct mdp5_ctl_manager *ctl_mgr = ctl->ctlm;
	int i;

	MDP5_CTL_DBG("reset_blend_regs ctl=%u", ctl->id);

	spin_lock_irqsave(&ctl->hw_lock, flags);

	for (i = 0; i < ctl_mgr->nlm; i++) {
		ctl_write(ctl, REG_MDP5_CTL_LAYER_REG(ctl->id, i), 0x0);
		ctl_write(ctl, REG_MDP5_CTL_LAYER_EXT_REG(ctl->id, i), 0x0);
	}

	spin_unlock_irqrestore(&ctl->hw_lock, flags);
	MDP5_CTL_DBG("reset_blend_regs ctl=%u done", ctl->id);
}

#define PIPE_LEFT	0
#define PIPE_RIGHT	1
int mdp5_ctl_blend(struct mdp5_ctl *ctl, struct mdp5_pipeline *pipeline,
		   enum mdp5_pipe stage[][MAX_PIPE_STAGE],
		   enum mdp5_pipe r_stage[][MAX_PIPE_STAGE],
		   u32 stage_cnt, u32 ctl_blend_op_flags)
{
	struct mdp5_hw_mixer *mixer = pipeline->mixer;
	struct mdp5_hw_mixer *r_mixer = pipeline->r_mixer;
	unsigned long flags;
	u32 blend_cfg = 0, blend_ext_cfg = 0;
	u32 r_blend_cfg = 0, r_blend_ext_cfg = 0;
	int i, start_stage;

	MSM_FUNC_ENTER("ctl=%u stage_cnt=%u", ctl->id, stage_cnt);
	MDP5_CTL_DBG("blend ctl=%u stage_cnt=%u flags=0x%08x", ctl->id,
		stage_cnt, ctl_blend_op_flags);

	mdp5_ctl_reset_blend_regs(ctl);

	if (ctl_blend_op_flags & MDP5_CTL_BLEND_OP_FLAG_BORDER_OUT) {
		start_stage = STAGE0;
		blend_cfg |= MDP5_CTL_LAYER_REG_BORDER_COLOR;
		if (r_mixer)
			r_blend_cfg |= MDP5_CTL_LAYER_REG_BORDER_COLOR;
	} else {
		start_stage = STAGE_BASE;
	}

	for (i = start_stage; stage_cnt && i <= STAGE_MAX; i++) {
		blend_cfg |=
			mdp_ctl_blend_mask(stage[i][PIPE_LEFT], i) |
			mdp_ctl_blend_mask(stage[i][PIPE_RIGHT], i);
		blend_ext_cfg |=
			mdp_ctl_blend_ext_mask(stage[i][PIPE_LEFT], i) |
			mdp_ctl_blend_ext_mask(stage[i][PIPE_RIGHT], i);
		if (r_mixer) {
			r_blend_cfg |=
				mdp_ctl_blend_mask(r_stage[i][PIPE_LEFT], i) |
				mdp_ctl_blend_mask(r_stage[i][PIPE_RIGHT], i);
			r_blend_ext_cfg |=
			     mdp_ctl_blend_ext_mask(r_stage[i][PIPE_LEFT], i) |
			     mdp_ctl_blend_ext_mask(r_stage[i][PIPE_RIGHT], i);
		}
	}

	spin_lock_irqsave(&ctl->hw_lock, flags);
	if (ctl->cursor_on)
		blend_cfg |=  MDP5_CTL_LAYER_REG_CURSOR_OUT;

	ctl_write(ctl, REG_MDP5_CTL_LAYER_REG(ctl->id, mixer->lm), blend_cfg);
	ctl_write(ctl, REG_MDP5_CTL_LAYER_EXT_REG(ctl->id, mixer->lm),
		  blend_ext_cfg);
	if (r_mixer) {
		ctl_write(ctl, REG_MDP5_CTL_LAYER_REG(ctl->id, r_mixer->lm),
			  r_blend_cfg);
		ctl_write(ctl, REG_MDP5_CTL_LAYER_EXT_REG(ctl->id, r_mixer->lm),
			  r_blend_ext_cfg);
	}
	spin_unlock_irqrestore(&ctl->hw_lock, flags);

	ctl->pending_ctl_trigger = mdp_ctl_flush_mask_lm(mixer->lm);
	if (r_mixer)
		ctl->pending_ctl_trigger |= mdp_ctl_flush_mask_lm(r_mixer->lm);

	DBG("lm%d: blend config = 0x%08x. ext_cfg = 0x%08x", mixer->lm,
		blend_cfg, blend_ext_cfg);
	if (r_mixer)
		DBG("lm%d: blend config = 0x%08x. ext_cfg = 0x%08x",
		    r_mixer->lm, r_blend_cfg, r_blend_ext_cfg);
	MDP5_CTL_DBG("blend ctl=%u pending=0x%08x", ctl->id,
		ctl->pending_ctl_trigger);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

u32 mdp_ctl_flush_mask_encoder(struct mdp5_interface *intf)
{
	if (intf->type == INTF_WB)
		return MDP5_CTL_FLUSH_WB;

	switch (intf->num) {
	case 0: return MDP5_CTL_FLUSH_TIMING_0;
	case 1: return MDP5_CTL_FLUSH_TIMING_1;
	case 2: return MDP5_CTL_FLUSH_TIMING_2;
	case 3: return MDP5_CTL_FLUSH_TIMING_3;
	default: return 0;
	}
}

u32 mdp_ctl_flush_mask_cursor(int cursor_id)
{
	switch (cursor_id) {
	case 0: return MDP5_CTL_FLUSH_CURSOR_0;
	case 1: return MDP5_CTL_FLUSH_CURSOR_1;
	default: return 0;
	}
}

u32 mdp_ctl_flush_mask_pipe(enum mdp5_pipe pipe)
{
	switch (pipe) {
	case SSPP_VIG0: return MDP5_CTL_FLUSH_VIG0;
	case SSPP_VIG1: return MDP5_CTL_FLUSH_VIG1;
	case SSPP_VIG2: return MDP5_CTL_FLUSH_VIG2;
	case SSPP_RGB0: return MDP5_CTL_FLUSH_RGB0;
	case SSPP_RGB1: return MDP5_CTL_FLUSH_RGB1;
	case SSPP_RGB2: return MDP5_CTL_FLUSH_RGB2;
	case SSPP_DMA0: return MDP5_CTL_FLUSH_DMA0;
	case SSPP_DMA1: return MDP5_CTL_FLUSH_DMA1;
	case SSPP_VIG3: return MDP5_CTL_FLUSH_VIG3;
	case SSPP_RGB3: return MDP5_CTL_FLUSH_RGB3;
	case SSPP_CURSOR0: return MDP5_CTL_FLUSH_CURSOR_0;
	case SSPP_CURSOR1: return MDP5_CTL_FLUSH_CURSOR_1;
	default:        return 0;
	}
}

u32 mdp_ctl_flush_mask_lm(int lm)
{
	switch (lm) {
	case 0:  return MDP5_CTL_FLUSH_LM0;
	case 1:  return MDP5_CTL_FLUSH_LM1;
	case 2:  return MDP5_CTL_FLUSH_LM2;
	case 3:  return MDP5_CTL_FLUSH_LM3;
	case 4:  return MDP5_CTL_FLUSH_LM4;
	case 5:  return MDP5_CTL_FLUSH_LM5;
	default: return 0;
	}
}

static u32 fix_sw_flush(struct mdp5_ctl *ctl, struct mdp5_pipeline *pipeline,
			u32 flush_mask)
{
	struct mdp5_ctl_manager *ctl_mgr = ctl->ctlm;
	u32 sw_mask = 0;
#define BIT_NEEDS_SW_FIX(bit) \
	(!(ctl_mgr->flush_hw_mask & bit) && (flush_mask & bit))

	/* for some targets, cursor bit is the same as LM bit */
	if (BIT_NEEDS_SW_FIX(MDP5_CTL_FLUSH_CURSOR_0))
		sw_mask |= mdp_ctl_flush_mask_lm(pipeline->mixer->lm);

	return sw_mask;
}

static void fix_for_single_flush(struct mdp5_ctl *ctl, u32 *flush_mask,
		u32 *flush_id)
{
	struct mdp5_ctl_manager *ctl_mgr = ctl->ctlm;
	MDP5_CTL_DBG("fix_single_flush ctl=%u mask_in=0x%08x id_in=%u", ctl->id,
		*flush_mask, *flush_id);

	if (ctl->pair) {
		DBG("CTL %d FLUSH pending mask %x", ctl->id, *flush_mask);
		ctl->flush_pending = true;
		ctl_mgr->single_flush_pending_mask |= (*flush_mask);
		*flush_mask = 0;

		if (ctl->pair->flush_pending) {
			*flush_id = min_t(u32, ctl->id, ctl->pair->id);
			*flush_mask = ctl_mgr->single_flush_pending_mask;

			ctl->flush_pending = false;
			ctl->pair->flush_pending = false;
			ctl_mgr->single_flush_pending_mask = 0;

			DBG("Single FLUSH mask %x,ID %d", *flush_mask,
				*flush_id);
		}
	}
	MDP5_CTL_DBG("fix_single_flush ctl=%u mask_out=0x%08x id_out=%u", ctl->id,
		*flush_mask, *flush_id);
}

/**
 * mdp5_ctl_commit() - Register Flush
 *
 * @ctl:        the CTL instance
 * @pipeline:   the encoder's INTF + MIXER configuration
 * @flush_mask: bitmask of display controller hw blocks to flush
 * @start:      if true, immediately update flush registers and set START
 *              bit, otherwise accumulate flush_mask bits until we are
 *              ready to START
 *
 * The flush register is used to indicate several registers are all
 * programmed, and are safe to update to the back copy of the double
 * buffered registers.
 *
 * Some registers FLUSH bits are shared when the hardware does not have
 * dedicated bits for them; handling these is the job of fix_sw_flush().
 *
 * CTL registers need to be flushed in some circumstances; if that is the
 * case, some trigger bits will be present in both flush mask and
 * ctl->pending_ctl_trigger.
 *
 * Return H/W flushed bit mask.
 */
u32 mdp5_ctl_commit(struct mdp5_ctl *ctl,
		    struct mdp5_pipeline *pipeline,
		    u32 flush_mask, bool start)
{
	struct mdp5_ctl_manager *ctl_mgr = ctl->ctlm;
	unsigned long flags;
	u32 flush_id = ctl->id;
	u32 curr_ctl_flush_mask;

	MSM_FUNC_ENTER("ctl=%u flush=0x%08x start=%d", ctl->id, flush_mask, start);
	MDP5_CTL_DBG("commit ctl=%u flush_mask=0x%08x start=%d", ctl->id,
		flush_mask, start);

	VERB("flush_mask=%x, trigger=%x", flush_mask, ctl->pending_ctl_trigger);

	if (ctl->pending_ctl_trigger & flush_mask) {
		flush_mask |= MDP5_CTL_FLUSH_CTL;
		ctl->pending_ctl_trigger = 0;
	}

	flush_mask |= fix_sw_flush(ctl, pipeline, flush_mask);

	flush_mask &= ctl_mgr->flush_hw_mask;

	curr_ctl_flush_mask = flush_mask;

	fix_for_single_flush(ctl, &flush_mask, &flush_id);

	if (!start) {
		ctl->flush_mask |= flush_mask;
		MDP5_CTL_DBG("commit ctl=%u deferred mask=0x%08x curr=0x%08x", ctl->id,
			ctl->flush_mask, curr_ctl_flush_mask);
		return curr_ctl_flush_mask;
	} else {
		flush_mask |= ctl->flush_mask;
		ctl->flush_mask = 0;
	}

	if (flush_mask) {
		spin_lock_irqsave(&ctl->hw_lock, flags);
		ctl_write(ctl, REG_MDP5_CTL_FLUSH(flush_id), flush_mask);
		spin_unlock_irqrestore(&ctl->hw_lock, flags);
	}

	if (start_signal_needed(ctl, pipeline)) {
		send_start_signal(ctl);
	}

	MDP5_CTL_DBG("commit ctl=%u curr=0x%08x", ctl->id, curr_ctl_flush_mask);
	MSM_FUNC_EXIT("ret=0x%08x", curr_ctl_flush_mask);
	return curr_ctl_flush_mask;
}

u32 mdp5_ctl_get_commit_status(struct mdp5_ctl *ctl)
{
	MSM_FUNC_ENTER("ctl=%u", ctl->id);
	u32 status = ctl_read(ctl, REG_MDP5_CTL_FLUSH(ctl->id));
	MDP5_CTL_DBG("commit_status ctl=%u status=0x%08x", ctl->id, status);
	MSM_FUNC_EXIT("status=0x%08x", status);
	return status;
}

int mdp5_ctl_get_ctl_id(struct mdp5_ctl *ctl)
{
	int id = WARN_ON(!ctl) ? -EINVAL : ctl->id;
	MDP5_CTL_DBG("get_ctl_id ctl=%p id=%d", ctl, id);
	return id;
}

/*
 * mdp5_ctl_pair() - Associate 2 booked CTLs for single FLUSH
 */
int mdp5_ctl_pair(struct mdp5_ctl *ctlx, struct mdp5_ctl *ctly, bool enable)
{
	struct mdp5_ctl_manager *ctl_mgr = ctlx->ctlm;
	struct mdp5_kms *mdp5_kms = get_kms(ctl_mgr);

	MSM_FUNC_ENTER("ctlx=%u ctly=%u enable=%d", ctlx->id, ctly->id, enable);
	MDP5_CTL_DBG("pair ctlx=%u ctly=%u enable=%d", ctlx->id, ctly->id, enable);

	/* do nothing silently if hw doesn't support */
	if (!ctl_mgr->single_flush_supported)
		return 0;

	if (!enable) {
		ctlx->pair = NULL;
		ctly->pair = NULL;
		mdp5_write(mdp5_kms, REG_MDP5_SPARE_0, 0);
		MDP5_CTL_DBG("pair cleared ctlx=%u ctly=%u", ctlx->id, ctly->id);
		return 0;
	} else if ((ctlx->pair != NULL) || (ctly->pair != NULL)) {
		DRM_DEV_ERROR(ctl_mgr->dev->dev, "CTLs already paired\n");
		return -EINVAL;
	} else if (!(ctlx->status & ctly->status & CTL_STAT_BOOKED)) {
		DRM_DEV_ERROR(ctl_mgr->dev->dev, "Only pair booked CTLs\n");
		return -EINVAL;
	}

	ctlx->pair = ctly;
	ctly->pair = ctlx;

	mdp5_write(mdp5_kms, REG_MDP5_SPARE_0,
		   MDP5_SPARE_0_SPLIT_DPL_SINGLE_FLUSH_EN);
	MDP5_CTL_DBG("pair configured ctlx=%u ctly=%u", ctlx->id, ctly->id);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

/*
 * mdp5_ctl_request() - CTL allocation
 *
 * Try to return booked CTL for @intf_num is 1 or 2, unbooked for other INTFs.
 * If no CTL is available in preferred category, allocate from the other one.
 *
 * @return fail if no CTL is available.
 */
struct mdp5_ctl *mdp5_ctlm_request(struct mdp5_ctl_manager *ctl_mgr,
		int intf_num)
{
	struct mdp5_ctl *ctl = NULL;
	const u32 checkm = CTL_STAT_BUSY | CTL_STAT_BOOKED;
	u32 match = ((intf_num == 1) || (intf_num == 2)) ? CTL_STAT_BOOKED : 0;
	unsigned long flags;
	int c;

	MSM_FUNC_ENTER("intf_num=%d", intf_num);
	MDP5_CTL_DBG("request intf_num=%d", intf_num);

	spin_lock_irqsave(&ctl_mgr->pool_lock, flags);

	/* search the preferred */
	for (c = 0; c < ctl_mgr->nctl; c++)
		if ((ctl_mgr->ctls[c].status & checkm) == match)
			goto found;

	dev_warn(ctl_mgr->dev->dev,
		"fall back to the other CTL category for INTF %d!\n", intf_num);

	match ^= CTL_STAT_BOOKED;
	for (c = 0; c < ctl_mgr->nctl; c++)
		if ((ctl_mgr->ctls[c].status & checkm) == match)
			goto found;

	DRM_DEV_ERROR(ctl_mgr->dev->dev, "No more CTL available!");
	goto unlock;

found:
	ctl = &ctl_mgr->ctls[c];
	ctl->status |= CTL_STAT_BUSY;
	ctl->pending_ctl_trigger = 0;
	DBG("CTL %d allocated", ctl->id);
	MDP5_CTL_DBG("request allocated ctl=%u status=0x%08x", ctl->id,
		ctl->status);

unlock:
	spin_unlock_irqrestore(&ctl_mgr->pool_lock, flags);
	MSM_FUNC_EXIT("ctl=%p", ctl);
	return ctl;
}

void mdp5_ctlm_hw_reset(struct mdp5_ctl_manager *ctl_mgr)
{
	unsigned long flags;
	int c;

	MSM_FUNC_ENTER("nctl=%u", ctl_mgr->nctl);
	MDP5_CTL_DBG("hw_reset nctl=%u", ctl_mgr->nctl);

	for (c = 0; c < ctl_mgr->nctl; c++) {
		struct mdp5_ctl *ctl = &ctl_mgr->ctls[c];

		spin_lock_irqsave(&ctl->hw_lock, flags);
		ctl_write(ctl, REG_MDP5_CTL_OP(ctl->id), 0);
		spin_unlock_irqrestore(&ctl->hw_lock, flags);
	}
	MDP5_CTL_DBG("hw_reset done");
	MSM_FUNC_EXIT("");
}

void mdp5_ctlm_destroy(struct mdp5_ctl_manager *ctl_mgr)
{
	MSM_FUNC_ENTER("ctl_mgr=%p", ctl_mgr);
	MDP5_CTL_DBG("destroy ctl_mgr=%p", ctl_mgr);
	kfree(ctl_mgr);
	MSM_FUNC_EXIT("");
}

struct mdp5_ctl_manager *mdp5_ctlm_init(struct drm_device *dev,
		void __iomem *mmio_base, struct mdp5_cfg_handler *cfg_hnd)
{
	struct mdp5_ctl_manager *ctl_mgr;
	const struct mdp5_cfg_hw *hw_cfg = mdp5_cfg_get_hw_config(cfg_hnd);
	int rev = mdp5_cfg_get_hw_rev(cfg_hnd);
	unsigned dsi_cnt = 0;
	const struct mdp5_ctl_block *ctl_cfg = &hw_cfg->ctl;
	unsigned long flags;
	int c, ret;

	MSM_FUNC_ENTER("dev=%p", dev);

	ctl_mgr = kzalloc(sizeof(*ctl_mgr), GFP_KERNEL);
	if (!ctl_mgr) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate CTL manager\n");
		ret = -ENOMEM;
		goto fail;
	}
	MDP5_CTL_DBG("init ctl_mgr=%p", ctl_mgr);

	if (WARN_ON(ctl_cfg->count > MAX_CTL)) {
		DRM_DEV_ERROR(dev->dev, "Increase static pool size to at least %d\n",
				ctl_cfg->count);
		ret = -ENOSPC;
		goto fail;
	}

	/* initialize the CTL manager: */
	ctl_mgr->dev = dev;
	ctl_mgr->nlm = hw_cfg->lm.count;
	ctl_mgr->nctl = ctl_cfg->count;
	ctl_mgr->flush_hw_mask = ctl_cfg->flush_hw_mask;
	spin_lock_init(&ctl_mgr->pool_lock);

	/* initialize each CTL of the pool: */
	spin_lock_irqsave(&ctl_mgr->pool_lock, flags);
	for (c = 0; c < ctl_mgr->nctl; c++) {
		struct mdp5_ctl *ctl = &ctl_mgr->ctls[c];

		if (WARN_ON(!ctl_cfg->base[c])) {
			DRM_DEV_ERROR(dev->dev, "CTL_%d: base is null!\n", c);
			ret = -EINVAL;
			spin_unlock_irqrestore(&ctl_mgr->pool_lock, flags);
			goto fail;
		}
		ctl->ctlm = ctl_mgr;
		ctl->id = c;
		ctl->reg_offset = ctl_cfg->base[c];
		ctl->status = 0;
		spin_lock_init(&ctl->hw_lock);
	}

	/*
	 * In bonded DSI case, CTL0 and CTL1 are always assigned to two DSI
	 * interfaces to support single FLUSH feature (Flush CTL0 and CTL1 when
	 * only write into CTL0's FLUSH register) to keep two DSI pipes in sync.
	 * Single FLUSH is supported from hw rev v3.0.
	 */
	for (c = 0; c < ARRAY_SIZE(hw_cfg->intf.connect); c++)
		if (hw_cfg->intf.connect[c] == INTF_DSI)
			dsi_cnt++;
	if ((rev >= 3) && (dsi_cnt > 1)) {
		ctl_mgr->single_flush_supported = true;
		/* Reserve CTL0/1 for INTF1/2 */
		ctl_mgr->ctls[0].status |= CTL_STAT_BOOKED;
		ctl_mgr->ctls[1].status |= CTL_STAT_BOOKED;
	}
	spin_unlock_irqrestore(&ctl_mgr->pool_lock, flags);
	DBG("Pool of %d CTLs created.", ctl_mgr->nctl);
	MDP5_CTL_DBG("init complete nlm=%u nctl=%u flush_mask=0x%08x",
		ctl_mgr->nlm, ctl_mgr->nctl, ctl_mgr->flush_hw_mask);

	MSM_FUNC_EXIT("ctl_mgr=%p", ctl_mgr);
	return ctl_mgr;

fail:
	if (ctl_mgr)
		mdp5_ctlm_destroy(ctl_mgr);
	MDP5_CTL_DBG("init failed ret=%d", ret);

	return ERR_PTR(ret);
}

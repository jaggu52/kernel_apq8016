// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "msm_gem.h"

struct msm_framebuffer {
	struct drm_framebuffer base;
	const struct msm_format *format;
};
#define to_msm_framebuffer(x) container_of(x, struct msm_framebuffer, base)

static struct drm_framebuffer *msm_framebuffer_init(struct drm_device *dev,
		const struct drm_mode_fb_cmd2 *mode_cmd, struct drm_gem_object **bos);

static const struct drm_framebuffer_funcs msm_framebuffer_funcs = {
	.create_handle = drm_gem_fb_create_handle,
	.destroy = drm_gem_fb_destroy,
	.dirty = drm_atomic_helper_dirtyfb,
};

#ifdef CONFIG_DEBUG_FS
void msm_framebuffer_describe(struct drm_framebuffer *fb, struct seq_file *m)
{
	struct msm_gem_stats stats = {};
	int i, n = fb->format->num_planes;

	MSM_FUNC_ENTER("fb=%p", fb);
	seq_printf(m, "fb: %dx%d@%4.4s (%2d, ID:%d)\n",
			fb->width, fb->height, (char *)&fb->format->format,
			drm_framebuffer_read_refcount(fb), fb->base.id);

	for (i = 0; i < n; i++) {
		seq_printf(m, "   %d: offset=%d pitch=%d, obj: ",
				i, fb->offsets[i], fb->pitches[i]);
		msm_gem_describe(fb->obj[i], m, &stats);
	}
	MSM_FUNC_EXIT("");
}
#endif

/* prepare/pin all the fb's bo's for scanout.  Note that it is not valid
 * to prepare an fb more multiple different initiator 'id's.  But that
 * should be fine, since only the scanout (mdpN) side of things needs
 * this, the gpu doesn't care about fb's.
 */
int msm_framebuffer_prepare(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace)
{
	int ret, i, n = fb->format->num_planes;
	uint64_t iova;

	MSM_FUNC_ENTER("fb=%p", fb);
	for (i = 0; i < n; i++) {
		ret = msm_gem_get_and_pin_iova(fb->obj[i], aspace, &iova);
		drm_dbg_state(fb->dev, "FB[%u]: iova[%d]: %08llx (%d)", fb->base.id, i, iova, ret);
		if (ret)
			goto out;
	}

	ret = 0;

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

void msm_framebuffer_cleanup(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace)
{
	int i, n = fb->format->num_planes;

	MSM_FUNC_ENTER("fb=%p", fb);
	for (i = 0; i < n; i++)
		msm_gem_unpin_iova(fb->obj[i], aspace);
	MSM_FUNC_EXIT("");
}

uint32_t msm_framebuffer_iova(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace, int plane)
{
	uint32_t addr;

	MSM_FUNC_ENTER("fb=%p plane=%d", fb, plane);
	if (!fb->obj[plane])
		addr = 0;
	else
		addr = msm_gem_iova(fb->obj[plane], aspace) + fb->offsets[plane];
	MSM_FUNC_EXIT("addr=0x%08x", addr);
	return addr;
}

struct drm_gem_object *msm_framebuffer_bo(struct drm_framebuffer *fb, int plane)
{
	struct drm_gem_object *obj;

	MSM_FUNC_ENTER("fb=%p plane=%d", fb, plane);
	obj = drm_gem_fb_get_obj(fb, plane);
	MSM_FUNC_EXIT("obj=%p", obj);
	return obj;
}

const struct msm_format *msm_framebuffer_format(struct drm_framebuffer *fb)
{
	struct msm_framebuffer *msm_fb = to_msm_framebuffer(fb);
	const struct msm_format *fmt;

	MSM_FUNC_ENTER("fb=%p", fb);
	fmt = msm_fb->format;
	MSM_FUNC_EXIT("format=%p", fmt);
	return fmt;
}

struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,
		struct drm_file *file, const struct drm_mode_fb_cmd2 *mode_cmd)
{
	const struct drm_format_info *info = drm_get_format_info(dev,
								 mode_cmd);
	struct drm_gem_object *bos[4] = {0};
	struct drm_framebuffer *fb;
	int ret, i, n = info->num_planes;

	MSM_FUNC_ENTER("dev=%p", dev);
	for (i = 0; i < n; i++) {
		bos[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!bos[i]) {
			ret = -ENXIO;
			goto out_unref;
		}
	}

	fb = msm_framebuffer_init(dev, mode_cmd, bos);
	if (IS_ERR(fb)) {
		ret = PTR_ERR(fb);
		goto out_unref;
	}

	MSM_FUNC_EXIT("fb=%p", fb);
	return fb;

out_unref:
	for (i = 0; i < n; i++)
		drm_gem_object_put(bos[i]);
	MSM_FUNC_EXIT("err=%d", ret);
	return ERR_PTR(ret);
}

static struct drm_framebuffer *msm_framebuffer_init(struct drm_device *dev,
		const struct drm_mode_fb_cmd2 *mode_cmd, struct drm_gem_object **bos)
{
	const struct drm_format_info *info = drm_get_format_info(dev,
								 mode_cmd);
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	struct msm_framebuffer *msm_fb = NULL;
	struct drm_framebuffer *fb;
	const struct msm_format *format;
	int ret, i, n;

	MSM_FUNC_ENTER("dev=%p", dev);
	drm_dbg_state(dev, "create framebuffer: mode_cmd=%p (%dx%d@%4.4s)",
			mode_cmd, mode_cmd->width, mode_cmd->height,
			(char *)&mode_cmd->pixel_format);

	n = info->num_planes;
	format = kms->funcs->get_format(kms, mode_cmd->pixel_format,
			mode_cmd->modifier[0]);
	if (!format) {
		DRM_DEV_ERROR(dev->dev, "unsupported pixel format: %4.4s\n",
				(char *)&mode_cmd->pixel_format);
		ret = -EINVAL;
		goto fail;
	}

	msm_fb = kzalloc(sizeof(*msm_fb), GFP_KERNEL);
	if (!msm_fb) {
		ret = -ENOMEM;
		goto fail;
	}

	fb = &msm_fb->base;

	msm_fb->format = format;

	if (n > ARRAY_SIZE(fb->obj)) {
		ret = -EINVAL;
		goto fail;
	}

	for (i = 0; i < n; i++) {
		unsigned int width = mode_cmd->width / (i ? info->hsub : 1);
		unsigned int height = mode_cmd->height / (i ? info->vsub : 1);
		unsigned int min_size;

		min_size = (height - 1) * mode_cmd->pitches[i]
			 + width * info->cpp[i]
			 + mode_cmd->offsets[i];

		if (bos[i]->size < min_size) {
			ret = -EINVAL;
			goto fail;
		}

		msm_fb->base.obj[i] = bos[i];
	}

	drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);

	ret = drm_framebuffer_init(dev, fb, &msm_framebuffer_funcs);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "framebuffer init failed: %d\n", ret);
		goto fail;
	}

	drm_dbg_state(dev, "create: FB ID: %d (%p)", fb->base.id, fb);
	MSM_FUNC_EXIT("fb=%p", fb);
	return fb;

fail:
	kfree(msm_fb);
	MSM_FUNC_EXIT("err=%d", ret);
	return ERR_PTR(ret);
}

struct drm_framebuffer *
msm_alloc_stolen_fb(struct drm_device *dev, int w, int h, int p, uint32_t format)
{
	struct drm_mode_fb_cmd2 mode_cmd = {
		.pixel_format = format,
		.width = w,
		.height = h,
		.pitches = { p },
	};
	struct drm_gem_object *bo;
	struct drm_framebuffer *fb;
	int size;

	MSM_FUNC_ENTER("dev=%p", dev);
	/* allocate backing bo */
	size = mode_cmd.pitches[0] * mode_cmd.height;
	DBG("allocating %d bytes for fb %d", size, dev->primary->index);
	bo = msm_gem_new(dev, size, MSM_BO_SCANOUT | MSM_BO_WC | MSM_BO_STOLEN);
	if (IS_ERR(bo)) {
		dev_warn(dev->dev, "could not allocate stolen bo\n");
		/* try regular bo: */
		bo = msm_gem_new(dev, size, MSM_BO_SCANOUT | MSM_BO_WC);
	}
	if (IS_ERR(bo)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate buffer object\n");
		return ERR_CAST(bo);
	}

	msm_gem_object_set_name(bo, "stolenfb");

	fb = msm_framebuffer_init(dev, &mode_cmd, &bo);
	if (IS_ERR(fb)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate fb\n");
		/* note: if fb creation failed, we can't rely on fb destroy
		 * to unref the bo:
		 */
		drm_gem_object_put(bo);
		MSM_FUNC_EXIT("err=%ld", PTR_ERR(fb));
		return ERR_CAST(fb);
	}

	MSM_FUNC_EXIT("fb=%p", fb);
	return fb;
}

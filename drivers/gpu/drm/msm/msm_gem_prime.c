// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/dma-buf.h>

#include <drm/drm_prime.h>

#include "msm_drv.h"
#include "msm_gem.h"

struct sg_table *msm_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);
	int npages = obj->size >> PAGE_SHIFT;
	struct sg_table *sgt = NULL;

	MSM_FUNC_ENTER("obj=%p", obj);

	if (WARN_ON(!msm_obj->pages))  /* should have already pinned! */
		goto out;

	sgt = drm_prime_pages_to_sg(obj->dev, msm_obj->pages, npages);

out:
	MSM_FUNC_EXIT("sgt=%p", sgt);
	return sgt;
}

int msm_gem_prime_vmap(struct drm_gem_object *obj, struct dma_buf_map *map)
{
	void *vaddr;
	int ret;

	MSM_FUNC_ENTER("obj=%p", obj);

	vaddr = msm_gem_get_vaddr(obj);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out;
	}
	dma_buf_map_set_vaddr(map, vaddr);
	ret = 0;

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

void msm_gem_prime_vunmap(struct drm_gem_object *obj, struct dma_buf_map *map)
{
	MSM_FUNC_ENTER("obj=%p", obj);
	msm_gem_put_vaddr(obj);
	MSM_FUNC_EXIT("");
}

struct drm_gem_object *msm_gem_prime_import_sg_table(struct drm_device *dev,
		struct dma_buf_attachment *attach, struct sg_table *sg)
{
	struct drm_gem_object *obj;

	MSM_FUNC_ENTER("dev=%p attach=%p", dev, attach);
	obj = msm_gem_import(dev, attach->dmabuf, sg);
	MSM_FUNC_EXIT("obj=%p", obj);
	return obj;
}

int msm_gem_prime_pin(struct drm_gem_object *obj)
{
	MSM_FUNC_ENTER("obj=%p", obj);
	if (!obj->import_attach)
		msm_gem_get_pages(obj);
	MSM_FUNC_EXIT("ret=0");
	return 0;
}

void msm_gem_prime_unpin(struct drm_gem_object *obj)
{
	MSM_FUNC_ENTER("obj=%p", obj);
	if (!obj->import_attach)
		msm_gem_put_pages(obj);
	MSM_FUNC_EXIT("");
}

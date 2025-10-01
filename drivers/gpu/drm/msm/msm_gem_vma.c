// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_mmu.h"

static void
msm_gem_address_space_destroy(struct kref *kref)
{
	struct msm_gem_address_space *aspace = container_of(kref,
			struct msm_gem_address_space, kref);

	MSM_FUNC_ENTER("aspace=%p", aspace);

	drm_mm_takedown(&aspace->mm);
	if (aspace->mmu)
		aspace->mmu->funcs->destroy(aspace->mmu);
	put_pid(aspace->pid);
	kfree(aspace);
	MSM_FUNC_EXIT("");
}


void msm_gem_address_space_put(struct msm_gem_address_space *aspace)
{
	MSM_FUNC_ENTER("aspace=%p", aspace);
	if (aspace)
		kref_put(&aspace->kref, msm_gem_address_space_destroy);
	MSM_FUNC_EXIT("");
}

struct msm_gem_address_space *
msm_gem_address_space_get(struct msm_gem_address_space *aspace)
{
	MSM_FUNC_ENTER("aspace=%p", aspace);
	if (!IS_ERR_OR_NULL(aspace))
		kref_get(&aspace->kref);

	MSM_FUNC_EXIT("aspace=%p", aspace);
	return aspace;
}

/* Actually unmap memory for the vma */
void msm_gem_purge_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	unsigned size = vma->node.size << PAGE_SHIFT;

	MSM_FUNC_ENTER("aspace=%p vma=%p", aspace, vma);
	/* Print a message if we try to purge a vma in use */
	if (WARN_ON(vma->inuse > 0))
		goto out;

	/* Don't do anything if the memory isn't mapped */
	if (!vma->mapped)
		goto out;

	if (aspace->mmu)
		aspace->mmu->funcs->unmap(aspace->mmu, vma->iova, size);

	vma->mapped = false;

out:
	MSM_FUNC_EXIT("mapped=%d inuse=%d", vma->mapped, vma->inuse);
}

/* Remove reference counts for the mapping */
void msm_gem_unmap_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	MSM_FUNC_ENTER("aspace=%p vma=%p", aspace, vma);
	if (!WARN_ON(!vma->iova))
		vma->inuse--;
	MSM_FUNC_EXIT("inuse=%d", vma->inuse);
}

int
msm_gem_map_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, int prot,
		struct sg_table *sgt, int npages)
{
	unsigned size = npages << PAGE_SHIFT;
	int ret = 0;
	bool mapped = false;

	MSM_FUNC_ENTER("aspace=%p vma=%p npages=%d", aspace, vma, npages);

	if (WARN_ON(!vma->iova))
		return -EINVAL;

	/* Increase the usage counter */
	vma->inuse++;

	if (vma->mapped)
		goto out;

	vma->mapped = true;

	if (aspace && aspace->mmu)
		ret = aspace->mmu->funcs->map(aspace->mmu, vma->iova, sgt,
				size, prot);

	if (ret) {
		vma->mapped = false;
		vma->inuse--;
	}

out:
	MSM_FUNC_EXIT("ret=%d mapped=%d inuse=%d", ret, vma->mapped, vma->inuse);
	return ret;
}

/* Close an iova.  Warn if it is still in use */
void msm_gem_close_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	MSM_FUNC_ENTER("aspace=%p vma=%p", aspace, vma);
	if (WARN_ON(vma->inuse > 0 || vma->mapped))
		goto out;

	spin_lock(&aspace->lock);
	if (vma->iova)
		drm_mm_remove_node(&vma->node);
	spin_unlock(&aspace->lock);

	vma->iova = 0;

	msm_gem_address_space_put(aspace);

out:
	MSM_FUNC_EXIT("iova=0x%llx inuse=%d mapped=%d",
		(unsigned long long)vma->iova, vma->inuse, vma->mapped);
}

/* Initialize a new vma and allocate an iova for it */
int msm_gem_init_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, int npages,
		u64 range_start, u64 range_end)
{
	int ret;

	MSM_FUNC_ENTER("aspace=%p vma=%p npages=%d", aspace, vma, npages);
	if (WARN_ON(vma->iova))
		return -EBUSY;

	spin_lock(&aspace->lock);
	ret = drm_mm_insert_node_in_range(&aspace->mm, &vma->node, npages, 0,
		0, range_start, range_end, 0);
	spin_unlock(&aspace->lock);

	if (ret)
		goto out;

	vma->iova = vma->node.start << PAGE_SHIFT;
	vma->mapped = false;

	kref_get(&aspace->kref);

out:
	MSM_FUNC_EXIT("ret=%d iova=0x%llx", ret, (unsigned long long)vma->iova);
	return ret;
}

struct msm_gem_address_space *
msm_gem_address_space_create(struct msm_mmu *mmu, const char *name,
		u64 va_start, u64 size)
{
	struct msm_gem_address_space *aspace;

	MSM_FUNC_ENTER("mmu=%p name=%s size=0x%llx", mmu, name, size);
	if (IS_ERR(mmu))
		return ERR_CAST(mmu);

	aspace = kzalloc(sizeof(*aspace), GFP_KERNEL);
	if (!aspace)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&aspace->lock);
	aspace->name = name;
	aspace->mmu = mmu;

	drm_mm_init(&aspace->mm, va_start >> PAGE_SHIFT, size >> PAGE_SHIFT);

	kref_init(&aspace->kref);

	MSM_FUNC_EXIT("aspace=%p", aspace);
	return aspace;
}

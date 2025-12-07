// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2018, 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/dma-mapping.h>
#include <linux/kthread.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <uapi/linux/sched/types.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>
#include <drm/drm_of.h>
#include <drm/drm_vblank.h>

#include "disp/msm_disp_snapshot.h"
#include "msm_drv.h"
#include "msm_debugfs.h"
#include "msm_fence.h"
#include "msm_gem.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "adreno/adreno_gpu.h"

/*
 * MSM driver version:
 * - 1.0.0 - initial interface
 * - 1.1.0 - adds madvise, and support for submits with > 4 cmd buffers
 * - 1.2.0 - adds explicit fence support for submit ioctl
 * - 1.3.0 - adds GMEM_BASE + NR_RINGS params, SUBMITQUEUE_NEW +
 *           SUBMITQUEUE_CLOSE ioctls, and MSM_INFO_IOVA flag for
 *           MSM_GEM_INFO ioctl.
 * - 1.4.0 - softpin, MSM_RELOC_BO_DUMP, and GEM_INFO support to set/get
 *           GEM object's debug name
 * - 1.5.0 - Add SUBMITQUERY_QUERY ioctl
 * - 1.6.0 - Syncobj support
 * - 1.7.0 - Add MSM_PARAM_SUSPENDS to access suspend count
 * - 1.8.0 - Add MSM_BO_CACHED_COHERENT for supported GPUs (a6xx)
 */
#define MSM_VERSION_MAJOR	1
#define MSM_VERSION_MINOR	8
#define MSM_VERSION_PATCHLEVEL	0

static const struct drm_mode_config_funcs mode_config_funcs = {
	.fb_create = msm_framebuffer_create,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs mode_config_helper_funcs = {
	.atomic_commit_tail = msm_atomic_commit_tail,
};

#ifdef CONFIG_DRM_MSM_REGISTER_LOGGING
static bool reglog = false;
MODULE_PARM_DESC(reglog, "Enable register read/write logging");
module_param(reglog, bool, 0600);
#else
#define reglog 0
#endif

#ifdef CONFIG_DRM_FBDEV_EMULATION
static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);
#endif

static char *vram = "16m";
MODULE_PARM_DESC(vram, "Configure VRAM size (for devices without IOMMU/GPUMMU)");
module_param(vram, charp, 0);

bool dumpstate = false;
MODULE_PARM_DESC(dumpstate, "Dump KMS state on errors");
module_param(dumpstate, bool, 0600);

static bool modeset = true;
MODULE_PARM_DESC(modeset, "Use kernel modesetting [KMS] (1=on (default), 0=disable)");
module_param(modeset, bool, 0600);

/*
 * Util/helpers:
 */

struct clk *msm_clk_bulk_get_clock(struct clk_bulk_data *bulk, int count,
		const char *name)
{
	int i;
	char n[32];
	struct clk *clk = NULL;

	MSM_FUNC_ENTER("name=%s count=%d", name, count);

	snprintf(n, sizeof(n), "%s_clk", name);

	for (i = 0; bulk && i < count; i++) {
		if (!strcmp(bulk[i].id, name) || !strcmp(bulk[i].id, n))
			clk = bulk[i].clk;
	}


	MSM_FUNC_EXIT("clk=%p", clk);
	return clk;
}

struct clk *msm_clk_get(struct platform_device *pdev, const char *name)
{
	struct clk *clk;
	char name2[32];
	struct clk *ret_clk;

	MSM_FUNC_ENTER("name=%s", name);

	clk = devm_clk_get(&pdev->dev, name);
	if (!IS_ERR(clk) || PTR_ERR(clk) == -EPROBE_DEFER) {
		ret_clk = clk;
		goto out;
	}

	snprintf(name2, sizeof(name2), "%s_clk", name);

	clk = devm_clk_get(&pdev->dev, name2);
	if (!IS_ERR(clk))
		dev_warn(&pdev->dev, "Using legacy clk name binding.  Use "
				"\"%s\" instead of \"%s\"\n", name, name2);

	ret_clk = clk;

out:
	MSM_FUNC_EXIT("clk=%p", ret_clk);
	return ret_clk;
}

static void __iomem *_msm_ioremap(struct platform_device *pdev, const char *name,
				  const char *dbgname, bool quiet, phys_addr_t *psize)
{
	struct resource *res;
	unsigned long size;
	void __iomem *ptr;
	void __iomem *mapped = ERR_PTR(-EINVAL);

	MSM_FUNC_ENTER("name=%s dbgname=%s quiet=%d", name ? name : "(null)",
		      dbgname ? dbgname : "(null)", quiet);

	if (name)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	else
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		if (!quiet)
			DRM_DEV_ERROR(&pdev->dev, "failed to get memory resource: %s\n", name);
		goto out;
	}

	size = resource_size(res);

	ptr = devm_ioremap(&pdev->dev, res->start, size);
	if (!ptr) {
		if (!quiet)
			DRM_DEV_ERROR(&pdev->dev, "failed to ioremap: %s\n", name);
		mapped = ERR_PTR(-ENOMEM);
		goto out;
	}

	if (reglog)
		printk(KERN_DEBUG "IO:region %s base 0x%08llx %p 0x%08lx\n", dbgname, res->start, ptr, size);

	if (psize)
		*psize = size;

	mapped = ptr;

out:
	MSM_FUNC_EXIT("addr=%p", mapped);
	return mapped;
}

void __iomem *msm_ioremap(struct platform_device *pdev, const char *name,
			  const char *dbgname)
{
	void __iomem *ptr;

	MSM_FUNC_ENTER("name=%s dbgname=%s", name ? name : "(null)",
		      dbgname ? dbgname : "(null)");
	ptr = _msm_ioremap(pdev, name, dbgname, false, NULL);
	MSM_FUNC_EXIT("addr=%p", ptr);
	return ptr;
}

void __iomem *msm_ioremap_quiet(struct platform_device *pdev, const char *name,
				const char *dbgname)
{
	void __iomem *ptr;

	MSM_FUNC_ENTER("name=%s dbgname=%s", name ? name : "(null)",
		      dbgname ? dbgname : "(null)");
	ptr = _msm_ioremap(pdev, name, dbgname, true, NULL);
	MSM_FUNC_EXIT("addr=%p", ptr);
	return ptr;
}

void __iomem *msm_ioremap_size(struct platform_device *pdev, const char *name,
			  const char *dbgname, phys_addr_t *psize)
{
	void __iomem *ptr;

	MSM_FUNC_ENTER("name=%s dbgname=%s", name ? name : "(null)",
		      dbgname ? dbgname : "(null)");
	ptr = _msm_ioremap(pdev, name, dbgname, false, psize);
	MSM_FUNC_EXIT("addr=%p", ptr);
	return ptr;
}

void msm_writel(u32 data, void __iomem *addr)
{
	if (reglog)
		printk(KERN_DEBUG "IO:W %p %08x\n", addr, data);
	writel(data, addr);
}

u32 msm_readl(const void __iomem *addr)
{
	u32 val = readl(addr);
	if (reglog)
		printk("IO:R %p %08x\n", addr, val);
	return val;
}

void msm_rmw(void __iomem *addr, u32 mask, u32 or)
{
	u32 val = msm_readl(addr);

	val &= ~mask;
	msm_writel(val | or, addr);
}

static irqreturn_t msm_irq(int irq, void *arg)
{
	struct drm_device *dev = arg;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	irqreturn_t ret;

	MSM_FUNC_ENTER("irq=%d", irq);
	BUG_ON(!kms);

	ret = kms->funcs->irq(kms);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void msm_irq_preinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;

	MSM_FUNC_ENTER("dev=%p", dev);
	BUG_ON(!kms);

	kms->funcs->irq_preinstall(kms);
	MSM_FUNC_EXIT("");
}

static int msm_irq_postinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);
	BUG_ON(!kms);

	if (kms->funcs->irq_postinstall)
		ret = kms->funcs->irq_postinstall(kms);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_irq_install(struct drm_device *dev, unsigned int irq)
{
	int ret;

	MSM_FUNC_ENTER("irq=%u", irq);

	if (irq == IRQ_NOTCONNECTED) {
		ret = -ENOTCONN;
		goto out;
	}

	msm_irq_preinstall(dev);

	ret = request_irq(irq, msm_irq, 0, dev->driver->name, dev);
	if (ret)
		goto out;

	ret = msm_irq_postinstall(dev);
	if (ret)
		free_irq(irq, dev);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void msm_irq_uninstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;

	MSM_FUNC_ENTER("dev=%p", dev);
	kms->funcs->irq_uninstall(kms);
	free_irq(kms->irq, dev);
	MSM_FUNC_EXIT("");
}

struct msm_vblank_work {
	struct work_struct work;
	int crtc_id;
	bool enable;
	struct msm_drm_private *priv;
};

static void vblank_ctrl_worker(struct work_struct *work)
{
	struct msm_vblank_work *vbl_work = container_of(work,
						struct msm_vblank_work, work);
	struct msm_drm_private *priv = vbl_work->priv;
	struct msm_kms *kms = priv->kms;

	MSM_FUNC_ENTER("crtc_id=%d enable=%d", vbl_work->crtc_id, vbl_work->enable);
	if (vbl_work->enable)
		kms->funcs->enable_vblank(kms, priv->crtcs[vbl_work->crtc_id]);
	else
		kms->funcs->disable_vblank(kms,	priv->crtcs[vbl_work->crtc_id]);

	kfree(vbl_work);
	MSM_FUNC_EXIT("");
}

static int vblank_ctrl_queue_work(struct msm_drm_private *priv,
					int crtc_id, bool enable)
{
	struct msm_vblank_work *vbl_work;
	int ret = 0;

	MSM_FUNC_ENTER("crtc_id=%d enable=%d", crtc_id, enable);
	vbl_work = kzalloc(sizeof(*vbl_work), GFP_ATOMIC);
	if (!vbl_work) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_WORK(&vbl_work->work, vblank_ctrl_worker);

	vbl_work->crtc_id = crtc_id;
	vbl_work->enable = enable;
	vbl_work->priv = priv;

	queue_work(priv->wq, &vbl_work->work);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_drm_uninit(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct msm_kms *kms = priv->kms;
	struct msm_mdss *mdss = priv->mdss;
	int i;

	MSM_FUNC_ENTER("Starting MSM DRM uninitialization");

	/*
	 * Shutdown the hw if we're far enough along where things might be on.
	 * If we run this too early, we'll end up panicking in any variety of
	 * places. Since we don't register the drm device until late in
	 * msm_drm_init, drm_dev->registered is used as an indicator that the
	 * shutdown will be successful.
	 */
	if (ddev->registered) {
		MSM_DRV_DBG("Unregistering DRM device and shutting down");
		drm_dev_unregister(ddev);
		drm_atomic_helper_shutdown(ddev);
	} else {
		MSM_DRV_DBG("DRM device not registered, skipping shutdown");
	}

	/* We must cancel and cleanup any pending vblank enable/disable
	 * work before msm_irq_uninstall() to avoid work re-enabling an
	 * irq after uninstall has disabled it.
	 */

	MSM_DRV_DBG("Flushing workqueue");
	flush_workqueue(priv->wq);

	/* clean up event worker threads */
	MSM_DRV_DBG("Cleaning up event worker threads for %u CRTCs", priv->num_crtcs);
	for (i = 0; i < priv->num_crtcs; i++) {
		if (priv->event_thread[i].worker) {
			MSM_DRV_DBG("Destroying event thread for CRTC %d", i);
			kthread_destroy_worker(priv->event_thread[i].worker);
		}
	}

	msm_gem_shrinker_cleanup(ddev);

	drm_kms_helper_poll_fini(ddev);

	msm_perf_debugfs_cleanup(priv);
	msm_rd_debugfs_cleanup(priv);

//#ifdef CONFIG_DRM_FBDEV_EMULATION
//	if (fbdev && priv->fbdev)
//		msm_fbdev_free(ddev);
//#endif

	msm_disp_snapshot_destroy(ddev);

	drm_mode_config_cleanup(ddev);

	//pm_runtime_get_sync(dev);
	msm_irq_uninstall(ddev);
	//pm_runtime_put_sync(dev);

	if (kms && kms->funcs)
		kms->funcs->destroy(kms);

	if (priv->vram.paddr) {
		unsigned long attrs = DMA_ATTR_NO_KERNEL_MAPPING;
		drm_mm_takedown(&priv->vram.mm);
		dma_free_attrs(dev, priv->vram.size, NULL,
			       priv->vram.paddr, attrs);
	}

	component_unbind_all(dev, ddev);

	if (mdss && mdss->funcs)
		mdss->funcs->destroy(ddev);

	ddev->dev_private = NULL;
	drm_dev_put(ddev);

	destroy_workqueue(priv->wq);
	kfree(priv);

	MSM_FUNC_EXIT("ret=0");
	return 0;
}

#define KMS_MDP4 4
#define KMS_MDP5 5
#define KMS_DPU  3

static int get_mdp_ver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	return (int) (unsigned long) of_device_get_match_data(dev);
}

#include <linux/of_address.h>

bool msm_use_mmu(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	bool use_mmu;

	MSM_FUNC_ENTER("dev=%p", dev);

	if (priv->disable_iommu) {
		MSM_FUNC_EXIT("use_mmu=0 (disabled via DT)");
		return false;
	}

	/* a2xx comes with its own MMU */
	use_mmu = priv->is_a2xx || iommu_present(&platform_bus_type);
	MSM_FUNC_EXIT("use_mmu=%d", use_mmu);
	return use_mmu;
}

static int msm_init_vram(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct device_node *node;
	unsigned long size = 0;
	int ret = 0;
	int status = 0;
	bool use_vram_carveout;

	MSM_FUNC_ENTER("dev=%p", dev);

	use_vram_carveout = of_property_read_bool(dev->dev->of_node,
						  "qcom,use-vram-carveout");
	priv->disable_iommu = use_vram_carveout;
	if (use_vram_carveout)
		allow_vram_carveout = true;

	/* In the device-tree world, we could have a 'memory-region'
	 * phandle, which gives us a link to our "vram".  Allocating
	 * is all nicely abstracted behind the dma api, but we need
	 * to know the entire size to allocate it all in one go. There
	 * are two cases:
	 *  1) device with no IOMMU, in which case we need exclusive
	 *     access to a VRAM carveout big enough for all gpu
	 *     buffers
	 *  2) device with IOMMU, but where the bootloader puts up
	 *     a splash screen.  In this case, the VRAM carveout
	 *     need only be large enough for fbdev fb.  But we need
	 *     exclusive access to the buffer to avoid the kernel
	 *     using those pages for other purposes (which appears
	 *     as corruption on screen before we have a chance to
	 *     load and do initial modeset)
	 */

	node = of_parse_phandle(dev->dev->of_node, "memory-region", 0);
	if (node) {
		struct resource r;
		ret = of_address_to_resource(node, 0, &r);
		of_node_put(node);
		if (ret)
			goto out;
		size = r.end - r.start;
		DRM_INFO("using VRAM carveout: %lx@%pa\n", size, &r.start);

		/* if we have no IOMMU, then we need to use carveout allocator.
		 * Grab the entire CMA chunk carved out in early startup in
		 * mach-msm:
		 */
	} else if (!msm_use_mmu(dev)) {
		DRM_INFO("using %s VRAM carveout\n", vram);
		size = memparse(vram, NULL);
	}

	DRM_INFO("vram size = %ld\n", size);

	if (size) {
		unsigned long attrs = 0;
		void *p;

		priv->vram.size = size;

		drm_mm_init(&priv->vram.mm, 0, (size >> PAGE_SHIFT) - 1);
		spin_lock_init(&priv->vram.lock);

		attrs |= DMA_ATTR_NO_KERNEL_MAPPING;
		attrs |= DMA_ATTR_WRITE_COMBINE;

		/* note that for no-kernel-mapping, the vaddr returned
		 * is bogus, but non-null if allocation succeeded:
		 */
		p = dma_alloc_attrs(dev->dev, size,
				&priv->vram.paddr, GFP_KERNEL, attrs);
		if (!p) {
			DRM_DEV_ERROR(dev->dev, "failed to allocate VRAM\n");
			priv->vram.paddr = 0;
			ret = -ENOMEM;
			goto out;
		}

		DRM_DEV_INFO(dev->dev, "VRAM: %08x->%08x\n",
				(uint32_t)priv->vram.paddr,
				(uint32_t)(priv->vram.paddr + size));
	}

out:
	status = ret;
	MSM_FUNC_EXIT("ret=%d", status);
	return status;
}

static int msm_drm_init(struct device *dev, const struct drm_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev;
	struct msm_drm_private *priv;
	struct msm_kms *kms;
	struct msm_mdss *mdss;
	int ret, i;

	MSM_FUNC_ENTER("Starting MSM DRM initialization");

	ddev = drm_dev_alloc(drv, dev);
	if (IS_ERR(ddev)) {
		DRM_DEV_ERROR(dev, "failed to allocate drm_device\n");
		MSM_ERROR_DBG("Failed to allocate drm_device, ret=%ld", PTR_ERR(ddev));
		return PTR_ERR(ddev);
	}
	MSM_DRV_DBG("Allocated DRM device successfully");

	platform_set_drvdata(pdev, ddev);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		MSM_ERROR_DBG("Failed to allocate msm_drm_private");
		goto err_put_drm_dev;
	}
	MSM_DRV_DBG("Allocated private data successfully");

	ddev->dev_private = priv;
	priv->dev = ddev;

	MSM_DRV_DBG("Initializing MDSS for MDP version: %d", get_mdp_ver(pdev));
	switch (get_mdp_ver(pdev)) {
	case KMS_MDP5:
		MSM_DRV_DBG("Using MDP5 MDSS initialization");
		ret = mdp5_mdss_init(ddev);
		break;
	case KMS_DPU:
		MSM_DRV_DBG("Using DPU MDSS initialization");
		ret = dpu_mdss_init(ddev);
		break;
	default:
		MSM_DRV_DBG("No MDSS initialization needed for MDP version: %d", get_mdp_ver(pdev));
		ret = 0;
		break;
	}
	if (ret) {
		MSM_ERROR_DBG("MDSS initialization failed, ret=%d", ret);
		goto err_free_priv;
	}

	mdss = priv->mdss;
	MSM_DRV_DBG("MDSS initialized successfully");

	priv->wq = alloc_ordered_workqueue("msm", 0);
	priv->hangcheck_period = DRM_MSM_HANGCHECK_DEFAULT_PERIOD;
	MSM_DRV_DBG("Created workqueue with hangcheck_period=%u", priv->hangcheck_period);

	INIT_LIST_HEAD(&priv->objects);
	mutex_init(&priv->obj_lock);

	INIT_LIST_HEAD(&priv->inactive_willneed);
	INIT_LIST_HEAD(&priv->inactive_dontneed);
	INIT_LIST_HEAD(&priv->inactive_unpinned);
	mutex_init(&priv->mm_lock);
	MSM_DRV_DBG("Initialized GEM object lists and locks");

	/* Teach lockdep about lock ordering wrt. shrinker: */
	fs_reclaim_acquire(GFP_KERNEL);
	might_lock(&priv->mm_lock);
	fs_reclaim_release(GFP_KERNEL);

	drm_mode_config_init(ddev);
	MSM_DRV_DBG("Initialized DRM mode config");

	ret = msm_init_vram(ddev);
	if (ret) {
		MSM_ERROR_DBG("VRAM initialization failed, ret=%d", ret);
		goto err_destroy_mdss;
	}
	MSM_DRV_DBG("VRAM initialized successfully");

	/* Bind all our sub-components: */
	ret = component_bind_all(dev, ddev);
	if (ret) {
		MSM_ERROR_DBG("Component bind all failed, ret=%d", ret);
		goto err_destroy_mdss;
	}
	MSM_DRV_DBG("All components bound successfully");

	dma_set_max_seg_size(dev, UINT_MAX);

	msm_gem_shrinker_init(ddev);
	MSM_DRV_DBG("GEM shrinker initialized");

	MSM_DRV_DBG("Initializing KMS for MDP version: %d", get_mdp_ver(pdev));
	switch (get_mdp_ver(pdev)) {
	case KMS_MDP4:
		MSM_DRV_DBG("Using MDP4 KMS initialization");
		kms = mdp4_kms_init(ddev);
		priv->kms = kms;
		break;
	case KMS_MDP5:
		MSM_DRV_DBG("Using MDP5 KMS initialization");
		kms = mdp5_kms_init(ddev);
		break;
	case KMS_DPU:
		MSM_DRV_DBG("Using DPU KMS initialization");
		kms = dpu_kms_init(ddev);
		priv->kms = kms;
		break;
	default:
		/* valid only for the dummy headless case, where of_node=NULL */
		WARN_ON(dev->of_node);
		MSM_DRV_DBG("No KMS initialization (headless mode)");
		kms = NULL;
		break;
	}

	if (IS_ERR(kms)) {
		DRM_DEV_ERROR(dev, "failed to load kms\n");
		ret = PTR_ERR(kms);
		priv->kms = NULL;
		MSM_ERROR_DBG("KMS initialization failed, ret=%d", ret);
		goto err_msm_uninit;
	}
	MSM_DRV_DBG("KMS initialized successfully");

	/* Enable normalization of plane zpos */
	ddev->mode_config.normalize_zpos = true;

	if (kms) {
		kms->dev = ddev;
		MSM_DRV_DBG("Calling KMS hardware initialization");
		ret = kms->funcs->hw_init(kms);
		if (ret) {
			DRM_DEV_ERROR(dev, "kms hw init failed: %d\n", ret);
			MSM_ERROR_DBG("KMS hardware initialization failed, ret=%d", ret);
			goto err_msm_uninit;
		}
		MSM_DRV_DBG("KMS hardware initialization completed successfully");
	}

	ddev->mode_config.funcs = &mode_config_funcs;
	ddev->mode_config.helper_private = &mode_config_helper_funcs;

	MSM_DRV_DBG("Initializing event threads for %u CRTCs", priv->num_crtcs);
	for (i = 0; i < priv->num_crtcs; i++) {
		/* initialize event thread */
		priv->event_thread[i].crtc_id = priv->crtcs[i]->base.id;
		priv->event_thread[i].dev = ddev;
		MSM_DRV_DBG("Creating event thread for CRTC %d (id=%u)", i, priv->event_thread[i].crtc_id);
		priv->event_thread[i].worker = kthread_create_worker(0,
			"crtc_event:%d", priv->event_thread[i].crtc_id);
		if (IS_ERR(priv->event_thread[i].worker)) {
			ret = PTR_ERR(priv->event_thread[i].worker);
			DRM_DEV_ERROR(dev, "failed to create crtc_event kthread\n");
			ret = PTR_ERR(priv->event_thread[i].worker);
			MSM_ERROR_DBG("Failed to create event thread for CRTC %d, ret=%d", i, ret);
			goto err_msm_uninit;
		}

		sched_set_fifo(priv->event_thread[i].worker->task);
		MSM_DRV_DBG("Event thread created for CRTC %d", i);
	}

	ret = drm_vblank_init(ddev, priv->num_crtcs);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "failed to initialize vblank\n");
		MSM_ERROR_DBG("VBlank initialization failed, ret=%d", ret);
		goto err_msm_uninit;
	}
	MSM_DRV_DBG("VBlank initialized for %u CRTCs", priv->num_crtcs);

	if (kms) {
		//pm_runtime_get_sync(dev);
		MSM_DRV_DBG("Installing IRQ handler for IRQ %d", kms->irq);
		ret = msm_irq_install(ddev, kms->irq);
		//pm_runtime_put_sync(dev);
		if (ret < 0) {
			DRM_DEV_ERROR(dev, "failed to install IRQ handler\n");
			MSM_ERROR_DBG("IRQ installation failed, ret=%d", ret);
			goto err_msm_uninit;
		}
		MSM_DRV_DBG("IRQ handler installed successfully");
	}

	MSM_DRV_DBG("Registering DRM device");
	ret = drm_dev_register(ddev, 0);
	if (ret) {
		MSM_ERROR_DBG("DRM device registration failed, ret=%d", ret);
		goto err_msm_uninit;
	}
	MSM_DRV_DBG("DRM device registered successfully");

	if (kms) {
		ret = msm_disp_snapshot_init(ddev);
		if (ret)
			DRM_DEV_ERROR(dev, "msm_disp_snapshot_init failed ret = %d\n", ret);
		MSM_DRV_DBG("Display snapshot initialized");
	}
	drm_mode_config_reset(ddev);
	MSM_DRV_DBG("DRM mode config reset completed");

//#ifdef CONFIG_DRM_FBDEV_EMULATION
//	if (kms && fbdev)
//		priv->fbdev = msm_fbdev_init(ddev);
//#endif

	ret = msm_debugfs_late_init(ddev);
	if (ret) {
		MSM_ERROR_DBG("Debugfs late init failed, ret=%d", ret);
		goto err_msm_uninit;
	}
	MSM_DRV_DBG("Debugfs late initialization completed");

	drm_kms_helper_poll_init(ddev);
	MSM_DRV_DBG("KMS helper poll initialized");

	MSM_FUNC_EXIT("Initialization completed successfully");
	return 0;

err_msm_uninit:
	msm_drm_uninit(dev);
	return ret;
err_destroy_mdss:
	if (mdss && mdss->funcs)
		mdss->funcs->destroy(ddev);
err_free_priv:
	kfree(priv);
err_put_drm_dev:
	drm_dev_put(ddev);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

/*
 * DRM operations:
 */

static void load_gpu(struct drm_device *dev)
{
	static DEFINE_MUTEX(init_lock);
	struct msm_drm_private *priv = dev->dev_private;

	MSM_FUNC_ENTER("Loading GPU");

	mutex_lock(&init_lock);

	if (!priv->gpu) {
		MSM_DRV_DBG("Loading Adreno GPU");
		priv->gpu = adreno_load_gpu(dev);
		MSM_DRV_DBG("GPU loaded successfully");
	} else {
		MSM_DRV_DBG("GPU already loaded");
	}

	mutex_unlock(&init_lock);
	MSM_FUNC_EXIT("");
}

static int context_init(struct drm_device *dev, struct drm_file *file)
{
	static atomic_t ident = ATOMIC_INIT(0);
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx;
	int ret = 0;

	MSM_FUNC_ENTER("Starting context initialization");

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		MSM_ERROR_DBG("Failed to allocate file private context");
		ret = -ENOMEM;
		goto out;
	}
	MSM_DRV_DBG("Allocated file private context successfully");

	INIT_LIST_HEAD(&ctx->submitqueues);
	rwlock_init(&ctx->queuelock);

	kref_init(&ctx->ref);
	MSM_DRV_DBG("Initializing submit queue");
	msm_submitqueue_init(dev, ctx);

	MSM_DRV_DBG("Creating private address space");
	ctx->aspace = msm_gpu_create_private_address_space(priv->gpu, current);
	file->driver_priv = ctx;

	ctx->seqno = atomic_inc_return(&ident);
	MSM_DRV_DBG("Context initialized with seqno: %d", ctx->seqno);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_open(struct drm_device *dev, struct drm_file *file)
{
	int ret;
	MSM_FUNC_ENTER("Opening MSM DRM device");
	
	/* For now, load gpu on open.. to avoid the requirement of having
	 * firmware in the initrd.
	 */
	//MSM_DRV_DBG("Loading GPU");
	//load_gpu(dev);

	//MSM_DRV_DBG("Initializing context");
	//ret = context_init(dev, file);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void context_close(struct msm_file_private *ctx)
{
	MSM_FUNC_ENTER("ctx=%p", ctx);
	msm_submitqueue_close(ctx);
	msm_file_private_put(ctx);
	MSM_FUNC_EXIT("");
}

static void msm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx = file->driver_priv;

	MSM_FUNC_ENTER("Closing MSM DRM device");
	return;

	mutex_lock(&dev->struct_mutex);
	if (ctx == priv->lastctx) {
		MSM_DRV_DBG("Clearing last context");
		priv->lastctx = NULL;
	}
	mutex_unlock(&dev->struct_mutex);

	MSM_DRV_DBG("Closing context");
	context_close(ctx);
	MSM_FUNC_EXIT("");
}

int msm_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int ret;

	MSM_FUNC_ENTER("Enabling vblank for pipe %u", pipe);

	if (!kms) {
		MSM_ERROR_DBG("KMS not available");
		ret = -ENXIO;
		goto out;
	}
	drm_dbg_vbl(dev, "crtc=%u", pipe);
	MSM_DRV_DBG("Enabling vblank for pipe %u", pipe);
	ret = vblank_ctrl_queue_work(priv, pipe, true);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

void msm_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int ret = 0;

	MSM_FUNC_ENTER("Disabling vblank for pipe %u", pipe);

	if (!kms) {
		MSM_DRV_DBG("KMS not available");
		ret = -ENXIO;
		goto out;
	}
	drm_dbg_vbl(dev, "crtc=%u", pipe);
	MSM_DRV_DBG("Disabling vblank for pipe %u", pipe);
	vblank_ctrl_queue_work(priv, pipe, false);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
}

/*
 * DRM ioctls:
 */

static int msm_ioctl_get_param(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_param *args = data;
	struct msm_gpu *gpu;
	int ret;

	MSM_FUNC_ENTER("pipe=%u", args->pipe);

	if (args->pipe != MSM_PIPE_3D0) {
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	gpu = priv->gpu;

	if (!gpu) {
		MSM_FUNC_EXIT("ret=%d", -ENXIO);
		return -ENXIO;
	}

	ret = gpu->funcs->get_param(gpu, args->param, &args->value);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_gem_new(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_new *args = data;
	int ret;

	MSM_FUNC_ENTER("size=%llu", args->size);

	if (args->flags & ~MSM_BO_FLAGS) {
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	ret = msm_gem_new_handle(dev, file, args->size,
			args->flags, &args->handle, NULL);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static inline ktime_t to_ktime(struct drm_msm_timespec timeout)
{
	return ktime_set(timeout.tv_sec, timeout.tv_nsec);
}

static int msm_ioctl_gem_cpu_prep(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_prep *args = data;
	struct drm_gem_object *obj;
	ktime_t timeout = to_ktime(args->timeout);
	int ret;

	MSM_FUNC_ENTER("handle=%u op=0x%x", args->handle, args->op);

	if (args->op & ~MSM_PREP_FLAGS) {
		DRM_ERROR("invalid op: %08x\n", args->op);
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		MSM_FUNC_EXIT("ret=%d", -ENOENT);
		return -ENOENT;
	}

	ret = msm_gem_cpu_prep(obj, args->op, &timeout);

	drm_gem_object_put(obj);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_gem_cpu_fini(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_fini *args = data;
	struct drm_gem_object *obj;
	int ret;

	MSM_FUNC_ENTER("handle=%u", args->handle);

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		MSM_FUNC_EXIT("ret=%d", -ENOENT);
		return -ENOENT;
	}

	ret = msm_gem_cpu_fini(obj);

	drm_gem_object_put(obj);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_gem_info_iova(struct drm_device *dev,
		struct drm_file *file, struct drm_gem_object *obj,
		uint64_t *iova)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx = file->driver_priv;
	int ret;

	MSM_FUNC_ENTER("obj=%p", obj);

	if (!priv->gpu) {
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	/*
	 * Don't pin the memory here - just get an address so that userspace can
	 * be productive
	 */
	ret = msm_gem_get_iova(obj, ctx->aspace, iova);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_gem_info(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_info *args = data;
	struct drm_gem_object *obj;
	struct msm_gem_object *msm_obj;
	int i, ret = 0;

	MSM_FUNC_ENTER("handle=%u info=0x%x", args->handle, args->info);

	if (args->pad) {
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
	case MSM_INFO_GET_IOVA:
		/* value returned as immediate, not pointer, so len==0: */
		if (args->len) {
			MSM_FUNC_EXIT("ret=%d", -EINVAL);
			return -EINVAL;
		}
		break;
	case MSM_INFO_SET_NAME:
	case MSM_INFO_GET_NAME:
		break;
	default:
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		MSM_FUNC_EXIT("ret=%d", -ENOENT);
		return -ENOENT;
	}

	msm_obj = to_msm_bo(obj);

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
		args->value = msm_gem_mmap_offset(obj);
		break;
	case MSM_INFO_GET_IOVA:
		ret = msm_ioctl_gem_info_iova(dev, file, obj, &args->value);
		break;
	case MSM_INFO_SET_NAME:
		/* length check should leave room for terminating null: */
		if (args->len >= sizeof(msm_obj->name)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(msm_obj->name, u64_to_user_ptr(args->value),
				   args->len)) {
			msm_obj->name[0] = '\0';
			ret = -EFAULT;
			break;
		}
		msm_obj->name[args->len] = '\0';
		for (i = 0; i < args->len; i++) {
			if (!isprint(msm_obj->name[i])) {
				msm_obj->name[i] = '\0';
				break;
			}
		}
		break;
	case MSM_INFO_GET_NAME:
		if (args->value && (args->len < strlen(msm_obj->name))) {
			ret = -EINVAL;
			break;
		}
		args->len = strlen(msm_obj->name);
		if (args->value) {
			if (copy_to_user(u64_to_user_ptr(args->value),
					 msm_obj->name, args->len))
				ret = -EFAULT;
		}
		break;
	}

	drm_gem_object_put(obj);

	MSM_FUNC_EXIT("ret=%d", ret);

	return ret;
}

static int wait_fence(struct msm_gpu_submitqueue *queue, uint32_t fence_id,
		      ktime_t timeout)
{
	struct dma_fence *fence = NULL;
	int ret = 0;

	MSM_FUNC_ENTER("queue=%p fence_id=%u", queue, fence_id);

	if (fence_id > queue->last_fence) {
		DRM_ERROR_RATELIMITED("waiting on invalid fence: %u (of %u)\n",
				      fence_id, queue->last_fence);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Map submitqueue scoped "seqno" (which is actually an idr key)
	 * back to underlying dma-fence
	 *
	 * The fence is removed from the fence_idr when the submit is
	 * retired, so if the fence is not found it means there is nothing
	 * to wait for
	 */
	ret = mutex_lock_interruptible(&queue->lock);
	if (ret)
		goto out;
	fence = idr_find(&queue->fence_idr, fence_id);
	if (fence)
		fence = dma_fence_get_rcu(fence);
	mutex_unlock(&queue->lock);

	if (!fence)
		goto out;

	ret = dma_fence_wait_timeout(fence, true, timeout_to_jiffies(&timeout));
	if (ret == 0) {
		ret = -ETIMEDOUT;
	} else if (ret != -ERESTARTSYS) {
		ret = 0;
	}

	dma_fence_put(fence);

	out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_wait_fence(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_wait_fence *args = data;
	struct msm_gpu_submitqueue *queue;
	int ret;

	MSM_FUNC_ENTER("queueid=%u", args->queueid);

	if (args->pad) {
		DRM_ERROR("invalid pad: %08x\n", args->pad);
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	if (!priv->gpu)
		ret = 0;
		MSM_FUNC_EXIT("ret=%d", ret);
		return ret;

	queue = msm_submitqueue_get(file->driver_priv, args->queueid);
	if (!queue) {
		MSM_FUNC_EXIT("ret=%d", -ENOENT);
		return -ENOENT;
	}

	ret = wait_fence(queue, args->fence, to_ktime(args->timeout));

	msm_submitqueue_put(queue);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_gem_madvise(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_madvise *args = data;
	struct drm_gem_object *obj;
	int ret;

	MSM_FUNC_ENTER("handle=%u madv=%u", args->handle, args->madv);

	switch (args->madv) {
	case MSM_MADV_DONTNEED:
	case MSM_MADV_WILLNEED:
		break;
	default:
		MSM_FUNC_EXIT("ret=%d", -EINVAL);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		MSM_FUNC_EXIT("ret=%d", -ENOENT);
		return -ENOENT;
	}

	ret = msm_gem_madvise(obj, args->madv);
	if (ret >= 0) {
		args->retained = ret;
		ret = 0;
	}

	drm_gem_object_put(obj);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}


static int msm_ioctl_submitqueue_new(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_submitqueue *args = data;
	int ret;

	MSM_FUNC_ENTER("prio=%u flags=0x%x", args->prio, args->flags);

	if (args->flags & ~MSM_SUBMITQUEUE_FLAGS) {
		ret = -EINVAL;
		goto out;
	}

	ret = msm_submitqueue_create(dev, file->driver_priv, args->prio,
		args->flags, &args->id);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_submitqueue_query(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	int ret;

	MSM_FUNC_ENTER("");
	ret = msm_submitqueue_query(dev, file->driver_priv, data);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_ioctl_submitqueue_close(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	u32 id = *(u32 *) data;
	int ret;

	MSM_FUNC_ENTER("id=%u", id);
	ret = msm_submitqueue_remove(file->driver_priv, id);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static const struct drm_ioctl_desc msm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MSM_GET_PARAM,    msm_ioctl_get_param,    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_NEW,      msm_ioctl_gem_new,      DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_INFO,     msm_ioctl_gem_info,     DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_PREP, msm_ioctl_gem_cpu_prep, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_FINI, msm_ioctl_gem_cpu_fini, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_SUBMIT,   msm_ioctl_gem_submit,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_WAIT_FENCE,   msm_ioctl_wait_fence,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_MADVISE,  msm_ioctl_gem_madvise,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_NEW,   msm_ioctl_submitqueue_new,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_CLOSE, msm_ioctl_submitqueue_close, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_QUERY, msm_ioctl_submitqueue_query, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(fops);

static const struct drm_driver msm_driver = {
	.driver_features    = DRIVER_GEM |
				DRIVER_RENDER |
				DRIVER_ATOMIC |
				DRIVER_MODESET |
				DRIVER_SYNCOBJ,
	.open               = msm_open,
	.postclose           = msm_postclose,
	.lastclose          = drm_fb_helper_lastclose,
	.dumb_create        = msm_gem_dumb_create,
	.dumb_map_offset    = msm_gem_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import_sg_table = msm_gem_prime_import_sg_table,
	.gem_prime_mmap     = drm_gem_prime_mmap,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init       = msm_debugfs_init,
#endif
	.ioctls             = msm_ioctls,
	.num_ioctls         = ARRAY_SIZE(msm_ioctls),
	.fops               = &fops,
	.name               = "msm",
	.desc               = "MSM Snapdragon DRM",
	.date               = "20130625",
	.major              = MSM_VERSION_MAJOR,
	.minor              = MSM_VERSION_MINOR,
	.patchlevel         = MSM_VERSION_PATCHLEVEL,
};

static int __maybe_unused msm_runtime_suspend(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct msm_mdss *mdss = priv->mdss;
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	DBG("");

	if (mdss && mdss->funcs)
		ret = mdss->funcs->disable(mdss);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int __maybe_unused msm_runtime_resume(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct msm_mdss *mdss = priv->mdss;
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	DBG("");

	if (mdss && mdss->funcs)
		ret = mdss->funcs->enable(mdss);

	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int __maybe_unused msm_pm_suspend(struct device *dev)
{
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	if (pm_runtime_suspended(dev))
		goto out;

	ret = msm_runtime_suspend(dev);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int __maybe_unused msm_pm_resume(struct device *dev)
{
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	if (pm_runtime_suspended(dev))
		goto out;

	ret = msm_runtime_resume(dev);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int __maybe_unused msm_pm_prepare(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev ? ddev->dev_private : NULL;
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	if (!priv || !priv->kms)
		goto out;

	ret = drm_mode_config_helper_suspend(ddev);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void __maybe_unused msm_pm_complete(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev ? ddev->dev_private : NULL;

	MSM_FUNC_ENTER("dev=%p", dev);

	if (!priv || !priv->kms)
		goto out;

	drm_mode_config_helper_resume(ddev);

out:
	MSM_FUNC_EXIT("");
}

static const struct dev_pm_ops msm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_pm_suspend, msm_pm_resume)
	SET_RUNTIME_PM_OPS(msm_runtime_suspend, msm_runtime_resume, NULL)
	.prepare = msm_pm_prepare,
	.complete = msm_pm_complete,
};

/*
 * Componentized driver support:
 */

/*
 * NOTE: duplication of the same code as exynos or imx (or probably any other).
 * so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/*
 * Identify what components need to be added by parsing what remote-endpoints
 * our MDP output ports are connected to. In the case of LVDS on MDP4, there
 * is no external component that we need to add since LVDS is within MDP4
 * itself.
 */
static int add_components_mdp(struct device *mdp_dev,
			      struct component_match **matchptr)
{
	struct device_node *np = mdp_dev->of_node;
	struct device_node *ep_node;
	struct device *master_dev;
	int ret = 0;

	MSM_FUNC_ENTER("mdp_dev=%p", mdp_dev);

	/*
	 * on MDP4 based platforms, the MDP platform device is the component
	 * master that adds other display interface components to itself.
	 *
	 * on MDP5 based platforms, the MDSS platform device is the component
	 * master that adds MDP5 and other display interface components to
	 * itself.
	 */
	if (of_device_is_compatible(np, "qcom,mdp4"))
		master_dev = mdp_dev;
	else
		master_dev = mdp_dev->parent;

	for_each_endpoint_of_node(np, ep_node) {
		struct device_node *intf;
		struct of_endpoint ep;

		ret = of_graph_parse_endpoint(ep_node, &ep);
		if (ret) {
			DRM_DEV_ERROR(mdp_dev, "unable to parse port endpoint\n");
			of_node_put(ep_node);
			goto out;
		}

		/*
		 * The LCDC/LVDS port on MDP4 is a speacial case where the
		 * remote-endpoint isn't a component that we need to add
		 */
		if (of_device_is_compatible(np, "qcom,mdp4") &&
		    ep.port == 0)
			continue;

		/*
		 * It's okay if some of the ports don't have a remote endpoint
		 * specified. It just means that the port isn't connected to
		 * any external interface.
		 */
		intf = of_graph_get_remote_port_parent(ep_node);
		if (!intf)
			continue;

		if (of_device_is_available(intf))
			drm_of_component_match_add(master_dev, matchptr,
						   compare_of, intf);

		of_node_put(intf);
	}

	ret = 0;

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int compare_name_mdp(struct device *dev, void *data)
{
	return (strstr(dev_name(dev), "mdp") != NULL);
}

static int add_display_components(struct platform_device *pdev,
				  struct component_match **matchptr)
{
	struct device *mdp_dev;
	struct device *dev = &pdev->dev;
	int ret;

	MSM_FUNC_ENTER("pdev=%p", pdev);

	/*
	 * MDP5/DPU based devices don't have a flat hierarchy. There is a top
	 * level parent: MDSS, and children: MDP5/DPU, DSI, HDMI, eDP etc.
	 * Populate the children devices, find the MDP5/DPU node, and then add
	 * the interfaces to our components list.
	 */
	switch (get_mdp_ver(pdev)) {
	case KMS_MDP5:
	case KMS_DPU:
		DRM_INFO("%s - %d\n", __func__, __LINE__);
		ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
		if (ret) {
			DRM_DEV_ERROR(dev, "failed to populate children devices\n");
			goto out;
		}

		mdp_dev = device_find_child(dev, NULL, compare_name_mdp);
		if (!mdp_dev) {
			DRM_DEV_ERROR(dev, "failed to find MDSS MDP node\n");
			of_platform_depopulate(dev);
			ret = -ENODEV;
			goto out;
		}

		put_device(mdp_dev);

		/* add the MDP component itself */
		drm_of_component_match_add(dev, matchptr, compare_of,
					   mdp_dev->of_node);
		break;
	case KMS_MDP4:
		/* MDP4 */
		mdp_dev = dev;
		break;
	}

	ret = add_components_mdp(mdp_dev, matchptr);
	if (ret)
		of_platform_depopulate(dev);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

/*
 * We don't know what's the best binding to link the gpu with the drm device.
 * Fow now, we just hunt for all the possible gpus that we support, and add them
 * as components.
 */
static const struct of_device_id msm_gpu_match[] = {
	{ .compatible = "qcom,adreno" },
	{ .compatible = "qcom,adreno-3xx" },
	{ .compatible = "amd,imageon" },
	{ .compatible = "qcom,kgsl-3d0" },
	{ },
};

static int add_gpu_components(struct device *dev,
			      struct component_match **matchptr)
{
	struct device_node *np;
	int ret = 0;

	MSM_FUNC_ENTER("dev=%p", dev);

	np = of_find_matching_node(NULL, msm_gpu_match);
	if (!np)
		goto out;

	if (of_device_is_available(np))
		drm_of_component_match_add(dev, matchptr, compare_of, np);

	of_node_put(np);

out:
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static int msm_drm_bind(struct device *dev)
{
	int ret;

	MSM_FUNC_ENTER("dev=%p", dev);
	DRM_INFO("%s - %d\n", __func__, __LINE__);
	ret = msm_drm_init(dev, &msm_driver);
	MSM_FUNC_EXIT("ret=%d", ret);
	return ret;
}

static void msm_drm_unbind(struct device *dev)
{
	MSM_FUNC_ENTER("dev=%p", dev);
	msm_drm_uninit(dev);
	MSM_FUNC_EXIT("");
}

static const struct component_master_ops msm_drm_ops = {
	.bind = msm_drm_bind,
	.unbind = msm_drm_unbind,
};

/*
 * Platform driver:
 */

static int msm_pdev_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	int ret;

	MSM_FUNC_ENTER("Starting platform device probe");

	MSM_DRV_DBG("MDP version: %d", get_mdp_ver(pdev));
	if (get_mdp_ver(pdev)) {
		MSM_DRV_DBG("Adding display components");
		ret = add_display_components(pdev, &match);
		if (ret) {
			MSM_ERROR_DBG("Failed to add display components, ret=%d", ret);
			goto out;
		}
		MSM_DRV_DBG("Display components added successfully");
	}

	//ret = add_gpu_components(&pdev->dev, &match);
	//if (ret) {
	//	goto fail;
	//}

	/* on all devices that I am aware of, iommu's which can map
	 * any address the cpu can see are used:
	 */
	MSM_DRV_DBG("Setting DMA mask");
	ret = dma_set_mask_and_coherent(&pdev->dev, ~0);
	if (ret) {
		MSM_ERROR_DBG("Failed to set DMA mask, ret=%d", ret);
		goto fail;
	}

	MSM_DRV_DBG("Adding component master");
	ret = component_master_add_with_match(&pdev->dev, &msm_drm_ops, match);
	if (ret) {
		MSM_ERROR_DBG("Failed to add component master, ret=%d", ret);
		goto fail;
	}

	ret = 0;
	goto out;

fail:
	MSM_DRV_DBG("Probe failed, cleaning up");
	of_platform_depopulate(&pdev->dev);

out:
	MSM_FUNC_EXIT("Platform device probe completed%s", ret ? " with errors" : " successfully");
	return ret;
}

static int msm_pdev_remove(struct platform_device *pdev)
{
	MSM_FUNC_ENTER("Removing platform device");
	
	MSM_DRV_DBG("Removing component master");
	component_master_del(&pdev->dev, &msm_drm_ops);
	
	MSM_DRV_DBG("Depopulating platform devices");
	of_platform_depopulate(&pdev->dev);

	MSM_FUNC_EXIT("Platform device removed successfully");
	return 0;
}

static void msm_pdev_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct msm_drm_private *priv = drm ? drm->dev_private : NULL;

	MSM_FUNC_ENTER("Shutting down platform device");

	if (!priv || !priv->kms) {
		MSM_DRV_DBG("No private data or KMS available for shutdown");
		goto out;
	}

	MSM_DRV_DBG("Shutting down DRM device");
	drm_atomic_helper_shutdown(drm);

out:
	MSM_FUNC_EXIT("");
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "qcom,mdp4", .data = (void *)KMS_MDP4 },
	{ .compatible = "qcom,mdss", .data = (void *)KMS_MDP5 },
	{ .compatible = "qcom,sdm845-mdss", .data = (void *)KMS_DPU },
	{ .compatible = "qcom,sc7180-mdss", .data = (void *)KMS_DPU },
	{ .compatible = "qcom,sc7280-mdss", .data = (void *)KMS_DPU },
	{ .compatible = "qcom,sm8150-mdss", .data = (void *)KMS_DPU },
	{ .compatible = "qcom,sm8250-mdss", .data = (void *)KMS_DPU },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static struct platform_driver msm_platform_driver = {
	.probe      = msm_pdev_probe,
	.remove     = msm_pdev_remove,
	.shutdown   = msm_pdev_shutdown,
	.driver     = {
		.name   = "msm",
		.of_match_table = dt_match,
		//.pm     = &msm_pm_ops,
	},
};

static int __init msm_drm_register(void)
{
	if (!modeset)
		return -EINVAL;

	DBG("init");
	MSM_FUNC_ENTER();

	msm_mdp_register();
	//msm_dpu_register();
	msm_dsi_register();
	//msm_edp_register();
	//msm_hdmi_register();
	//msm_dp_register();
	//adreno_register();

	MSM_FUNC_EXIT();
	return platform_driver_register(&msm_platform_driver);
}

static void __exit msm_drm_unregister(void)
{
	DBG("fini");
	MSM_FUNC_ENTER();
	platform_driver_unregister(&msm_platform_driver);
	//msm_dp_unregister();
	//msm_hdmi_unregister();
	//adreno_unregister();
	//msm_edp_unregister();
	msm_dsi_unregister();
	msm_mdp_unregister();
	//msm_dpu_unregister();
	MSM_FUNC_EXIT();
}

module_init(msm_drm_register);
module_exit(msm_drm_unregister);

MODULE_AUTHOR("Rob Clark <robdclark@gmail.com");
MODULE_DESCRIPTION("MSM DRM Driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include "dsi.h"

struct drm_encoder *msm_dsi_get_encoder(struct msm_dsi *msm_dsi)
{
	MSM_FUNC_ENTER("[DSI] get_encoder msm_dsi=%p", msm_dsi);
	if (!msm_dsi || !msm_dsi_device_connected(msm_dsi))
	{
		MSM_FUNC_EXIT("[DSI] encoder=NULL");
		return NULL;
	}

	MSM_FUNC_EXIT("[DSI] encoder=%p", msm_dsi->encoder);
	return msm_dsi->encoder;
}

bool msm_dsi_is_cmd_mode(struct msm_dsi *msm_dsi)
{
	unsigned long host_flags;
	bool cmd_mode;

	MSM_FUNC_ENTER("[DSI] is_cmd_mode msm_dsi=%p", msm_dsi);
	host_flags = msm_dsi_host_get_mode_flags(msm_dsi->host);
	cmd_mode = !(host_flags & MIPI_DSI_MODE_VIDEO);
	MSM_FUNC_EXIT("[DSI] cmd_mode=%d", cmd_mode);
	return cmd_mode;
}

static int dsi_get_phy(struct msm_dsi *msm_dsi)
{
	struct platform_device *pdev = msm_dsi->pdev;
	struct platform_device *phy_pdev = NULL;
	struct device_node *phy_node;
	int ret = 0;

	MSM_FUNC_ENTER("[DSI] get_phy msm_dsi=%p", msm_dsi);

	phy_node = of_parse_phandle(pdev->dev.of_node, "phys", 0);
	if (!phy_node) {
		DRM_DEV_ERROR(&pdev->dev, "cannot find phy device\n");
		ret = -ENXIO;
		goto out;
	}

	phy_pdev = of_find_device_by_node(phy_node);
	if (phy_pdev) {
		msm_dsi->phy = platform_get_drvdata(phy_pdev);
		msm_dsi->phy_dev = &phy_pdev->dev;
	}

	of_node_put(phy_node);

	if (!phy_pdev || !msm_dsi->phy) {
		DRM_DEV_ERROR(&pdev->dev, "%s: phy driver is not ready\n", __func__);
		ret = -EPROBE_DEFER;
		goto out;
	}

out:
	MSM_FUNC_EXIT("[DSI] ret=%d", ret);
	return ret;
}

static void dsi_destroy(struct msm_dsi *msm_dsi)
{
	if (!msm_dsi)
		return;

	MSM_FUNC_ENTER("[DSI] destroy msm_dsi=%p", msm_dsi);

	msm_dsi_manager_unregister(msm_dsi);

	if (msm_dsi->phy_dev) {
		put_device(msm_dsi->phy_dev);
		msm_dsi->phy = NULL;
		msm_dsi->phy_dev = NULL;
	}

	if (msm_dsi->host) {
		msm_dsi_host_destroy(msm_dsi->host);
		msm_dsi->host = NULL;
	}

	platform_set_drvdata(msm_dsi->pdev, NULL);
	MSM_FUNC_EXIT("[DSI] destroy msm_dsi=%p", msm_dsi);
}

static struct msm_dsi *dsi_init(struct platform_device *pdev)
{
	struct msm_dsi *msm_dsi;
	int ret;

	MSM_FUNC_ENTER("[DSI] init pdev=%p", pdev);

	if (!pdev) {
		MSM_FUNC_EXIT("[DSI] ret=%d", -ENXIO);
		return ERR_PTR(-ENXIO);
	}

	msm_dsi = devm_kzalloc(&pdev->dev, sizeof(*msm_dsi), GFP_KERNEL);
	if (!msm_dsi) {
		MSM_FUNC_EXIT("[DSI] ret=%d", -ENOMEM);
		return ERR_PTR(-ENOMEM);
	}
	DBG("dsi probed=%p", msm_dsi);

	msm_dsi->id = -1;
	msm_dsi->pdev = pdev;
	platform_set_drvdata(pdev, msm_dsi);

	/* Init dsi host */
	ret = msm_dsi_host_init(msm_dsi);
	if (ret)
		goto destroy_dsi;

	/* GET dsi PHY */
	ret = dsi_get_phy(msm_dsi);
	if (ret)
		goto destroy_dsi;

	/* Register to dsi manager */
	ret = msm_dsi_manager_register(msm_dsi);
	if (ret)
		goto destroy_dsi;

	MSM_FUNC_EXIT("[DSI] msm_dsi=%p", msm_dsi);
	return msm_dsi;

destroy_dsi:
	dsi_destroy(msm_dsi);
	MSM_FUNC_EXIT("[DSI] ret=%d", ret);
	return ERR_PTR(ret);
}

static int dsi_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct msm_drm_private *priv = drm->dev_private;
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_dsi *msm_dsi;
	int ret = 0;

	MSM_FUNC_ENTER("[DSI] bind dev=%p master=%p", dev, master);
	DBG("");
	msm_dsi = dsi_init(pdev);
	if (IS_ERR(msm_dsi)) {
		/* Don't fail the bind if the dsi port is not connected */
		ret = PTR_ERR(msm_dsi);
		if (ret == -ENODEV)
			ret = 0;
		goto out;
	}

	priv->dsi[msm_dsi->id] = msm_dsi;

	ret = 0;

out:
	MSM_FUNC_EXIT("[DSI] bind ret=%d", ret);
	return ret;
}

static void dsi_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct msm_drm_private *priv = drm->dev_private;
	struct msm_dsi *msm_dsi = dev_get_drvdata(dev);
	int id = msm_dsi->id;

	MSM_FUNC_ENTER("[DSI] unbind dev=%p master=%p", dev, master);
	if (priv->dsi[id]) {
		dsi_destroy(msm_dsi);
		priv->dsi[id] = NULL;
	}
	MSM_FUNC_EXIT("[DSI] unbind dev=%p", dev);
}

static const struct component_ops dsi_ops = {
	.bind   = dsi_bind,
	.unbind = dsi_unbind,
};

static int dsi_dev_probe(struct platform_device *pdev)
{
	int ret;

	MSM_FUNC_ENTER("[DSI] dev_probe pdev=%p", pdev);
	ret = component_add(&pdev->dev, &dsi_ops);
	MSM_FUNC_EXIT("[DSI] dev_probe ret=%d", ret);
	return ret;
}

static int dsi_dev_remove(struct platform_device *pdev)
{
	MSM_FUNC_ENTER("[DSI] dev_remove pdev=%p", pdev);
	DBG("");
	component_del(&pdev->dev, &dsi_ops);
	MSM_FUNC_EXIT("[DSI] dev_remove ret=0");
	return 0;
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "qcom,mdss-dsi-ctrl" },
	{}
};

static const struct dev_pm_ops dsi_pm_ops = {
	SET_RUNTIME_PM_OPS(msm_dsi_runtime_suspend, msm_dsi_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver dsi_driver = {
	.probe = dsi_dev_probe,
	.remove = dsi_dev_remove,
	.driver = {
		.name = "msm_dsi",
		.of_match_table = dt_match,
		//.pm = &dsi_pm_ops,
	},
};

void __init msm_dsi_register(void)
{
	MSM_FUNC_ENTER("[DSI] register");
	DBG("");
	msm_dsi_phy_driver_register();
	platform_driver_register(&dsi_driver);
	MSM_FUNC_EXIT("[DSI] register");
}

void __exit msm_dsi_unregister(void)
{
	MSM_FUNC_ENTER("[DSI] unregister");
	DBG("");
	msm_dsi_phy_driver_unregister();
	platform_driver_unregister(&dsi_driver);
	MSM_FUNC_EXIT("[DSI] unregister");
}

int msm_dsi_modeset_init(struct msm_dsi *msm_dsi, struct drm_device *dev,
			 struct drm_encoder *encoder)
{
	struct msm_drm_private *priv;
	struct drm_bridge *ext_bridge;
	int ret;

	MSM_FUNC_ENTER("[DSI] modeset_init msm_dsi=%p dev=%p encoder=%p",
		       msm_dsi, dev, encoder);

	if (WARN_ON(!encoder) || WARN_ON(!msm_dsi) || WARN_ON(!dev))
	{
		MSM_FUNC_EXIT("[DSI] ret=%d", -EINVAL);
		return -EINVAL;
	}

	priv = dev->dev_private;
	msm_dsi->dev = dev;

	ret = msm_dsi_host_modeset_init(msm_dsi->host, dev);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to modeset init host: %d\n", ret);
		goto fail;
	}

	if (!msm_dsi_manager_validate_current_config(msm_dsi->id)) {
		ret = -EINVAL;
		goto fail;
	}

	msm_dsi->encoder = encoder;

	msm_dsi->bridge = msm_dsi_manager_bridge_init(msm_dsi->id);
	if (IS_ERR(msm_dsi->bridge)) {
		ret = PTR_ERR(msm_dsi->bridge);
		DRM_DEV_ERROR(dev->dev, "failed to create dsi bridge: %d\n", ret);
		msm_dsi->bridge = NULL;
		goto fail;
	}

	/* Initialize the internal panel or external bridge */
	ext_bridge = msm_dsi_host_get_bridge(msm_dsi->host);

	if (ext_bridge)
		msm_dsi->connector =
			msm_dsi_manager_ext_bridge_init(msm_dsi->id);
	else
		msm_dsi->connector =
			msm_dsi_manager_connector_init(msm_dsi->id);

	if (IS_ERR(msm_dsi->connector)) {
		ret = PTR_ERR(msm_dsi->connector);
		DRM_DEV_ERROR(dev->dev,
			"failed to create dsi connector: %d\n", ret);
		msm_dsi->connector = NULL;
		goto fail;
	}

	priv->bridges[priv->num_bridges++]       = msm_dsi->bridge;
	priv->connectors[priv->num_connectors++] = msm_dsi->connector;

	ret = 0;
	goto out;
fail:
	/* bridge/connector are normally destroyed by drm: */
	if (msm_dsi->bridge) {
		msm_dsi_manager_bridge_destroy(msm_dsi->bridge);
		msm_dsi->bridge = NULL;
	}

	/* don't destroy connector if we didn't make it */
	if (msm_dsi->connector && !msm_dsi->external_bridge)
		msm_dsi->connector->funcs->destroy(msm_dsi->connector);

	msm_dsi->connector = NULL;

out:
	MSM_FUNC_EXIT("[DSI] ret=%d", ret);
	return ret;
}

void msm_dsi_snapshot(struct msm_disp_state *disp_state, struct msm_dsi *msm_dsi)
{
	MSM_FUNC_ENTER("[DSI] snapshot msm_dsi=%p", msm_dsi);
	msm_dsi_host_snapshot(disp_state, msm_dsi->host);
	msm_dsi_phy_snapshot(disp_state, msm_dsi->phy);
	MSM_FUNC_EXIT("[DSI] snapshot");
}

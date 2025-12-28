# SPDX-License-Identifier: GPL-2.0

ccflags-y := -I $(srctree)/$(src)
ccflags-$(CONFIG_DRM_MSM_DSI) += -I $(srctree)/$(src)/dsi

apq-y := \
    	apq_drv.o \
	mdp/mdp5_kms.o

#apq-$(CONFIG_DRM_MSM_DSI) += dsi/dsi.o \
#			dsi/dsi_cfg.o \
#			dsi/dsi_host.o \
#			dsi/dsi_manager.o \
#			dsi/phy/dsi_phy.o \
#
#apq-$(CONFIG_DRM_MSM_DSI_28NM_PHY) += dsi/phy/dsi_phy_28nm.o

obj-$(CONFIG_DRM_APQ) += apq.o

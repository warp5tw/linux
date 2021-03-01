# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the video capture/playback device drivers.
#

# Place here, alphabetically sorted by directory
# (e. g. LC_ALL=C sort Makefile)
obj-y += allegro-dvt/
obj-y += amlogic/
obj-y += amphion/
obj-y += aspeed/
obj-y += atmel/
obj-y += cadence/
obj-y += chips-media/
obj-y += intel/
obj-y += marvell/
obj-y += mediatek/
obj-y += nvidia/
obj-y += nxp/
obj-y += qcom/
obj-y += renesas/
obj-y += rockchip/
obj-y += samsung/
obj-y += st/
obj-y += sunxi/
obj-y += ti/
obj-y += via/
obj-y += xilinx/

# Please place here only ancillary drivers that aren't SoC-specific
# Please keep it alphabetically sorted by Kconfig name
# (e. g. LC_ALL=C sort Makefile)
obj-$(CONFIG_VIDEO_MEM2MEM_DEINTERLACE)	+= m2m-deinterlace.o
obj-$(CONFIG_VIDEO_MUX)			+= video-mux.o

obj-$(CONFIG_VIDEO_S3C_CAMIF)		+= s3c-camif/
obj-$(CONFIG_VIDEO_SAMSUNG_EXYNOS4_IS)	+= exynos4-is/
obj-$(CONFIG_VIDEO_SAMSUNG_S5P_JPEG)	+= s5p-jpeg/
obj-$(CONFIG_VIDEO_SAMSUNG_S5P_MFC)	+= s5p-mfc/

obj-$(CONFIG_VIDEO_SAMSUNG_S5P_G2D)	+= s5p-g2d/
obj-$(CONFIG_VIDEO_SAMSUNG_EXYNOS_GSC)	+= exynos-gsc/

obj-$(CONFIG_VIDEO_STI_BDISP)		+= sti/bdisp/
obj-$(CONFIG_VIDEO_STI_HVA)		+= sti/hva/
obj-$(CONFIG_DVB_C8SECTPFE)		+= sti/c8sectpfe/

obj-$(CONFIG_VIDEO_STI_DELTA)		+= sti/delta/

obj-y					+= stm32/

obj-y					+= davinci/

obj-$(CONFIG_VIDEO_SH_VOU)		+= sh_vou.o

obj-$(CONFIG_VIDEO_RCAR_DRIF)		+= rcar_drif.o
obj-$(CONFIG_VIDEO_RENESAS_CEU)		+= renesas-ceu.o
obj-$(CONFIG_VIDEO_RENESAS_FCP)		+= rcar-fcp.o
obj-$(CONFIG_VIDEO_RENESAS_FDP1)	+= rcar_fdp1.o
obj-$(CONFIG_VIDEO_RENESAS_JPU)		+= rcar_jpu.o
obj-$(CONFIG_VIDEO_RENESAS_VSP1)	+= vsp1/

obj-$(CONFIG_VIDEO_ROCKCHIP_ISP1)	+= rockchip/rkisp1/
obj-$(CONFIG_VIDEO_ROCKCHIP_RGA)	+= rockchip/rga/

obj-y	+= omap/

obj-$(CONFIG_VIDEO_AM437X_VPFE)		+= am437x/

obj-$(CONFIG_VIDEO_XILINX)		+= xilinx/

obj-$(CONFIG_VIDEO_RCAR_ISP)		+= rcar-isp.o
obj-$(CONFIG_VIDEO_RCAR_VIN)		+= rcar-vin/

obj-$(CONFIG_VIDEO_ATMEL_ISC)		+= atmel/
obj-$(CONFIG_VIDEO_ATMEL_ISI)		+= atmel/
obj-$(CONFIG_VIDEO_ATMEL_XISC)		+= atmel/

obj-$(CONFIG_VIDEO_STM32_DCMI)		+= stm32/
obj-$(CONFIG_VIDEO_STM32_DMA2D)		+= stm32/

obj-$(CONFIG_VIDEO_MEDIATEK_VPU)	+= mtk-vpu/

obj-$(CONFIG_VIDEO_MEDIATEK_VCODEC)	+= mtk-vcodec/

obj-$(CONFIG_VIDEO_MEDIATEK_MDP)	+= mtk-mdp/

obj-$(CONFIG_VIDEO_MEDIATEK_JPEG)	+= mtk-jpeg/

obj-$(CONFIG_VIDEO_QCOM_CAMSS)		+= qcom/camss/

obj-$(CONFIG_VIDEO_QCOM_VENUS)		+= qcom/venus/

obj-y					+= sunxi/

obj-$(CONFIG_VIDEO_MESON_GE2D)		+= meson/ge2d/

obj-$(CONFIG_VIDEO_NUVOTON)    		+= nuvoton/

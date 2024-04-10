# SPDX-License-Identifier: GPL-2.0
dtbo-$(CONFIG_BOARD_VICHARAK_VAAMAN) += \
	rk3399-vaaman-cdn-dp.dtbo \
	rk3399-vaaman-camera-imx219.dtbo \
	rk3399-vaaman-camera-ov5647.dtbo \
	rk3399-vaaman-dsi-waveshare.dtbo \
	rk3399-vaaman-dwc3-0-host.dtbo \
	rk3399-vaaman-dwc3-0-otg.dtbo \
	rk3399-vaaman-fpga-communication.dtbo \
	rk3399-vaaman-i2c2.dtbo \
	rk3399-vaaman-i2c6.dtbo \
	rk3399-vaaman-pwm0.dtbo \
	rk3399-vaaman-pwm1.dtbo \
	rk3399-vaaman-spdif-pin15.dtbo \
	rk3399-vaaman-spdif-pin32.dtbo \
	rk3399-vaaman-spi2-jedec-flash.dtbo

dtbo-$(CONFIG_BOARD_VICHARAK_AXON) +=

dtbotxt-$(CONFIG_ARCH_ROCKCHIP) += \
	README.rockchip-overlays

targets += $(dtbo-y) $(dtbotxt-y)

always-y		:= $(dtbo-y) $(dtbotxt-y)
clean-files		:= *.dtbo.disabled

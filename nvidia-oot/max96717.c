// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Copyright (c) 2026 RealSense, Inc. All rights reserved.
/*
 * max96717.c - max96717 GMSL2 Serializer driver
 *
 * Dual-mode driver: supports both Pixel Mode (validated MAX96717 + MAX96712
 * EVB bring-up) and Tunnel Mode (D5xx / MAX96724). The mode is selected per
 * device via the "adi,pixel-mode" device-tree property:
 *     present  -> Pixel Mode
 *     absent   -> Tunnel Mode (default)
 *
 * Address reassignment is performed on the deserializer (not here), so this
 * driver does not touch DEV_ADDR / prim-ser state.
 */

#include <nvidia/conftest.h>

#include <media/camera_common.h>
#include <linux/module.h>
#include <linux/of.h>
#include <media/max96717.h>

/* ===== register specifics (shared + pixel mode) ===== */
#define MAX96717_REG1_ADDR 0x1		/* GMSL2 link rate / CC routing */
#define MAX96717_REG2_ADDR 0x2		/* a.k.a. PIPE_EN: soft-reset / video init */
#define MAX96717_CTRL0_ADDR 0x10
#define MAX96717_TX1_ADDR 0x29
#define MAX96717_EXT11_ADDR 0x383
#define MAX96717_FRONTTOP_10_ADDR 0x312
#define MAX96717_VIDEO_TX0_ADDR 0x110
#define MAX96717_VIDEO_TX1_ADDR 0x111
#define MAX96717_MIPI_RX0_ADDR 0x330
#define MAX96717_MIPI_RX1_ADDR 0x331
#define MAX96717_MIPI_RX2_ADDR 0x332
#define MAX96717_MIPI_RX3_ADDR 0x333
#define MAX96717_MIPI_RX8_ADDR 0x338

#define MAX96717_I2C4_ADDR 0x44
#define MAX96717_I2C5_ADDR 0x45

/* ===== register specifics (tunnel mode) ===== */
/* REG2 / PIPE_EN values (shared addr 0x02) */
#define MAX96717_SOFT_RESET		0x03	/* PIPE_EN soft reset */
#define MAX96717_PIPE_EN_ALL		0x43	/* pipes enabled + video init */
/* REG1: link rate. IIC_2_EN=1, TX_RATE=6Gbps, RX_RATE=187.5Mbps */
#define MAX96717_LINK_SPEED_6GBPS	0x88
/* CTRL0: GMSL2 mode + TX enable, per link */
#define MAX96717_CTRL0_LINK_A		0x21
#define MAX96717_CTRL0_LINK_B		0x22
/* EXT11 tunnel-mode enable (bit[7]) */
#define MAX96717_TUN_MODE_EN		0x80
/* VIDEO_TX0/TX1 tunnel-mode data path */
#define MAX96717_VIDEO_TX0_TUN		0xE9
#define MAX96717_VIDEO_TX1_TUN		0x10
/* MIPI_RX1: 4-lane + input deskew (bit[6]=deskew, bits[5:4]=11) */
#define MAX96717_CSI_4_LANES		0x70
/* Lane mapping for 1x4 mode */
#define MAX96717_CSI_1X4_LANE_MAP1	0xE0
#define MAX96717_CSI_1X4_LANE_MAP2	0x04
/* MIPI_RX0: CSI reset on / normal operation */
#define MAX96717_MIPI_RX0_RESET		0x08
#define MAX96717_MIPI_RX0_NORMAL	0x00

/*
 * MFP7/MFP8 high-impedance release.
 *
 * At power-up GPIO8 comes up with the output driver enabled, so the
 * serializer actively drives the camera-side M2_I2C line and wedges the bus:
 * any IMU (BMI088) I2C access by the camera FW times out whenever the
 * serializer is powered. Confirmed on HW - tri-stating GPIO8 restored IMU
 * I2C reads. Release MFP7/MFP8 (output driver off, no link TX/RX, no pull).
 *
 * GPIO_n registers are a 3-byte group (A/B/C) at 0x02BE + 3*n.
 */
#define MAX96717_GPIO7_A_ADDR		0x02D3	/* MFP7 / pass-through SDA1 */
#define MAX96717_GPIO7_B_ADDR		0x02D4
#define MAX96717_GPIO8_A_ADDR		0x02D6	/* MFP8 / pass-through SCL1 */
#define MAX96717_GPIO8_B_ADDR		0x02D7
#define MAX96717_GPIO_A_HIGH_Z		0x01	/* GPIO_OUT_DIS=1, TX/RX_EN=0 */
#define MAX96717_GPIO_B_NO_PULL		0x00

#define MAX96717_MAX_PIPES		4

/*
 * GPIO-over-GMSL tunneling for external frame sync (FSYNC/ESYNC).
 *
 * The MAX96717 GPIO_n registers form a 3-byte group (A/B/C) at 0x02BE + 3*n:
 *   GPIO_n_A: GPIO_OUT_DIS / GPIO_TX_EN / GPIO_RX_EN / TX_COMP_EN
 *   GPIO_n_B: GPIO_TX_ID[4:0]  - GMSL channel this pin transmits on
 *   GPIO_n_C: GPIO_RX_ID[4:0]  - GMSL channel this pin receives from
 *
 * Schematic signal mapping on the D585/D580 board:
 *   H_VSYNC_TRIG     -> MFP0  (PIN 2) = GPIO0  (DES->SER, host sync trigger)
 *   RGB_FSYNC        -> CFG0  (PIN 3) = GPIO1  (DES->SER, RGB frame sync;
 *                          CFG0 is output-only after boot, so GPIO1 must be
 *                          RX-from-link driving the pin, not input)
 *   H_STROBE_OUT_1V8 -> MFP6  (PIN22) = GPIO6  (SER->DES, camera strobe/EOF
 *                          feedback for host-side frame timing)
 *
 * GMSL2 GPIO channel assignment (mirrors validated MAX9295 ESYNC scheme):
 *   ch23: Host->Camera  sync edge (GPIO0 + GPIO1 both RX from ch23)
 *   ch31: Camera->Host  strobe/EOF (GPIO6 TX on ch31)
 */
#define MAX96717_GPIO0_A_ADDR		0x02BE
#define MAX96717_GPIO0_B_ADDR		0x02BF
#define MAX96717_GPIO0_C_ADDR		0x02C0
#define MAX96717_GPIO1_A_ADDR		0x02C1
#define MAX96717_GPIO1_B_ADDR		0x02C2
#define MAX96717_GPIO1_C_ADDR		0x02C3
#define MAX96717_GPIO6_A_ADDR		0x02D0
#define MAX96717_GPIO6_B_ADDR		0x02D1
#define MAX96717_GPIO6_C_ADDR		0x02D2

/* Enable: GPIO0 (H_VSYNC_TRIG) - RX from ch23, drives camera pin. */
#define MAX96717_GPIO0_A_ESYNC		0x24	/* RX_EN=1, OUT_DIS=0, TX_COMP_EN=1 */
#define MAX96717_GPIO0_B_ESYNC		0x77	/* TX_ID=23, push-pull, pull-up */
#define MAX96717_GPIO0_C_ESYNC		0x57	/* RX_ID=23 */
/* Enable: GPIO1 (RGB_FSYNC) - RX from ch23, drives camera pin (CFG0=output only). */
#define MAX96717_GPIO1_A_ESYNC		0x05	/* OUT_DIS=1*, RX_EN=1, TX_EN=0 */
#define MAX96717_GPIO1_B_ESYNC		0x1F	/* TX_ID=31 (unused, TX_EN=0) */
#define MAX96717_GPIO1_C_ESYNC		0x57	/* RX_ID=23 */
/* Enable: GPIO6 (H_STROBE_OUT_1V8) - TX on ch31, reads camera strobe pin. */
#define MAX96717_GPIO6_A_ESYNC		0x27	/* TX_EN=1, RX_EN=0, OUT_DIS=1, TX_COMP_EN=1 */
#define MAX96717_GPIO6_B_ESYNC		0x1F	/* TX_ID=31, push-pull, no pull */
#define MAX96717_GPIO6_C_ESYNC		0x00	/* RX_ID=0 (unused, RX_EN=0) */

/* Disable: restore power-up reset values (datasheet GPIO_n reset column). */
#define MAX96717_GPIO0_A_RESET		0x99
#define MAX96717_GPIO0_B_RESET		0xA0
#define MAX96717_GPIO0_C_RESET		0x40
#define MAX96717_GPIO1_A_RESET		0x81
#define MAX96717_GPIO1_B_RESET		0x21
#define MAX96717_GPIO1_C_RESET		0x41
#define MAX96717_GPIO6_A_RESET		0x99
#define MAX96717_GPIO6_B_RESET		0xA6
#define MAX96717_GPIO6_C_RESET		0x46

struct max96717_client_ctx {
	struct gmsl_link_ctx *g_ctx;
	bool st_done;
};

struct max96717 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	struct max96717_client_ctx g_client;
	struct mutex lock;
	bool pixel_mode;
};

struct reg_pair {
	u16 addr;
	u8 val;
};

static int max96717_write_reg(struct device *dev, u16 addr, u8 val)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		dev_err(dev, "%s:i2c write failed, 0x%x = %x\n",
			__func__, addr, val);

	/* delay before next i2c command as required for SERDES link */
	usleep_range(100, 110);

	return err;
}

static int max96717_set_registers(struct device *dev, struct reg_pair *map,
				 u32 count)
{
	int err = 0;
	u32 j = 0;

	for (j = 0; j < count; j++) {
		err = max96717_write_reg(dev,
			map[j].addr, map[j].val);
		if (err != 0)
			break;
	}

	return err;
}

int max96717_setup_control(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;
	struct gmsl_link_ctx *g_ctx;

	mutex_lock(&priv->lock);

	if (!priv->g_client.g_ctx) {
		dev_err(dev, "%s: no sensor dev client found\n", __func__);
		err = -EINVAL;
		goto error;
	}

	g_ctx = priv->g_client.g_ctx;

	/*
	 * I2C address translation for sensor passthrough.
	 * Address *reassignment* is handled by the deserializer, so here we
	 * only program the SER's I2C4/I2C5 (SRC_B/DST_B) translation pair.
	 */
	max96717_write_reg(dev, MAX96717_I2C4_ADDR, (g_ctx->sdev_reg << 1));
	max96717_write_reg(dev, MAX96717_I2C5_ADDR, (g_ctx->sdev_def << 1));

	g_ctx->serdev_found = true;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_setup_control);

/*
 * max96717_enable_gpio_tunneling - enable sync/strobe GPIO tunneling
 *
 * Configures three GPIOs for external frame sync:
 *   GPIO0 (H_VSYNC_TRIG):     RX from ch23, drives camera MFP0 pin
 *   GPIO1 (RGB_FSYNC):        RX from ch23, drives camera CFG0/MFP1 pin
 *   GPIO6 (H_STROBE_OUT_1V8): TX on ch31, reads camera strobe for host timing
 *
 * Register contents mirror the validated MAX9295 ESYNC configuration. The
 * d4xx ser_interface uses the name enable_gpio_tunneling()/disable...().
 */
int max96717_enable_gpio_tunneling(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	struct reg_pair map[] = {
		{MAX96717_GPIO0_A_ADDR, MAX96717_GPIO0_A_ESYNC},
		{MAX96717_GPIO0_B_ADDR, MAX96717_GPIO0_B_ESYNC},
		{MAX96717_GPIO0_C_ADDR, MAX96717_GPIO0_C_ESYNC},
		{MAX96717_GPIO1_A_ADDR, MAX96717_GPIO1_A_ESYNC},
		{MAX96717_GPIO1_B_ADDR, MAX96717_GPIO1_B_ESYNC},
		{MAX96717_GPIO1_C_ADDR, MAX96717_GPIO1_C_ESYNC},
		{MAX96717_GPIO6_A_ADDR, MAX96717_GPIO6_A_ESYNC},
		{MAX96717_GPIO6_B_ADDR, MAX96717_GPIO6_B_ESYNC},
		{MAX96717_GPIO6_C_ADDR, MAX96717_GPIO6_C_ESYNC},
	};
	int err;

	mutex_lock(&priv->lock);

	err = max96717_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: GPIO tunneling enable failed (%d)\n",
			__func__, err);
	else
		dev_info(dev, "%s: GPIO tunneling enabled (sync ch23, strobe ch31)\n",
			 __func__);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_enable_gpio_tunneling);

/*
 * max96717_disable_gpio_tunneling - disable sync/strobe GPIO tunneling
 *
 * Restores GPIO0/GPIO1/GPIO6 to their power-up reset state so the
 * serializer no longer tunnels frame-sync or strobe transitions.
 */
int max96717_disable_gpio_tunneling(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	struct reg_pair map[] = {
		{MAX96717_GPIO0_A_ADDR, MAX96717_GPIO0_A_RESET},
		{MAX96717_GPIO0_B_ADDR, MAX96717_GPIO0_B_RESET},
		{MAX96717_GPIO0_C_ADDR, MAX96717_GPIO0_C_RESET},
		{MAX96717_GPIO1_A_ADDR, MAX96717_GPIO1_A_RESET},
		{MAX96717_GPIO1_B_ADDR, MAX96717_GPIO1_B_RESET},
		{MAX96717_GPIO1_C_ADDR, MAX96717_GPIO1_C_RESET},
		{MAX96717_GPIO6_A_ADDR, MAX96717_GPIO6_A_RESET},
		{MAX96717_GPIO6_B_ADDR, MAX96717_GPIO6_B_RESET},
		{MAX96717_GPIO6_C_ADDR, MAX96717_GPIO6_C_RESET},
	};
	int err;

	mutex_lock(&priv->lock);

	err = max96717_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: GPIO tunneling disable failed (%d)\n",
			__func__, err);
	else
		dev_dbg(dev, "%s: GPIO tunneling disabled (pins restored)\n",
			__func__);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_disable_gpio_tunneling);

int max96717_reset_control(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);
	if (!priv->g_client.g_ctx) {
		dev_err(dev, "%s: no sdev client found\n", __func__);
		err = -EINVAL;
		goto error;
	}

	priv->g_client.st_done = false;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_reset_control);

int max96717_sdev_pair(struct device *dev, struct gmsl_link_ctx *g_ctx)
{
	struct max96717 *priv;
	int err = 0;

	if (!dev || !g_ctx || !g_ctx->s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);
	mutex_lock(&priv->lock);
	if (priv->g_client.g_ctx) {
		dev_err(dev, "%s: device already paired\n", __func__);
		err = -EINVAL;
		goto error;
	}

	priv->g_client.st_done = false;

	priv->g_client.g_ctx = g_ctx;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_sdev_pair);

int max96717_sdev_unpair(struct device *dev, struct device *s_dev)
{
	struct max96717 *priv = NULL;
	int err = 0;

	if (!dev || !s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	if (!priv->g_client.g_ctx) {
		dev_err(dev, "%s: device is not paired\n", __func__);
		err = -ENOMEM;
		goto error;
	}

	if (priv->g_client.g_ctx->s_dev != s_dev) {
		dev_err(dev, "%s: invalid device\n", __func__);
		err = -EINVAL;
		goto error;
	}

	priv->g_client.g_ctx = NULL;
	priv->g_client.st_done = false;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_sdev_unpair);

static  struct regmap_config max96717_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

/*
 * max96717_init_settings - one-shot serializer bring-up for the selected mode.
 *
 * Both modes do all of their register programming here; set_pipe() is a no-op
 * (see below). Pixel mode uses the validated MAX96717 + MAX96712 EVB sequence;
 * tunnel mode consolidates the full GMSL2 tunnel bring-up (link rate, CTRL0,
 * MFP7/8 release, CSI-2 lanes, EXT11 tunnel enable, video TX) in one place.
 */
int max96717_init_settings(struct device *dev)
{
	int err = 0;
	struct max96717 *priv = dev_get_drvdata(dev);

	/* ---------- Pixel mode (validated) ---------- */
	/*
	 * Serializer (MAX96717 @ 0x40) bring-up, ported verbatim from the
	 * validated GMSL config XML (MAX96717 + MAX96712, 4-lane 700Mbps/lane,
	 * pixel mode). The XML repeats this exact sequence during its AIO
	 * re-bring-up phase, so it is listed once here.
	 */
	struct reg_pair ser_cfg_pre[] = {
		{MAX96717_REG2_ADDR, 0x03}, /* 0x2 - REG2: VID_TX_EN=0, INIT=1 */
		{MAX96717_CTRL0_ADDR, 0x21}, /* 0x10 - CTRL0: RESET_ONESHOT, GMSL2 link A */
	};
	struct reg_pair ser_cfg_mid[] = {
		{MAX96717_TX1_ADDR, 0x00}, /* 0x29 - FEC OFF */
		{MAX96717_EXT11_ADDR, 0x00}, /* 0x383 - Pixel mode */
		{MAX96717_VIDEO_TX0_ADDR, 0x60}, /* 0x110 - VIDEO_TX0 AUTO_BPP=0 ENC_MODE=10 */
		{MAX96717_VIDEO_TX1_ADDR, 0x10}, /* 0x111 - VIDEO_TX1 BPP=16 forced (matches DT) */
		{MAX96717_FRONTTOP_10_ADDR, 0x4}, /* 0x312 - Fronttop_10 double 8bit */
		{MAX96717_MIPI_RX1_ADDR, 0x30}, /* 0x331 - MIPI_RX1: 4-lane */
		{MAX96717_MIPI_RX0_ADDR, 0x48}, /* 0x330 - MIPI_RX0 reset ON + non-cont-clk */
	};
	struct reg_pair ser_cfg_post[] = {
		{MAX96717_MIPI_RX0_ADDR, 0x40}, /* 0x330 - MIPI_RX0 reset OFF, non-cont-clk enabled */
		{MAX96717_MIPI_RX8_ADDR, 0x22}, /* 0x338 - MIPI_RX8 settle=0x22 (t_hs[5:4]/t_clk[1:0]) */
		{MAX96717_REG2_ADDR, 0x43}, /* 0x2 - REG2: VID_TX_EN */
	};

	/* ---------- Tunnel mode ---------- */
	/*
	 * Release MFP7/MFP8 first so the serializer stops driving the
	 * camera-side M2_I2C bus shared with the BMI088 IMU.
	 */
	struct reg_pair tun_gpio_release[] = {
		{MAX96717_GPIO7_A_ADDR, MAX96717_GPIO_A_HIGH_Z},
		{MAX96717_GPIO7_B_ADDR, MAX96717_GPIO_B_NO_PULL},
		{MAX96717_GPIO8_A_ADDR, MAX96717_GPIO_A_HIGH_Z},
		{MAX96717_GPIO8_B_ADDR, MAX96717_GPIO_B_NO_PULL},
	};
	/*
	 * GMSL2 link bring-up + CSI-2 input + tunnel enable + video TX.
	 * Consolidated from the branch's setup_control()/setup_streaming()/
	 * init_settings() (minus SER address reassignment, which is done on
	 * the deserializer). CTRL0 link select is filled in below from g_ctx.
	 */
	struct reg_pair tun_cfg[] = {
		{MAX96717_REG1_ADDR, MAX96717_LINK_SPEED_6GBPS}, /* 0x01 - 6Gbps */
		{MAX96717_CTRL0_ADDR, MAX96717_CTRL0_LINK_A}, /* 0x10 - GMSL2 link (patched) */
		{MAX96717_REG2_ADDR, MAX96717_SOFT_RESET}, /* 0x02 - PIPE_EN soft reset */
		{MAX96717_EXT11_ADDR, MAX96717_TUN_MODE_EN}, /* 0x383 - tunnel mode ON */
		{MAX96717_MIPI_RX1_ADDR, MAX96717_CSI_4_LANES}, /* 0x331 - 4 lanes + deskew */
		{MAX96717_MIPI_RX2_ADDR, MAX96717_CSI_1X4_LANE_MAP1}, /* 0x332 - lane map */
		{MAX96717_MIPI_RX3_ADDR, MAX96717_CSI_1X4_LANE_MAP2}, /* 0x333 - lane map */
		{MAX96717_VIDEO_TX0_ADDR, MAX96717_VIDEO_TX0_TUN}, /* 0x110 - tunnel data path */
		{MAX96717_VIDEO_TX1_ADDR, MAX96717_VIDEO_TX1_TUN}, /* 0x111 - tunnel data path */
	};

	mutex_lock(&priv->lock);

	if (priv->pixel_mode) {
		err |= max96717_set_registers(dev, ser_cfg_pre,
					     ARRAY_SIZE(ser_cfg_pre));
		msleep(100);
		err |= max96717_set_registers(dev, ser_cfg_mid,
					     ARRAY_SIZE(ser_cfg_mid));
		/* XML waits 2ms between MIPI_RX0 reset assert and release */
		usleep_range(2000, 2100);
		err |= max96717_set_registers(dev, ser_cfg_post,
					     ARRAY_SIZE(ser_cfg_post));
	} else {
		struct gmsl_link_ctx *g_ctx = priv->g_client.g_ctx;

		if (g_ctx && g_ctx->serdes_csi_link == GMSL_SERDES_CSI_LINK_B)
			tun_cfg[1].val = MAX96717_CTRL0_LINK_B;

		/* Free the camera-side M2_I2C (IMU) bus before anything else. */
		err |= max96717_set_registers(dev, tun_gpio_release,
					     ARRAY_SIZE(tun_gpio_release));
		/* Settle after CTRL0/I2C translation before PIPE_EN/FRONTTOP. */
		usleep_range(5000, 6000);

		err |= max96717_set_registers(dev, tun_cfg, ARRAY_SIZE(tun_cfg));
		/* 2ms between MIPI_RX0 reset assert and release */
		max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR,
				   MAX96717_MIPI_RX0_RESET);
		usleep_range(2000, 2100);
		max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR,
				   MAX96717_MIPI_RX0_NORMAL);
		/* pipes on + video init */
		max96717_write_reg(dev, MAX96717_REG2_ADDR,
				   MAX96717_PIPE_EN_ALL);
	}

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_init_settings);

int max96717_set_pipe(struct device *dev, int pipe_id,
		     u8 data_type1, u8 data_type2, u32 vc_id)
{
	/*
	 * No runtime per-pipe config needed: pixel mode programs everything in
	 * init_settings(); tunnel mode tunnels all VCs transparently (the
	 * data_type-dependent writes are kept commented in init_settings()).
	 */
	return 0;
}
EXPORT_SYMBOL(max96717_set_pipe);

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int max96717_probe(struct i2c_client *client)
#else
static int max96717_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
#endif
{
	struct max96717 *priv;
	int err = 0;

	dev_info(&client->dev, "[MAX96717]: probing GMSL Serializer\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(priv->i2c_client,
				&max96717_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	mutex_init(&priv->lock);

	/* Mode select: "adi,pixel-mode" present -> pixel; absent -> tunnel. */
	priv->pixel_mode = of_property_read_bool(client->dev.of_node,
						 "adi,pixel-mode");
	dev_info(&client->dev, "[MAX96717]: %s mode\n",
		 priv->pixel_mode ? "pixel" : "tunnel");

	dev_set_drvdata(&client->dev, priv);

	/* dev communication gets validated when GMSL link setup is done */
	dev_info(&client->dev, "%s: success\n", __func__);

	return err;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int max96717_remove(struct i2c_client *client)
#else
static void max96717_remove(struct i2c_client *client)
#endif
{
	struct max96717 *priv;

	if (client != NULL) {
		priv = dev_get_drvdata(&client->dev);
		mutex_destroy(&priv->lock);
		i2c_unregister_device(client);
		client = NULL;
	}
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
	return 0;
#endif
}

static const struct i2c_device_id max96717_id[] = {
	{ "max96717", 0 },
	{ },
};

static const struct of_device_id max96717_of_match[] = {
	{ .compatible = "adi,max96717", },
	{ .compatible = "maxim,max96717", },
	{ },
};
MODULE_DEVICE_TABLE(of, max96717_of_match);
MODULE_DEVICE_TABLE(i2c, max96717_id);

static struct i2c_driver max96717_i2c_driver = {
	.driver = {
		.name = "max96717",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(max96717_of_match),
	},
	.probe = max96717_probe,
	.remove = max96717_remove,
	.id_table = max96717_id,
};

module_i2c_driver(max96717_i2c_driver);

MODULE_DESCRIPTION("GMSL2 Serializer driver max96717 (Pixel + Tunnel Mode)");
MODULE_AUTHOR("Sudhir Vyas <svyas@nvidia.com>");
MODULE_AUTHOR("RealSense AI");
MODULE_LICENSE("GPL v2");

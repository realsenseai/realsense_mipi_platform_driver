/*
 * max96717.c - MAX96717 GMSL2 Serializer driver (Tunnel Mode)
 *
 * Copyright (c) 2026, RealSense, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <media/camera_common.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/version.h>
#include <media/max96717.h>

/* ===== Register Addresses ===== */

/* REG0: Device ID (read-only, returns 0x40) */
#define MAX96717_DEV_ADDR		0x00

/* Errata #5: force the negative GMSL output on for 6Gbps coax links. */
#define MAX96717_RLMSCE_ADDR		0x14CE
#define MAX96717_ENMINUS_FORCE_ON	(BIT(4) | BIT(3))

/*
 *   REG1: GMSL2 link rate, CC pin routing, pass-through I2C enables
 *   TX_RATE[3:2]: 01=3Gbps, 10=6Gbps
 *   RX_RATE[1:0]: 00=187.5Mbps reverse
 */
#define MAX96717_REG1_ADDR		0x01

/*
 *   PIPE_EN: Pipe enable / soft reset
 *   0x03 = soft reset, 0x43 = pipes enabled + video init
 *   Matches MAX9295 PIPE_EN at 0x02
 */
#define MAX96717_PIPE_EN_ADDR		0x02

/*
 *   CTRL0: GMSL2 TX Control
 *   bit[5]=1: GMSL2 mode, bit[0]=1: enable transmitter
 *   0x21 = GMSL2 mode + TX enable on Link A
 */
#define MAX96717_CTRL0_ADDR		0x10

/*
 *   CTRL3: Start Video - final enable bit
 *   bit[0]=1: activate video stream
 */
#define MAX96717_CTRL3_ADDR		0x13

/*
 *   TX1: FEC Enable
 *   bit[6]=1: Reed-Solomon FEC enabled
 */
#define MAX96717_TX1_ADDR		0x29

/*
 *   MAX96717 I2C address translation registers (GMSL2 standard):
 *   SRC_A / DST_A (0x42/0x43): Translation pair A (SER broadcast addr)
 *   SRC_B / DST_B (0x44/0x45): Translation pair B (sensor passthrough)
 *
 *   When host writes to SRC_B address, the MAX96717 translates it to
 *   DST_B and forwards via GMSL back-channel to the remote sensor.
 *
 */
#define MAX96717_SRC_A_ADDR		0x42
#define MAX96717_DST_A_ADDR		0x43
#define MAX96717_I2C4_ADDR		0x44
#define MAX96717_I2C5_ADDR		0x45

/*
 *   TX_STR_SEL: Stream ID select
 *   bits[1:0]: stream ID for DES pipe routing
 */
#define MAX96717_TX_STR_SEL_ADDR	0x5B

/*
 *   VIDEO_TX0/TX1: Video TX path config
 *   VIDEO_TX0[2] bypasses PCLK detection.  Tunnel mode carries CSI-2
 *   packets directly and must not gate sparse 5/15 FPS traffic on the
 *   recovered pixel-clock detector.
 */
#define MAX96717_VIDEO_TX0_ADDR		0x110
#define MAX96717_VIDEO_TX1_ADDR		0x111

/*
 *   FRONTTOP_0: Front-end topology
 *   0x12 = single CSI-2 port A, all VCs pass through
 */
#define MAX96717_FRONTTOP0_ADDR		0x308

/*
 *   MIPI_RX0-RX3: CSI-2 receiver configuration
 *   RX0 (0x330): CSI mode (0x00 = D-PHY controller A)
 *   RX1 (0x331): bit[6] deskew + ctrl1_num_lanes bits[5:4]
 *                0x70 = deskew enabled + bits[5:4]=11 (4 lanes)
 *   RX2 (0x332): Lane mapping - 0xE0 = default 1x4 map
 *   RX3 (0x333): Lane mapping - 0x04 = default 1x4 map
 */
#define MAX96717_MIPI_RX0_ADDR		0x330
#define MAX96717_MIPI_RX1_ADDR		0x331
#define MAX96717_MIPI_RX2_ADDR		0x332
#define MAX96717_MIPI_RX3_ADDR		0x333

/*
 *   EXT11: TUNNEL MODE ENABLE
 *   bit[7]=1: Tun_Mode = ON
 *   0x383 = 0x80
 */
#define MAX96717_EXT11_ADDR		0x383

/* PHY calibration stage-1 value */
#define MAX96717_PHY_CAL_STAGE1		0x80
/* PHY calibration stage-2 value */
#define MAX96717_PHY_CAL_STAGE2		0x90
/* CSI input timing / frequency config */
#define MAX96717_CSI_TIMING_3F0		0x59
#define MAX96717_CSI_TIMING_3F1		0x09

/* ===== Register Values ===== */

/* Soft reset value for PIPE_EN register */
#define MAX96717_SOFT_RESET		0x03

/* Final enable: pipes enabled + video init */
#define MAX96717_PIPE_EN_ALL		0x43

/* CTRL0: GMSL2 mode + TX enable (Link A) */
#define MAX96717_CTRL0_LINK_A		0x21
/* CTRL0: GMSL2 mode + TX enable (Link B) */
#define MAX96717_CTRL0_LINK_B		0x22

/* CTRL0 reset all */
#define MAX96717_RESET_ALL		0x80

/* EXT11 tunnel mode enable value */
#define MAX96717_TUN_MODE_EN		0x80
#define MAX96717_TUN_MODE_DIS		0x00

/* VIDEO_TX0 tunnel mode data path enable + CLKDET_BYP (bit 2). */
#define MAX96717_VIDEO_TX0_CLKDET_BYP	BIT(2)
#define MAX96717_VIDEO_TX0_TUN		(0xE9 | MAX96717_VIDEO_TX0_CLKDET_BYP)
/* VIDEO_TX1 tunnel mode config */
#define MAX96717_VIDEO_TX1_TUN		0x10
#define MAX96717_VIDEO_TX0_BPP_MANUAL	0x60
#define MAX96717_VIDEO_TX1_BPP16	0x50
#define MAX96717_VIDEO_TX2_ADDR		0x112
#define MAX96717_VIDEO_TX2_DRIFT_DIS	0x08

/*
 *   MIPI RX1: 4-lane CSI-2 with input deskew enabled.
 *   bit[6]=1 (deskew), bits[5:4]=11 (4 lanes).
 */
#define MAX96717_CSI_4_LANES		0x70
/* 2-lane CSI-2, bits[5:4]=01 */
#define MAX96717_CSI_2_LANES		0x10

/* Lane mapping for 1x4 mode */
#define MAX96717_CSI_1X4_LANE_MAP1	0xE0
#define MAX96717_CSI_1X4_LANE_MAP2	0x04

/* FRONTTOP_0: single port A, all VCs */
#define MAX96717_FRONTTOP0_PORT_A	0x12
/* Pixel mode from Port B into Pipe Z */
#define MAX96717_FRONTTOP0_PIXEL_PORT_B	0x64
#define MAX96717_FRONTTOP5_ADDR		0x30D
#define MAX96717_FRONTTOP9_ADDR		0x311
#define MAX96717_FRONTTOP16_ADDR	0x318
#define MAX96717_FRONTTOP17_ADDR	0x319
#define MAX96717_FRONTTOP9_START_VIDEO	0x40

/* MIPI_RX0: CSI reset on + non-continuous clock */
#define MAX96717_MIPI_RX0_RESET		0x08
/* MIPI_RX0: Normal operation */
#define MAX96717_MIPI_RX0_NORMAL	0x00

/* REG1: TX_RATE[3:2]=01, RX_RATE[1:0]=00 */
#define MAX96717_LINK_SPEED_3GBPS	0x04
/* REG1: main CC enabled, TX_RATE[3:2]=10, RX_RATE[1:0]=00. */
#define MAX96717_LINK_SPEED_6GBPS	0x08

/* EXT11 tunnel mode pre-config (0x0383=0x80) */
#define MAX96717_EXT11_TUN_PRE		0x80

#define MAX96717_MAX_PIPES		4

/*
 * MFP7/MFP8 high-impedance release.
 *
 * At power-up GPIO8 comes up with GPIO_OUT_DIS=0 (output driver enabled,
 * GPIO8_A=0x9c observed live), so the serializer actively drives the M2_I2C
 * line and wedges the bus: any IMU I2C access by the camera FW times out
 * whenever the serializer is powered.  This was confirmed on hardware -
 * tri-stating GPIO8 alone immediately restored IMU I2C reads.
 *
 * Release MFP7/MFP8 (output driver off, no link TX/RX, no pull) so only
 * the M2_I2C bus
 * masters own the line.
 *
 * GPIO_n registers are a 3-byte group (A/B/C) starting at 0x02BE:
 *   GPIO_n_A = 0x02BE + 3*n
 */
#define MAX96717_GPIO7_A_ADDR		0x02D3	/* MFP7 / pass-through SDA1 */
#define MAX96717_GPIO7_B_ADDR		0x02D4
#define MAX96717_GPIO8_A_ADDR		0x02D6	/* MFP8 / pass-through SCL1 */
#define MAX96717_GPIO8_B_ADDR		0x02D7
/* GPIO_A: GPIO_OUT_DIS=1 (high-Z), GPIO_TX_EN=0, GPIO_RX_EN=0 */
#define MAX96717_GPIO_A_HIGH_Z		0x01
/* GPIO_B: PULL_UPDN_SEL=None, TX_ID=0 */
#define MAX96717_GPIO_B_NO_PULL		0x00

/* ===== Data Structures ===== */

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
	/* primary serializer properties */
	__u32 def_addr;
	__u32 pst2_ref;
};

static struct max96717 *prim_priv__;

struct reg_pair {
	u16 addr;
	u8 val;
};

/* ===== Low-level I2C ===== */

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
	u32 j;

	for (j = 0; j < count; j++) {
		err = max96717_write_reg(dev, map[j].addr, map[j].val);
		if (err)
			break;
	}

	return err;
}

/* ===== Streaming Setup ===== */

/*
 * max96717_setup_streaming - Configure CSI-2 input and enable Tunnel Mode
 *
 * In Tunnel Mode (unlike MAX9295 Pixel Mode), the serializer encapsulates
 * the entire CSI-2 byte stream verbatim into a single GMSL2 tunnel.
 * All VCs (VC0-VC3) are preserved end-to-end.
 */
int max96717_setup_streaming(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;
	struct gmsl_link_ctx *g_ctx;

	mutex_lock(&priv->lock);

	if (!priv->g_client.g_ctx) {
		dev_err(dev, "%s: no sdev client found\n", __func__);
		err = -EINVAL;
		goto error;
	}

	if (priv->g_client.st_done) {
		dev_dbg(dev, "%s: stream setup is already done\n", __func__);
		goto error;
	}

	g_ctx = priv->g_client.g_ctx;

	max96717_write_reg(dev, MAX96717_EXT11_ADDR, MAX96717_TUN_MODE_EN);
	max96717_write_reg(dev, MAX96717_MIPI_RX1_ADDR, MAX96717_CSI_4_LANES);
	max96717_write_reg(dev, MAX96717_MIPI_RX2_ADDR, MAX96717_CSI_1X4_LANE_MAP1);
	max96717_write_reg(dev, MAX96717_MIPI_RX3_ADDR, MAX96717_CSI_1X4_LANE_MAP2);
	max96717_write_reg(dev, MAX96717_VIDEO_TX0_ADDR, MAX96717_VIDEO_TX0_TUN);
	max96717_write_reg(dev, MAX96717_VIDEO_TX1_ADDR, MAX96717_VIDEO_TX1_TUN);

	max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR, MAX96717_MIPI_RX0_RESET);
	usleep_range(2000, 2100);
	max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR, MAX96717_MIPI_RX0_NORMAL);

	/* pipes on + video init */
	max96717_write_reg(dev, MAX96717_PIPE_EN_ADDR, MAX96717_PIPE_EN_ALL);

	priv->g_client.st_done = true;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_setup_streaming);

/* ===== Control Setup ===== */

/*
 * max96717_setup_control - Set up I2C address translation and link control
 *
 * Steps:
 *   1. Reassign serializer I2C address via DEV_ADDR (reg 0x00)
 *   2. Configure GMSL2 link mode via CTRL0 (reg 0x10)
 *   3. Set I2C address translation for sensor passthrough:
 *      I2C4/SRC_B (0x44) <- (sdev_reg << 1): proxy addr visible to host
 *      I2C5/DST_B (0x45) <- (sdev_def << 1): real addr on back-channel
 *
 */
int max96717_setup_control(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;
	struct gmsl_link_ctx *g_ctx;
	u8 ctrl0_val;

	mutex_lock(&priv->lock);

	if (!priv->g_client.g_ctx) {
		dev_err(dev, "%s: no sensor dev client found\n", __func__);
		err = -EINVAL;
		goto error;
	}

	g_ctx = priv->g_client.g_ctx;

	if (prim_priv__) {
		unsigned int default_id = 0;
		unsigned int target_id = 0;
		int default_ret = -EREMOTEIO;
		int target_ret = -EREMOTEIO;
		int retry;

		for (retry = 0; retry < 10; retry++) {
			default_ret = regmap_read(prim_priv__->regmap,
						  MAX96717_DEV_ADDR, &default_id);
			if (!default_ret)
				break;

			target_ret = regmap_read(priv->regmap,
						 MAX96717_DEV_ADDR, &target_id);
			if (!target_ret)
				break;

			msleep(20);
		}

		if (!default_ret) {
			/* Primary address (0x40) still responsive — do reassign */
			err = max96717_write_reg(&prim_priv__->i2c_client->dev,
						 MAX96717_DEV_ADDR,
						 (g_ctx->ser_reg << 1));
			if (err)
				goto error;

			for (retry = 0; retry < 10; retry++) {
				msleep(20);
				target_ret = regmap_read(priv->regmap,
							 MAX96717_DEV_ADDR,
							 &target_id);
				if (!target_ret)
					break;
			}
			if (target_ret) {
				dev_err(dev,
					"%s: SER target address 0x%02x did not ACK after reassignment\n",
					__func__, g_ctx->ser_reg);
				err = target_ret;
				goto error;
			}
		} else if (!target_ret) {
			dev_info(&prim_priv__->i2c_client->dev,
				 "%s: SER already reassigned (target 0x%02x ACK, addr 0x40 NACK)\n",
				 __func__, g_ctx->ser_reg);
		} else {
			dev_err(dev,
				"%s: SER did not ACK at default 0x40 or target 0x%02x\n",
				__func__, g_ctx->ser_reg);
			err = target_ret;
			goto error;
		}
	}

	/*
	 * Enable 6Gbps while keeping main CC on MFP9/MFP10. IIC_2 must
	 * remain disabled because its pass-through pins overlap main CC.
	 */
	err = max96717_write_reg(dev, MAX96717_REG1_ADDR,
				 MAX96717_LINK_SPEED_6GBPS);
	if (err) {
		dev_err(dev, "%s: failed to configure serializer link rate\n",
			__func__);
		goto error;
	}

	err = regmap_update_bits(priv->regmap, MAX96717_RLMSCE_ADDR,
				 MAX96717_ENMINUS_FORCE_ON,
				 MAX96717_ENMINUS_FORCE_ON);
	if (err) {
		dev_err(dev, "%s: failed to force SION on for 6Gbps coax\n",
			__func__);
		goto error;
	}

	if (g_ctx->serdes_csi_link == GMSL_SERDES_CSI_LINK_A)
		ctrl0_val = MAX96717_CTRL0_LINK_A;
	else
		ctrl0_val = MAX96717_CTRL0_LINK_B;

	dev_info(dev, "%s: SER rate=6Gbps/187.5Mbps main_CC(MFP9/10)=on IIC_2=off link=%c CTRL0=0x%02x\n",
		 __func__,
		 (g_ctx->serdes_csi_link == GMSL_SERDES_CSI_LINK_A) ? 'A' : 'B',
		 ctrl0_val);

	/* CTRL0: manual single-link mode + one-shot reset on selected PHY. */
	err = max96717_write_reg(dev, MAX96717_CTRL0_ADDR, ctrl0_val);

	if (err) {
		dev_err(dev, "%s: ERROR: ser device not found\n", __func__);
		goto error;
	}

	/* delay to settle link */
	msleep(100);

	/*
	 * I2C address translation for sensor passthrough.
	 *
	 * I2C4 / SRC_B (0x44): sensor proxy addr visible to Orin host
	 * I2C5 / DST_B (0x45): sensor real addr on GMSL back-channel
	 *
	 * When host sends I2C to sdev_reg (e.g., 0x1a), the MAX96717
	 * translates it to sdev_def (e.g., 0x42) and forwards via GMSL
	 * back-channel to the HKR sensor.
	 */
	max96717_write_reg(dev, MAX96717_I2C4_ADDR,
			   (g_ctx->sdev_reg << 1));
	max96717_write_reg(dev, MAX96717_I2C5_ADDR,
			   (g_ctx->sdev_def << 1));

	/* dev addr pass-through ref */
	if (prim_priv__)
		prim_priv__->pst2_ref++;

	g_ctx->serdev_found = true;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_setup_control);

/* ===== GPIO Tunneling ===== */

/*
 * GPIO-over-GMSL tunneling for external frame sync (FSYNC/ESYNC).
 *
 * The MAX96717 GPIO_n registers form a 3-byte group (A/B/C) at 0x02BE + 3*n:
 *   GPIO_n_A: GPIO_OUT_DIS / GPIO_TX_EN / GPIO_RX_EN / TX_COMP_EN
 *   GPIO_n_B: GPIO_TX_ID[4:0]  - GMSL channel this pin transmits on
 *   GPIO_n_C: GPIO_RX_ID[4:0]  - GMSL channel this pin receives from
 *
 * Schematic signal mapping on the D585/D580 board:
 *   H_VSYNC_TRIG    → MFP0  (PIN 2) = GPIO0  (DES→SER, host sync trigger)
 *   RGB_FSYNC       → CFG0  (PIN 3) = GPIO1  (DES→SER, RGB frame sync; note
 *                          CFG0 is output-only after boot, so GPIO1 must be
 *                          RX-from-link driving the pin, not input)
 *   H_STROBE_OUT_1V8 → MFP6 (PIN22) = GPIO6  (SER→DES, camera strobe/EOF
 *                          feedback for host-side frame timing)
 *
 * GMSL2 GPIO channel assignment (mirrors validated MAX9295 ESYNC scheme):
 *   ch23: Host→Camera  sync edge (GPIO0 + GPIO1 both RX from ch23)
 *   ch31: Camera→Host  strobe/EOF (GPIO6 TX on ch31)
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

/* Enable: GPIO0 (H_VSYNC_TRIG) — RX from ch23, drives camera pin. */
#define MAX96717_GPIO0_A_ESYNC		0x24	/* RX_EN=1, OUT_DIS=0, TX_COMP_EN=1 */
#define MAX96717_GPIO0_B_ESYNC		0x77	/* TX_ID=23, push-pull, pull-up */
#define MAX96717_GPIO0_C_ESYNC		0x57	/* RX_ID=23 */
/* Enable: GPIO1 (RGB_FSYNC) — RX from ch23, drives camera pin (CFG0=output only). */
#define MAX96717_GPIO1_A_ESYNC		0x05	/* OUT_DIS=1*, RX_EN=1, TX_EN=0 */
#define MAX96717_GPIO1_B_ESYNC		0x1F	/* TX_ID=31 (unused, TX_EN=0) */
#define MAX96717_GPIO1_C_ESYNC		0x57	/* RX_ID=23 */
/* Enable: GPIO6 (H_STROBE_OUT_1V8) — TX on ch31, reads camera strobe pin. */
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

/*
 * max96717_setup_gpio_tunneling - enable sync/strobe GPIO tunneling
 *
 * Configures three GPIOs for external frame sync:
 *   GPIO0 (H_VSYNC_TRIG):  RX from ch23, drives camera MFP0 pin
 *   GPIO1 (RGB_FSYNC):     RX from ch23, drives camera CFG0/MFP1 pin
 *   GPIO6 (H_STROBE_OUT_1V8): TX on ch31, reads camera strobe for host timing
 *
 * Used for external sync (FSYNC/trigger).  The register values mirror the
 * validated MAX9295 ESYNC configuration.
 */
int max96717_setup_gpio_tunneling(struct device *dev)
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
EXPORT_SYMBOL(max96717_setup_gpio_tunneling);

/*
 * max96717_disable_gpio_tunneling - disable sync/strobe GPIO tunneling
 *
 * Restores GPIO0/GPIO1/GPIO6 to their power-up reset state so the
 * serializer no longer tunnels frame-sync or strobe transitions.
 * Counterpart to max96717_setup_gpio_tunneling().
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

/* ===== Reset Control ===== */

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

	if (prim_priv__) {
		prim_priv__->pst2_ref--;

		max96717_write_reg(dev, MAX96717_DEV_ADDR,
				   (prim_priv__->def_addr << 1));
		if (prim_priv__->pst2_ref == 0)
			max96717_write_reg(&prim_priv__->i2c_client->dev,
					   MAX96717_CTRL0_ADDR,
					   MAX96717_RESET_ALL);
	}

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_reset_control);

/* ===== Device Pairing ===== */

int max96717_sdev_pair(struct device *dev, struct gmsl_link_ctx *g_ctx)
{
	struct max96717 *priv;
	int err = 0;

	if (!dev || !g_ctx || !g_ctx->s_dev) {
		pr_err("%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);
	mutex_lock(&priv->lock);
	if (priv->g_client.g_ctx) {
		/*
		 * Already paired — tolerate if this is a re-probe of
		 * the same camera after a failed first attempt.
		 * Simply overwrite with the new g_ctx.
		 */
		dev_warn(dev, "%s: re-pairing (previous probe may have failed)\n",
			 __func__);
	}

	priv->g_client.st_done = false;
	priv->g_client.g_ctx = g_ctx;

	/* mutex_unlock in error path, but we have none — inline the unlock */
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_sdev_pair);

int max96717_sdev_unpair(struct device *dev, struct device *s_dev)
{
	struct max96717 *priv = NULL;
	int err = 0;

	if (!dev || !s_dev) {
		pr_err("%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	if (!priv->g_client.g_ctx) {
		dev_dbg(dev, "%s: device is not paired\n", __func__);
		err = 0; /* Not an error — already unpaired */
		goto error;
	}

	if (priv->g_client.g_ctx->s_dev != s_dev) {
		dev_warn(dev, "%s: s_dev mismatch, clearing anyway\n", __func__);
	}

	priv->g_client.g_ctx = NULL;
	priv->g_client.st_done = false;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_sdev_unpair);

/* ===== Pipe Configuration ===== */

/*
 * __max96717_set_pipe - Configure a single pipe's DT/VC mapping
 *
 * In Tunnel Mode, per-pipe DT/VC filtering is bypassed on the serializer。
 * However, we still write the registers for d4xx framework compatibility.
 *
 */
static int __max96717_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
			       u8 data_type2, u32 vc_id)
{
	int err = 0;
	u8 bpp = 0x30;
	struct max96717 *priv = dev_get_drvdata(dev);

	if (priv->pixel_mode) {
		struct reg_pair pixel_pipe_control[] = {
			{MAX96717_FRONTTOP16_ADDR, 0x5E},
			{MAX96717_FRONTTOP17_ADDR, 0x52},
			{MAX96717_FRONTTOP5_ADDR, 0x01},
		};

		pixel_pipe_control[0].val = 0x40 | data_type1;
		pixel_pipe_control[1].val = data_type2 ? (0x40 | data_type2) : 0x00;
		pixel_pipe_control[2].val = 1 << vc_id;

		return max96717_set_registers(dev, pixel_pipe_control,
					     ARRAY_SIZE(pixel_pipe_control));
	} else {
		struct reg_pair map_pipe_control[] = {
			/* Pipe X DT addr + 0x2*pipe_id */
			{0x0314, 0x5E},  /* data_type1 */
			{0x0315, 0x52},  /* data_type2 */
			{0x0309, 0x01},  /* VC select */
			{0x030A, 0x00},
			{0x031C, 0x30},  /* BPP */
			{0x0102, 0x0E},  /* LIM_HEART: Disabled */
		};

		if (data_type1 == GMSL_CSI_DT_RGB_888)
			bpp = 0x18;

		map_pipe_control[0].addr += 0x2 * pipe_id;
		map_pipe_control[1].addr += 0x2 * pipe_id;
		map_pipe_control[2].addr += 0x2 * pipe_id;
		map_pipe_control[3].addr += 0x2 * pipe_id;
		map_pipe_control[4].addr += 0x1 * pipe_id;
		map_pipe_control[5].addr += 0x8 * pipe_id;

		map_pipe_control[0].val = 0x40 | data_type1;
		map_pipe_control[1].val = 0x40 | data_type2;
		if (pipe_id == 0)
			map_pipe_control[1].val |= 0x80;
		map_pipe_control[2].val = 1 << vc_id;
		map_pipe_control[3].val = 0x00;
		map_pipe_control[4].val = bpp;
		map_pipe_control[5].val = 0x0E;

		err = max96717_set_registers(dev, map_pipe_control,
					     ARRAY_SIZE(map_pipe_control));
	}

	return err;
}

/*
 * max96717_init_settings - Initialize default pipe/streaming settings
 *
 * In Tunnel Mode, per-pipe DT config is not functionally needed
 * but we configure pipes for d4xx compatibility.
 * Default: 4 pipes with YUV422_8 + EMBED, VC0-3.
 */
int max96717_init_settings(struct device *dev)
{
	int err = 0;
	int i;
	struct max96717 *priv = dev_get_drvdata(dev);

	struct reg_pair map_init[] = {
		/*
		 * PIPE_EN soft reset first, then enable tunnel mode.
		 * Note: FRONTTOP0 is bypassed in tunnel mode (per UG) —
		 * CSI-2 input goes directly into the tunnel, no port config needed.
		 */
		{MAX96717_PIPE_EN_ADDR, MAX96717_SOFT_RESET},
		{MAX96717_EXT11_ADDR, MAX96717_TUN_MODE_EN},
		{MAX96717_PIPE_EN_ADDR, MAX96717_PIPE_EN_ALL},
	};
	struct reg_pair pixel_init[] = {
		{MAX96717_PIPE_EN_ADDR, MAX96717_SOFT_RESET},
		{MAX96717_EXT11_ADDR, MAX96717_TUN_MODE_DIS},
		{MAX96717_FRONTTOP0_ADDR, MAX96717_FRONTTOP0_PIXEL_PORT_B},
		{MAX96717_FRONTTOP9_ADDR, MAX96717_FRONTTOP9_START_VIDEO},
		{MAX96717_VIDEO_TX0_ADDR, MAX96717_VIDEO_TX0_BPP_MANUAL},
		{MAX96717_VIDEO_TX1_ADDR, MAX96717_VIDEO_TX1_BPP16},
		{MAX96717_VIDEO_TX2_ADDR, MAX96717_VIDEO_TX2_DRIFT_DIS},
		{MAX96717_PIPE_EN_ADDR, MAX96717_PIPE_EN_ALL},
	};
	/*
	 * Release MFP7/MFP8 before any other config so the serializer
	 * stops driving the camera-side M2_I2C bus shared with the BMI088
	 * IMU. Confirmed on HW: tri-stating GPIO8 restores IMU I2C access.
	 */
	struct reg_pair gpio_release[] = {
		{MAX96717_GPIO7_A_ADDR, MAX96717_GPIO_A_HIGH_Z},
		{MAX96717_GPIO7_B_ADDR, MAX96717_GPIO_B_NO_PULL},
		{MAX96717_GPIO8_A_ADDR, MAX96717_GPIO_A_HIGH_Z},
		{MAX96717_GPIO8_B_ADDR, MAX96717_GPIO_B_NO_PULL},
	};

	mutex_lock(&priv->lock);

	/*
	 * After CTRL0 + I2C translation writes in
	 * setup_control, the MAX96717 needs settling time before
	 * accepting writes to PIPE_EN (0x02) and FRONTTOP registers.
	 */
	usleep_range(5000, 6000);

	/*
	 * Tri-state MFP7/MFP8 first to release the camera-side M2_I2C
	 * bus shared with the BMI088 IMU before configuring the rest of
	 * the SER.
	 */
	err = max96717_set_registers(dev, gpio_release,
				    ARRAY_SIZE(gpio_release));
	if (err)
		dev_warn(dev, "%s: failed to release MFP7/MFP8 (M2_I2C bus)\n",
			 __func__);

	if (priv->pixel_mode) {
		err = max96717_set_registers(dev, pixel_init,
					 ARRAY_SIZE(pixel_init));
		err |= __max96717_set_pipe(dev, 0, GMSL_CSI_DT_YUV422_8,
				   0, 0);
	} else {
		err = max96717_set_registers(dev, map_init, ARRAY_SIZE(map_init));

		for (i = 0; i < MAX96717_MAX_PIPES; i++)
			err |= __max96717_set_pipe(dev, i, GMSL_CSI_DT_YUV422_8,
					   GMSL_CSI_DT_EMBED, i);
	}

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_init_settings);

int max96717_set_pipe(struct device *dev, int pipe_id,
		      u8 data_type1, u8 data_type2, u32 vc_id)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	if (pipe_id > (MAX96717_MAX_PIPES - 1)) {
		dev_info(dev, "%s: input pipe_id: %d exceeds max96717 max pipes\n",
			 __func__, pipe_id);
		return -EINVAL;
	}

	dev_dbg(dev, "%s pipe_id %d, data_type1 %u, data_type2 %u, vc_id %u\n",
		__func__, pipe_id, data_type1, data_type2, vc_id);
	if (!priv->pixel_mode) {
		dev_dbg(dev, "%s: tunnel mode preserves VC/DT; no per-VC map needed\n",
			__func__);
		return 0;
	}

	mutex_lock(&priv->lock);

	err = __max96717_set_pipe(dev, pipe_id, data_type1, data_type2, vc_id);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_set_pipe);

/*
 * max96717_retrigger_tx - Toggle SER TX to restore tunnel detection after
 * a DES ONESHOT.  DES ONESHOT resets the data path; the SER must cycle
 * its TX enable for the DES to re-acquire the tunnel stream.
 */
void max96717_retrigger_tx(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);
	max96717_write_reg(dev, MAX96717_PIPE_EN_ADDR, MAX96717_SOFT_RESET);
	msleep(30);
	max96717_write_reg(dev, MAX96717_PIPE_EN_ADDR, MAX96717_PIPE_EN_ALL);
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96717_retrigger_tx);

/* ===== Probe / Remove ===== */

static struct regmap_config max96717_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static int max96717_probe(struct i2c_client *client
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
			   , const struct i2c_device_id *id
#endif
			   )
{
	struct max96717 *priv;
	int err = 0;
	struct device_node *node = client->dev.of_node;

	dev_info(&client->dev, "[MAX96717]: probing GMSL2 Serializer (Tunnel Mode)\n");

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
	priv->pixel_mode = of_property_read_bool(node, "adi,pixel-mode");

	if (of_get_property(node, "is-prim-ser", NULL)) {
		if (prim_priv__) {
			dev_err(&client->dev,
				"prim-ser already exists\n");
			return -EEXIST;
		}

		err = of_property_read_u32(node, "reg", &priv->def_addr);
		if (err < 0) {
			dev_err(&client->dev, "reg not found\n");
			return -EINVAL;
		}

		prim_priv__ = priv;
	}

	dev_set_drvdata(&client->dev, priv);

	dev_info(&client->dev, "%s: success\n", __func__);

	return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
static int max96717_remove(struct i2c_client *client)
#else
static void max96717_remove(struct i2c_client *client)
#endif
{
	struct max96717 *priv = dev_get_drvdata(&client->dev);

	if (priv == prim_priv__)
		prim_priv__ = NULL;
	mutex_destroy(&priv->lock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
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

static int __init max96717_init(void)
{
	return i2c_add_driver(&max96717_i2c_driver);
}

static void __exit max96717_exit(void)
{
	i2c_del_driver(&max96717_i2c_driver);
}

module_init(max96717_init);
module_exit(max96717_exit);

MODULE_DESCRIPTION("GMSL2 Serializer driver max96717 (Tunnel Mode)");
MODULE_AUTHOR("RealSense AI");
MODULE_LICENSE("GPL v2");

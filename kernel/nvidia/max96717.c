/*
 * max96717.c - max96717 GMSL Serializer driver
 *
 * Copyright (c) 2018-2026, NVIDIA CORPORATION.  All rights reserved.
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
#include <linux/module.h>
#include <media/max96717.h>

/* register specifics */
#define MAX96717_REG2_ADDR 0x2
#define MAX96717_CTRL0_ADDR 0x10
#define MAX96717_TX1_ADDR 0x29
#define MAX96717_EXT11_ADDR 0x383
#define MAX96717_FRONTTOP_10_ADDR 0x312
#define MAX96717_VIDEO_TX0_ADDR 0x110
#define MAX96717_VIDEO_TX1_ADDR 0x111
#define MAX96717_MIPI_RX0_ADDR 0x330
#define MAX96717_MIPI_RX1_ADDR 0x331
#define MAX96717_MIPI_RX8_ADDR 0x338

/* MIPI_RX0 (0x330): bit3 = mipi_rx_reset (not self-clearing), bit6 = non-cont-clk.
 * Pulsing reset re-arms the serializer MIPI RX PHY; both values keep bit6 as-is. */
#define MAX96717_MIPI_RX0_RESET 0x48
#define MAX96717_MIPI_RX0_NORMAL 0x40

#define MAX96717_2BE_ADDR 0x02BE
#define MAX96717_2BF_ADDR 0x02BF
#define MAX96717_2C0_ADDR 0x02C0
#define MAX96717_2C1_ADDR 0x02C1
#define MAX96717_2C2_ADDR 0x02C2
#define MAX96717_2C3_ADDR 0x02C3

#define MAX96717_I2C4_ADDR 0x44
#define MAX96717_I2C5_ADDR 0x45


/* GPIO 0 config values - Receives ID 0x17 */
#define MAX96717_2BE_ESYNC 0x24
#define MAX96717_2BF_ESYNC 0x60
#define MAX96717_2C0_ESYNC 0x57
/* GPIO 1 config values - Receives ID 0x17 */
#define MAX96717_2C1_ESYNC 0x24
#define MAX96717_2C2_ESYNC 0x60
#define MAX96717_2C3_ESYNC 0x57

#define MAX96717_GPIO7_A_ADDR		0x02D3	/* MFP7 / pass-through SDA1 */
#define MAX96717_GPIO7_B_ADDR		0x02D4
#define MAX96717_GPIO8_A_ADDR		0x02D6	/* MFP8 / pass-through SCL1 */
#define MAX96717_GPIO8_B_ADDR		0x02D7
#define MAX96717_GPIO6_A_ADDR		0x02D0
#define MAX96717_GPIO6_B_ADDR		0x02D1
#define MAX96717_GPIO6_C_ADDR		0x02D2
/* GPIO_A: GPIO_OUT_DIS=1 (high-Z), GPIO_TX_EN=0, GPIO_RX_EN=0 */
#define MAX96717_GPIO_A_HIGH_Z		0x01
/* GPIO_B: PULL_UPDN_SEL=None, TX_ID=0 */
#define MAX96717_GPIO_B_NO_PULL		0x00

struct max96717_client_ctx {
	struct gmsl_link_ctx *g_ctx;
	bool st_done;
};

struct max96717 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	struct max96717_client_ctx g_client;
	struct mutex lock;
	/* bit[ser_vc_id] set while that pipe is streaming (set in set_pipe,
	 * cleared in stream_stop). When it drops to 0 the MIPI RX is re-armed. */
	u8 active_vc_mask;
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

	err = max96717_write_reg(dev, MAX96717_I2C4_ADDR, (g_ctx->sdev_reg << 1));
	err |= max96717_write_reg(dev, MAX96717_I2C5_ADDR, (g_ctx->sdev_def << 1));
	if (err) {
		goto error;
	}

	g_ctx->serdev_found = true;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96717_setup_control);

static int max96717_write_acc(struct device *dev, struct regmap *regmap,
			     u16 addr, u8 val, int *err)
{
	unsigned int readback = 0;

	if (*err)
		return *err;
	*err = max96717_write_reg(dev, addr, val);
	if (!*err)
		regmap_read(regmap, addr, &readback);
	dev_dbg(dev, "%s: reg 0x%04x written 0x%02x readback 0x%02x\n",
		__func__, addr, val, readback);
	return *err;
}

int max96717_enable_gpio_tunneling(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);

	/*
	 * GPIO0 carries H_VSYNC_TRIG from the deserializer on RX channel 23,
	 * GPIO1 carries RGB_FSYNC on the same channel (Currently unused by HKR), and GPIO6 returns the
	 * camera H_STROBE_OUT_1V8 signal on TX channel 31. These camera-side
	 * signals are required in both pixel and tunnel video modes.
	 */
	/* GPIO 0 receiver */
	max96717_write_acc(dev, priv->regmap, MAX96717_2BE_ADDR, MAX96717_2BE_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2BF_ADDR, MAX96717_2BF_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C0_ADDR, MAX96717_2C0_ESYNC, &err);
	/* GPIO 1 receiver */
	max96717_write_acc(dev, priv->regmap, MAX96717_2C1_ADDR, MAX96717_2C1_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C2_ADDR, MAX96717_2C2_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C3_ADDR, MAX96717_2C3_ESYNC, &err);
	/* GPIO 6 transmitter */
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_A_ADDR, 0x27, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_B_ADDR, 0x1F, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_C_ADDR, 0x00, &err);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_enable_gpio_tunneling);

int max96717_disable_gpio_tunneling(struct device *dev)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);

	/* Restore default SRC pin function — plain GPIO, no ESYNC tunneling */
	max96717_write_acc(dev, priv->regmap, MAX96717_2BE_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2BF_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C0_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C1_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C2_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C3_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_A_ADDR, 0x99, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_B_ADDR, 0xA6, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_GPIO6_C_ADDR, 0x46, &err);

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
		err = -EINVAL;
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

struct reg_pair {
	u16 addr;
	u8 val;
};

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

static int max96717_mipi_rx_reset_pulse(struct device *dev)
{
	int err;

	err  = max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR,
				  MAX96717_MIPI_RX0_RESET);
	usleep_range(2000, 2100);
	err |= max96717_write_reg(dev, MAX96717_MIPI_RX0_ADDR,
				  MAX96717_MIPI_RX0_NORMAL);

	return err;
}

int max96717_init_settings(struct device *dev)
{
	int err = 0;
	struct max96717 *priv = dev_get_drvdata(dev);

	/*
	 * Serializer (MAX96717 @ 0x40) bring-up, ported verbatim from the
	 * validated GMSL config XML (MAX96717 + MAX96712, 4-lane 700Mbps/lane,
	 * pixel mode). For now ALL serializer registers live here; they will be
	 * split between init_settings() and set_pipe() later. The XML repeats
	 * this exact sequence during its AIO re-bring-up phase, so it is listed
	 * once here.
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
	};
	struct reg_pair ser_cfg_post[] = {
		{MAX96717_MIPI_RX8_ADDR, 0x22}, /* 0x338 - MIPI_RX8 settle=0x22 (t_hs[5:4]/t_clk[1:0]) */
		{MAX96717_REG2_ADDR, 0x43}, /* 0x2 - REG2: VID_TX_EN */
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

	/* Fresh link bring-up: no pipe is streaming yet. Clearing this here
	 * ensures a deserializer/link reset can't strand a stale bit and
	 * permanently suppress the last-stream MIPI RX re-arm. */
	priv->active_vc_mask = 0;

	err |= max96717_set_registers(dev, gpio_release,
				     ARRAY_SIZE(gpio_release));

	err |= max96717_set_registers(dev, ser_cfg_pre,
				     ARRAY_SIZE(ser_cfg_pre));
	msleep(100);
	err |= max96717_set_registers(dev, ser_cfg_mid,
				     ARRAY_SIZE(ser_cfg_mid));
	err |= max96717_mipi_rx_reset_pulse(dev);
	err |= max96717_set_registers(dev, ser_cfg_post,
				     ARRAY_SIZE(ser_cfg_post));

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_init_settings);

int max96717_set_pipe(struct device *dev, int pipe_id,
		     u8 data_type1, u8 data_type2, u32 vc_id)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	if (vc_id >= 8)
		return -EINVAL;

	mutex_lock(&priv->lock);
	
	priv->active_vc_mask |= (u8)BIT(vc_id);
	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_set_pipe);

int max96717_stream_stop(struct device *dev, u32 vc_id)
{
	struct max96717 *priv = dev_get_drvdata(dev);
	int err = 0;

	if (vc_id >= 8)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->active_vc_mask &= (u8)~BIT(vc_id);

	if (priv->active_vc_mask == 0) {
		/*
		 * Last stream stopped -> the shared MAX96717 video pipe is going
		 * idle. Pulse the MIPI RX reset to re-arm the MIPI RX PHY while
		 * idle, so the next stream re-locks cleanly instead of wedging.
		 */
		err = max96717_mipi_rx_reset_pulse(dev);
		dev_dbg(dev, "%s: last stream stopped, MIPI RX re-armed (err %d)\n",
			__func__, err);
	}
	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_stream_stop);
static int max96717_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct max96717 *priv;
	int err = 0;

	dev_info(&client->dev, "[MAX96717]: probing GMSL Serializer\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&client->dev, "dev allocation failed\n");
		return -ENOMEM;
	}
	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(priv->i2c_client,
				&max96717_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	mutex_init(&priv->lock);

	dev_set_drvdata(&client->dev, priv);

	/* dev communication gets validated when GMSL link setup is done */
	dev_info(&client->dev, "%s:  success\n", __func__);

	return err;
}

static int max96717_remove(struct i2c_client *client)
{
	struct max96717 *priv;

	if (client != NULL) {
		priv = dev_get_drvdata(&client->dev);
		mutex_destroy(&priv->lock);
		i2c_unregister_device(client);
		client = NULL;
	}

	return 0;
}

static const struct i2c_device_id max96717_id[] = {
	{ "max96717", 0 },
	{ },
};

static const struct of_device_id max96717_of_match[] = {
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

MODULE_DESCRIPTION("GMSL Serializer driver max96717");
MODULE_AUTHOR("Sudhir Vyas <svyas@nvidia.com>");
MODULE_AUTHOR("Dmitry Perchanov <dmitry.perchanov@intel.com>");
MODULE_LICENSE("GPL v2");
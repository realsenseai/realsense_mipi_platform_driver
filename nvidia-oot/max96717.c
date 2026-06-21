// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
/*
 * max96717.c - max96717 GMSL Serializer driver
 */

#include <nvidia/conftest.h>

#include <media/camera_common.h>
#include <linux/module.h>
#include <media/max96717.h>

/* register specifics */
#define MAX96717_MIPI_RX0_ADDR 0x330
#define MAX96717_MIPI_RX1_ADDR 0x331
#define MAX96717_MIPI_RX2_ADDR 0x332
#define MAX96717_MIPI_RX3_ADDR 0x333

#define MAX96717_CTRL0_ADDR 0x10
#define MAX96717_SRC_CTRL_ADDR 0x2BF
#define MAX96717_SRC_PWDN_ADDR 0x02BE
#define MAX96717_2C0_ADDR 0x02C0
#define MAX96717_2C1_ADDR 0x02C1
#define MAX96717_2C2_ADDR 0x02C2
#define MAX96717_2C3_ADDR 0x02C3
#define MAX96717_SRC_OUT_RCLK_ADDR 0x3F1
#define MAX96717_START_PIPE_ADDR 0x311
#define MAX96717_PIPE_EN_ADDR 0x2
#define MAX96717_CSI_PORT_SEL_ADDR 0x308

#define MAX96717_I2C4_ADDR 0x44
#define MAX96717_I2C5_ADDR 0x45

#define MAX96717_DEV_ADDR 0x00

#define MAX96717_STREAM_PIPE_UNUSED 0x22
#define MAX96717_CSI_MODE_1X4 0x00
#define MAX96717_CSI_MODE_2X2 0x03
#define MAX96717_CSI_MODE_2X4 0x06

#define MAX96717_CSI_PORT_B(num_lanes) (((num_lanes) << 4) & 0xF0)
#define MAX96717_CSI_PORT_A(num_lanes) ((num_lanes) & 0x0F)

#define MAX96717_CSI_1X4_MODE_LANE_MAP1 0xE0
#define MAX96717_CSI_1X4_MODE_LANE_MAP2 0x04

#define MAX96717_CSI_2X4_MODE_LANE_MAP1 0xEE
#define MAX96717_CSI_2X4_MODE_LANE_MAP2 0xE4

#define MAX96717_CSI_2X2_MODE_LANE_MAP1 MAX96717_CSI_2X4_MODE_LANE_MAP1
#define MAX96717_CSI_2X2_MODE_LANE_MAP2 MAX96717_CSI_2X4_MODE_LANE_MAP2

#define MAX96717_ST_ID_0 0x0
#define MAX96717_ST_ID_1 0x1
#define MAX96717_ST_ID_2 0x2
#define MAX96717_ST_ID_3 0x3

#define MAX96717_PIPE_X_START_B 0x80
#define MAX96717_PIPE_Y_START_B 0x40
#define MAX96717_PIPE_Z_START_B 0x20
#define MAX96717_PIPE_U_START_B 0x10

#define MAX96717_PIPE_X_START_A 0x1
#define MAX96717_PIPE_Y_START_A 0x2
#define MAX96717_PIPE_Z_START_A 0x4
#define MAX96717_PIPE_U_START_A 0x8

#define MAX96717_START_PORT_A 0x10
#define MAX96717_START_PORT_B 0x20

#define MAX96717_CSI_LN2 0x1
#define MAX96717_CSI_LN4 0x3

#define MAX96717_EN_LINE_INFO 0x40

#define MAX96717_VID_TX_EN_X 0x10
#define MAX96717_VID_TX_EN_Y 0x20
#define MAX96717_VID_TX_EN_Z 0x40
#define MAX96717_VID_TX_EN_U 0x80

#define MAX96717_VID_INIT 0x3
#define MAX96717_SRC_RCLK 0x89

#define MAX96717_RESET_ALL 0x80
#define MAX96717_RESET_SRC 0x60
#define MAX96717_RESET_ESYNC 0x77

#define MAX96717_PWDN_GPIO 0x90
#define MAX96717_PWDN_ESYNC_EXT 0x24

#define MAX96717_2C0_ESYNC 0x57
#define MAX96717_2C1_ESYNC 0x05
#define MAX96717_2C2_ESYNC 0x1F
#define MAX96717_2C3_ESYNC 0x57

struct max96717_client_ctx {
	struct gmsl_link_ctx *g_ctx;
	bool st_done;
};

struct max96717 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	struct max96717_client_ctx g_client;
	struct mutex lock;
};

struct map_ctx {
	u8 dt;
	u16 addr;
	u8 val;
	u8 st_id;
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

	max96717_write_reg(dev, MAX96717_I2C4_ADDR, (g_ctx->sdev_reg << 1));
	max96717_write_reg(dev, MAX96717_I2C5_ADDR, (g_ctx->sdev_def << 1));

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

	/* Configure ESYNC registers for MAX96712A deserializer */
	max96717_write_acc(dev, priv->regmap, MAX96717_SRC_PWDN_ADDR, MAX96717_PWDN_ESYNC_EXT, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_SRC_CTRL_ADDR, MAX96717_RESET_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C0_ADDR, MAX96717_2C0_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C1_ADDR, MAX96717_2C1_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C2_ADDR, MAX96717_2C2_ESYNC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C3_ADDR, MAX96717_2C3_ESYNC, &err);

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
	max96717_write_acc(dev, priv->regmap, MAX96717_SRC_PWDN_ADDR, MAX96717_PWDN_GPIO, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_SRC_CTRL_ADDR, MAX96717_RESET_SRC, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C0_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C1_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C2_ADDR, 0x00, &err);
	max96717_write_acc(dev, priv->regmap, MAX96717_2C3_ADDR, 0x00, &err);

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
	return 0;
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
		{0x0002, 0x03}, /* REG2: VID_TX_EN=0, INIT=1 */
		{0x0010, 0x21}, /* CTRL0: RESET_ONESHOT, GMSL2 link A */
	};
	struct reg_pair ser_cfg_mid[] = {
		{0x0029, 0x00}, /* FEC OFF */
		{0x0383, 0x00}, /* Pixel mode */
		{0x0312, 0x00}, /* FRONTTOP_10 no double-pixel (Mode 1 uniform bpp) */
		{0x0313, 0x00}, /* FRONTTOP_10 no double-pixel (Mode 1 uniform bpp) */
		{0x0110, 0x60}, /* VIDEO_TX0 AUTO_BPP=0 ENC_MODE=10 */
		{0x0111, 0x10}, /* VIDEO_TX1 BPP=16 forced (matches DT) */
		{0x0331, 0x30}, /* MIPI_RX1: 4-lane */
		{0x0330, 0x48}, /* MIPI_RX0 reset ON + non-cont-clk */
	};
	struct reg_pair ser_cfg_post[] = {
		{0x0330, 0x40}, /* MIPI_RX0 reset OFF, non-cont-clk enabled */
		{0x0338, 0x22}, /* MIPI_RX8 settle=0x22 (t_hs[5:4]/t_clk[1:0]) */
		{0x0002, 0x43}, /* REG2: VID_TX_EN */
	};

	mutex_lock(&priv->lock);

	err |= max96717_set_registers(dev, ser_cfg_pre,
				     ARRAY_SIZE(ser_cfg_pre));
	msleep(100);
	err |= max96717_set_registers(dev, ser_cfg_mid,
				     ARRAY_SIZE(ser_cfg_mid));
	/* XML waits 2ms between MIPI_RX0 reset assert and release */
	usleep_range(2000, 2100);
	err |= max96717_set_registers(dev, ser_cfg_post,
				     ARRAY_SIZE(ser_cfg_post));

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96717_init_settings);

int max96717_set_pipe(struct device *dev, int pipe_id,
		     u8 data_type1, u8 data_type2, u32 vc_id)
{
	/* No runtime config needed in pixel mode */
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
	{ .compatible = "maxim,max96717", },
	{ },
};
MODULE_DEVICE_TABLE(of, max96717_of_match);
MODULE_DEVICE_TABLE(i2c, max96717_id);

static struct i2c_driver max96717_i2c_driver = {
	.driver = {
		.name = "max96717",
		.owner = THIS_MODULE,
	},
	.probe = max96717_probe,
	.remove = max96717_remove,
	.id_table = max96717_id,
};

module_i2c_driver(max96717_i2c_driver);

MODULE_DESCRIPTION("GMSL Serializer driver max96717");
MODULE_AUTHOR("Sudhir Vyas <svyas@nvidia.com>");
MODULE_LICENSE("GPL v2");

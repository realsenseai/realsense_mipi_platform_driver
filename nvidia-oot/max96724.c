// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 RealSense, Inc.
/*
 * MAX96724 GMSL2 quad deserializer driver
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>
#include <media/max96724.h>

/* Link and control registers. */
#define MAX96724_LINK_EN_ADDR		0x0006
#define MAX96724_LINK_RATE_AB_ADDR	0x0010
#define MAX96724_LINK_RATE_CD_ADDR	0x0011
#define MAX96724_LINK_A_LOCK_ADDR	0x001A
#define MAX96724_LINK_B_LOCK_ADDR	0x000A
#define MAX96724_LINK_C_LOCK_ADDR	0x000B
#define MAX96724_LINK_D_LOCK_ADDR	0x000C
#define MAX96724_ONESHOT_ADDR		0x0018
#define MAX96724_REG13_ADDR		0x000D
#define MAX96724_ERRCH_A_ADDR		0x1449
#define MAX96724_ERRCH_B_ADDR		0x1549
#define MAX96724_ERRCH_C_ADDR		0x1649
#define MAX96724_ERRCH_D_ADDR		0x1749
#define MAX96724_ERRCH_FORCE_ON		0x75
#define MAX96724_SRAM_LCRC_ERR_ADDR	0x0458

/* Video pipe registers. */
#define MAX96724_PIPE_SEL0_ADDR		0xF0
#define MAX96724_PIPE_SEL1_ADDR		0xF1
#define MAX96724_PIPE_EN_ADDR		0xF4
#define MAX96724_VID_RX0_P0_ADDR	0x100
#define MAX96724_VID_RX6_P0_ADDR	0x106
#define MAX96724_VID_RX_STRIDE		0x12
#define MAX96724_BACKTOP_EN_ADDR	0x040B

/* CSI output registers. */
#define MAX96724_DPLL_FREQ0_ADDR	0x0415
#define MAX96724_CSI_OUT_CFG_ADDR	0x08A0
#define MAX96724_CSI_PHY_EN_ADDR	0x08A2
#define MAX96724_CSI_LANE_MAP1_ADDR	0x08A3
#define MAX96724_CSI_LANE_MAP2_ADDR	0x08A4
#define MAX96724_CSI_PHY_POL0_ADDR	0x08A5
#define MAX96724_CSI_PHY_POL1_ADDR	0x08A6
#define MAX96724_CSI_PHY_POL_SWAP	0x3F
#define MAX96724_MIPI_CTRL_SEL_ADDR	0x08CA
#define MAX96724_MIPI_CTRL_SEL		0xE4
#define MAX96724_LANE_CTRL0_ADDR	0x090A

/* Pipe X registers; subsequent pipes use a 0x40 stride. */
#define MAX96724_TX11_PIPE_X_EN_ADDR		0x090B
#define MAX96724_TX45_PIPE_X_DST_CTRL_ADDR	0x092D
#define MAX96724_PIPE_X_SRC_0_MAP_ADDR		0x090D
#define MAX96724_PIPE_X_DST_0_MAP_ADDR		0x090E
#define MAX96724_PIPE_X_SRC_1_MAP_ADDR		0x090F
#define MAX96724_PIPE_X_DST_1_MAP_ADDR		0x0910
#define MAX96724_PIPE_X_SRC_2_MAP_ADDR		0x0911
#define MAX96724_PIPE_X_DST_2_MAP_ADDR		0x0912
#define MAX96724_PIPE_X_SRC_3_MAP_ADDR		0x0913
#define MAX96724_PIPE_X_DST_3_MAP_ADDR		0x0914
#define MAX96724_PIPE_X_ST_SEL_ADDR	0x0933
#define MAX96724_TX46_PIPE_X_ADDR	0x092E
#define MAX96724_TX47_PIPE_X_ADDR	0x092F
#define MAX96724_TX48_PIPE_X_ADDR	0x0930
#define MAX96724_TX49_PIPE_X_ADDR	0x0931
#define MAX96724_PIPE0_TUN_EN_ADDR	0x0936
#define MAX96724_PIPE0_TUN_DEST_ADDR	0x0939

/* Register values. */
#define MAX96724_SOFT_RESET		0x40
#define MAX96724_LINK_LOCKED		0x08
#define MAX96724_LINK_LOCK_POLL_MS	100
#define MAX96724_LINK_LOCK_TIMEOUT_MS	5000
#define MAX96724_BACKTOP_CSIB_EN	0x02
#define MAX96724_VID_RX0_CFG_VAL	0x33
#define MAX96724_VID_RX6_CFG_VAL	0x0A
#define MAX96724_TUN_EN			0x21
#define MAX96724_TUN_DEST_CTRL1		0x10
#define MAX96724_CSI_MODE_2X4_VAL	0x04
#define MAX96724_CSI_OUT_EN_VAL		0x84
#define MAX96724_CSI_PHY_EN_ALL		0xF0
#define MAX96724_CSI_LANE_MAP_DEFAULT	0xE4
#define MAX96724_LANE_CTRL_4LANE	0xC0
#define MAX96724_DPLL_2000MBPS		0x34
#define MAX96724_ONESHOT_ALL		0x0F
#define MAX96724_PIPE_SEL0		0x62
#define MAX96724_PIPE_SEL1		0xEA
#define MAX96724_PIPE_EN_4		0x0F
#define MAX96724_ALL_MAP_CTRL1		0x55
#define MAX96724_MAP3_CTRL1		0x15
#define MAX96724_LINK_EN_BASE		0xF0

#define MAX96717_DEFAULT_ADDR		0x40
#define MAX96717_DEV_ADDR		0x0000

#define MAX96724_MAX_SOURCES		4
#define MAX96724_MAX_PIPES		4
#define MAX96724_MAX_LINKS		4

#define MAX96724_PIPE_X			0
#define MAX96724_PIPE_Y			1
#define MAX96724_PIPE_Z			2
#define MAX96724_PIPE_U			3
#define MAX96724_CSI_MODE_2X4		0x08
#define MAX96724_LANE_MAP1_2X4		0xE4

struct max96724_source_ctx {
	struct gmsl_link_ctx *g_ctx;
	struct device *s_dev;
	u32 serdes_csi_link;
	u32 num_csi_lanes;
	bool control_setup;
	bool serdev_found;
};

struct pipe_ctx {
	u32 id;
	u32 dt_type;
	u32 dt_type2;
	u32 vc_id;
	u32 st_count;
	bool map_configured;
};

struct max96724 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	u32 num_src;
	u32 max_src;
	u32 num_src_found;
	struct max96724_source_ctx sources[MAX96724_MAX_SOURCES];
	/* Protects state and multi-register programming sequences. */
	struct mutex lock;
	u32 sdev_ref;
	bool link_setup;
	struct pipe_ctx pipe[MAX96724_MAX_PIPES];
	u8 csi_mode;
	u8 lane_mp1;
	struct gpio_desc *reset_gpio;
	int pw_ref;
	struct regulator *vdd_cam_1v2;
	u8 link_speed;  /* DT-configurable: 3 or 6 Gbps */
	u8 link_mask;
	bool poc_enabled; /* FG24-4CH board POC/IO enable applied once */
	bool datapath_retriggered;
	u8 retriggered_pipe_mask;
};

struct reg_pair {
	u16 addr;
	u8 val;
};

static int max96724_write_reg(struct device *dev, u16 addr, u8 val)
{
	struct max96724 *priv;
	int err;

	if (!dev)
		return -EINVAL;

	priv = dev_get_drvdata(dev);
	if (!priv || !priv->regmap)
		return -ENODEV;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		dev_err(dev, "%s:i2c write failed, 0x%x = %x\n",
			__func__, addr, val);

	/* delay before next i2c command as required for SERDES link */
	usleep_range(100, 110);

	return err;
}

static int max96724_set_registers(struct device *dev, struct reg_pair *map,
				  u32 count)
{
	int err = 0;
	u32 j;

	for (j = 0; j < count; j++) {
		err = max96724_write_reg(dev, map[j].addr, map[j].val);
		if (err)
			break;
	}

	return err;
}

static u16 max96724_link_lock_addr(u32 link)
{
	switch (link) {
	case 0:
		return MAX96724_LINK_A_LOCK_ADDR;
	case 1:
		return MAX96724_LINK_B_LOCK_ADDR;
	case 2:
		return MAX96724_LINK_C_LOCK_ADDR;
	case 3:
		return MAX96724_LINK_D_LOCK_ADDR;
	default:
		return 0;
	}
}

static int max96724_wait_link_lock(struct device *dev, u32 link)
{
	struct max96724 *priv;
	u16 lock_addr = max96724_link_lock_addr(link);
	unsigned int lock = 0;
	int elapsed;
	int err;

	if (!dev)
		return -EINVAL;

	priv = dev_get_drvdata(dev);
	if (!priv || !priv->regmap)
		return -ENODEV;

	if (!lock_addr)
		return -EINVAL;

	for (elapsed = 0;
	     elapsed <= MAX96724_LINK_LOCK_TIMEOUT_MS;
	     elapsed += MAX96724_LINK_LOCK_POLL_MS) {
		err = regmap_read(priv->regmap, lock_addr, &lock);
		if (err)
			return err;

		if (lock & MAX96724_LINK_LOCKED) {
			dev_dbg(dev, "GMSL link %c locked after %d ms\n",
				'A' + link, elapsed);
			return 0;
		}

		if (elapsed < MAX96724_LINK_LOCK_TIMEOUT_MS)
			msleep(MAX96724_LINK_LOCK_POLL_MS);
	}

	dev_err(dev, "GMSL link %c lock timeout (reg 0x%04x=0x%02x)\n",
		'A' + link, lock_addr, lock);

	return -ETIMEDOUT;
}

/* Caller must hold priv->lock. */
static int max96724_get_sdev_idx_locked(struct max96724 *priv,
					struct device *s_dev,
					unsigned int *idx)
{
	unsigned int i;

	for (i = 0; i < priv->num_src; i++) {
		if (priv->sources[i].s_dev == s_dev)
			break;
	}
	if (i == priv->num_src)
		return -EINVAL;

	if (idx)
		*idx = i;

	return 0;
}

static void max96724_pipes_reset(struct max96724 *priv)
{
	/*
	 * Default pipe configuration for D5xx/SC1.2:
	 * In tunnel mode, DT types are not used for filtering
	 * but we initialize them for d4xx framework compatibility.
	 */
	struct pipe_ctx pipe_defaults[] = {
		{ .id = MAX96724_PIPE_X, .dt_type = GMSL_CSI_DT_RAW_12 },
		{ .id = MAX96724_PIPE_Y, .dt_type = GMSL_CSI_DT_RAW_12 },
		{ .id = MAX96724_PIPE_Z, .dt_type = GMSL_CSI_DT_EMBED },
		{ .id = MAX96724_PIPE_U, .dt_type = GMSL_CSI_DT_EMBED },
	};

	memcpy(priv->pipe, pipe_defaults, sizeof(pipe_defaults));
}

static void max96724_reset_ctx(struct max96724 *priv)
{
	unsigned int i;

	priv->link_setup = false;
	priv->num_src_found = 0;
	max96724_pipes_reset(priv);
	for (i = 0; i < priv->num_src; i++) {
		priv->sources[i].control_setup = false;
		priv->sources[i].serdev_found = false;
	}
}

static bool max96724_source_control_put(struct max96724 *priv,
					unsigned int source_idx)
{
	struct max96724_source_ctx *source = &priv->sources[source_idx];

	if (!source->control_setup)
		return false;

	source->control_setup = false;
	if (source->serdev_found) {
		if (WARN_ON(!priv->num_src_found))
			priv->num_src_found = 0;
		else
			priv->num_src_found--;
		source->serdev_found = false;
	}

	if (WARN_ON(!priv->sdev_ref))
		priv->sdev_ref = 0;
	else
		priv->sdev_ref--;

	return priv->sdev_ref == 0;
}

static int max96724_reset_control_locked(struct device *dev,
					 struct max96724 *priv,
					 unsigned int source_idx)
{
	int err = 0;

	if (!max96724_source_control_put(priv, source_idx))
		return 0;

	max96724_reset_ctx(priv);
	err = max96724_write_reg(dev, MAX96724_REG13_ADDR,
				 MAX96724_SOFT_RESET);
	msleep(100);

	return err;
}

static int max96724_configure_csi_port(struct device *dev,
				       struct max96724 *priv)
{
	struct reg_pair map[] = {
		{MAX96724_CSI_OUT_CFG_ADDR, MAX96724_CSI_MODE_2X4_VAL},
		{MAX96724_CSI_PHY_EN_ADDR, MAX96724_CSI_PHY_EN_ALL},
		{MAX96724_CSI_LANE_MAP1_ADDR, priv->lane_mp1},
		{MAX96724_CSI_LANE_MAP2_ADDR, MAX96724_CSI_LANE_MAP_DEFAULT},
		{MAX96724_CSI_PHY_POL0_ADDR, MAX96724_CSI_PHY_POL_SWAP},
		{MAX96724_CSI_PHY_POL1_ADDR, MAX96724_CSI_PHY_POL_SWAP},
	};
	int err;
	unsigned int i;

	err = max96724_set_registers(dev, map, ARRAY_SIZE(map));
	for (i = 0; !err && i < MAX96724_MAX_PIPES; i++) {
		u16 addr = MAX96724_LANE_CTRL0_ADDR + 0x40 * i;

		err = max96724_write_reg(dev, addr,
					 MAX96724_LANE_CTRL_4LANE);
	}

	return err;
}

static int max96724_enable_error_channels(struct device *dev, u8 link_mask)
{
	static const u16 addr[] = {
		MAX96724_ERRCH_A_ADDR, MAX96724_ERRCH_B_ADDR,
		MAX96724_ERRCH_C_ADDR, MAX96724_ERRCH_D_ADDR,
	};
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int i;
	int err = 0;

	if (priv->link_speed != 6)
		return 0;

	for (i = 0; !err && i < ARRAY_SIZE(addr); i++) {
		if (link_mask & BIT(i))
			err = max96724_write_reg(dev, addr[i],
						 MAX96724_ERRCH_FORCE_ON);
	}

	return err;
}

/* Caller holds priv->lock. */
static int max96724_configure_datapath(struct device *dev, bool oneshot,
				       bool enable_pipes)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int i;
	int err;

	err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, 0x00);
	err |= max96724_write_reg(dev, MAX96724_PIPE_SEL0_ADDR,
				  MAX96724_PIPE_SEL0);
	err |= max96724_write_reg(dev, MAX96724_PIPE_SEL1_ADDR,
				  MAX96724_PIPE_SEL1);
	err |= max96724_configure_csi_port(dev, priv);

	for (i = 0; !err && i < MAX96724_MAX_PIPES; i++) {
		u16 addr = MAX96724_DPLL_FREQ0_ADDR + 3 * i;

		err = max96724_write_reg(dev, addr,
					 MAX96724_DPLL_2000MBPS);
	}

	err |= max96724_write_reg(dev, MAX96724_MIPI_CTRL_SEL_ADDR,
				  MAX96724_MIPI_CTRL_SEL);
	for (i = 0; !err && i < MAX96724_MAX_PIPES; i++) {
		u16 pipe_off = 0x40 * i;
		u16 vid_off = MAX96724_VID_RX_STRIDE * i;

		err = max96724_write_reg(dev,
					 MAX96724_PIPE_X_ST_SEL_ADDR + pipe_off,
					 0x10);
		err |= max96724_write_reg(dev,
			MAX96724_VID_RX0_P0_ADDR + vid_off,
			MAX96724_VID_RX0_CFG_VAL);
		err |= max96724_write_reg(dev,
			MAX96724_VID_RX6_P0_ADDR + vid_off,
			MAX96724_VID_RX6_CFG_VAL);
		err |= max96724_write_reg(dev,
			MAX96724_TX49_PIPE_X_ADDR + pipe_off, 0x00);
		err |= max96724_write_reg(dev,
			MAX96724_PIPE0_TUN_EN_ADDR + pipe_off,
			MAX96724_TUN_EN);
		err |= max96724_write_reg(dev,
			MAX96724_PIPE0_TUN_DEST_ADDR + pipe_off,
			MAX96724_TUN_DEST_CTRL1);
		err |= max96724_write_reg(dev,
			MAX96724_TX45_PIPE_X_DST_CTRL_ADDR + pipe_off,
			MAX96724_ALL_MAP_CTRL1);
		err |= max96724_write_reg(dev,
			MAX96724_TX46_PIPE_X_ADDR + pipe_off,
			MAX96724_ALL_MAP_CTRL1);
		err |= max96724_write_reg(dev,
			MAX96724_TX47_PIPE_X_ADDR + pipe_off,
			MAX96724_ALL_MAP_CTRL1);
		err |= max96724_write_reg(dev,
			MAX96724_TX48_PIPE_X_ADDR + pipe_off,
			MAX96724_ALL_MAP_CTRL1);
	}

	err |= max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR,
				  MAX96724_BACKTOP_CSIB_EN);
	err |= max96724_write_reg(dev, MAX96724_CSI_OUT_CFG_ADDR,
				  MAX96724_CSI_OUT_EN_VAL);
	if (oneshot)
		err |= max96724_write_reg(dev, MAX96724_ONESHOT_ADDR,
					  MAX96724_ONESHOT_ALL);
	if (enable_pipes)
		err |= max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
					  MAX96724_PIPE_EN_4);
	return err;
}

/* FG24 bridge POC/IO enable sequence. */
#define MAX96724_POC_IO_EN_ADDR		0x0001
#define MAX96724_POC_IO_EN_VAL		0xE0
#define MAX96724_POC_MFP8_ADDR		0x0319
#define MAX96724_POC_MFP8_PRE		0x80
#define MAX96724_POC_MFP8_HIGH		0x98

static int max96724_board_poc_enable(struct device *dev)
{
	int err;

	err = max96724_write_reg(dev, MAX96724_POC_IO_EN_ADDR,
				 MAX96724_POC_IO_EN_VAL);
	err |= max96724_write_reg(dev, MAX96724_POC_MFP8_ADDR,
				  MAX96724_POC_MFP8_PRE);
	if (err)
		return err;

	msleep(100);
	err = max96724_write_reg(dev, MAX96724_POC_MFP8_ADDR,
				 MAX96724_POC_MFP8_HIGH);
	msleep(500);

	return err;
}

int max96724_power_on(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);
	if (priv->pw_ref == 0) {
		usleep_range(1, 2);
		if (priv->reset_gpio)
			gpiod_set_value_cansleep(priv->reset_gpio, 0);

		usleep_range(30, 50);

		if (priv->vdd_cam_1v2) {
			err = regulator_enable(priv->vdd_cam_1v2);
			if (unlikely(err))
				goto ret;
		}

		usleep_range(30, 50);

		/* exit reset mode: XCLR */
		if (priv->reset_gpio) {
			gpiod_set_value_cansleep(priv->reset_gpio, 0);
			usleep_range(30, 50);
			gpiod_set_value_cansleep(priv->reset_gpio, 1);
			usleep_range(30, 50);
		}

		msleep(20);
	}

	priv->pw_ref++;

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_power_on);

void max96724_power_off(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);
	if (!priv->pw_ref || --priv->pw_ref)
		goto out;

	usleep_range(1, 2);
	if (priv->reset_gpio)
		gpiod_set_value_cansleep(priv->reset_gpio, 0);

	if (priv->vdd_cam_1v2)
		regulator_disable(priv->vdd_cam_1v2);
out:
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_power_off);

/*
 * Access a serializer through the deserializer's I2C pass-through. The caller
 * holds priv->lock, so temporarily changing the client address is serialized
 * with every other MAX96724 transaction.
 */
static int max96724_read_slave_reg(struct max96724 *priv, u8 slave_addr,
				   u16 addr, unsigned int *val)
{
	struct i2c_client *client = priv->i2c_client;
	u16 saved_addr = client->addr;
	int err;

	client->addr = slave_addr;
	err = regmap_read(priv->regmap, addr, val);
	client->addr = saved_addr;
	usleep_range(100, 110);

	return err;
}

static int max96724_write_slave_reg(struct max96724 *priv, u8 slave_addr,
				    u16 addr, u8 val)
{
	struct i2c_client *client = priv->i2c_client;
	u16 saved_addr = client->addr;
	int err;

	client->addr = slave_addr;
	err = regmap_write(priv->regmap, addr, val);
	client->addr = saved_addr;
	if (err)
		dev_err(&client->dev,
			"%s: I2C write failed, slave 0x%02x reg 0x%04x\n",
			__func__, slave_addr, addr);
	usleep_range(100, 110);

	return err;
}

static int max96724_enable_links(struct device *dev, u8 link_mask,
				 bool reset_oneshot)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	u8 link_rate = priv->link_speed == 6 ? 0x22 : 0x11;
	int err;

	err = max96724_write_reg(dev, MAX96724_LINK_RATE_AB_ADDR, link_rate);
	err |= max96724_write_reg(dev, MAX96724_LINK_RATE_CD_ADDR, link_rate);
	err |= max96724_enable_error_channels(dev, link_mask);
	err |= max96724_write_reg(dev, MAX96724_LINK_EN_ADDR,
				  MAX96724_LINK_EN_BASE | link_mask);
	if (err || !reset_oneshot)
		return err;

	/*
	 * Match the MAX96712 link bring-up sequence: let the selected link
	 * settle before retraining it, then allow the one-shot reset to finish
	 * before polling lock or accessing the serializer.
	 */
	msleep(20);
	err = max96724_write_reg(dev, MAX96724_ONESHOT_ADDR, link_mask);
	if (!err)
		msleep(100);

	return err;
}

static int max96724_assign_serializer_addresses(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int reg_val;
	unsigned int link;
	int restore_err;
	int err = 0;

	err = max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR, 0x00);
	if (err)
		return err;

	for (link = 0; link < MAX96724_MAX_LINKS; link++) {
		u8 target_addr;

		if (!(priv->link_mask & BIT(link)))
			continue;

		target_addr = MAX96717_DEFAULT_ADDR + link;
		err = max96724_enable_links(dev, BIT(link), true);
		if (err)
			break;

		err = max96724_wait_link_lock(dev, link);
		if (err)
			break;

		err = max96724_read_slave_reg(priv, target_addr,
					     MAX96717_DEV_ADDR, &reg_val);
		if (!err && reg_val == target_addr << 1) {
			dev_dbg(dev, "serializer on link %c already at 0x%02x\n",
				'A' + link, target_addr);
			continue;
		}

		err = max96724_write_slave_reg(priv, MAX96717_DEFAULT_ADDR,
					      MAX96717_DEV_ADDR,
					      target_addr << 1);
		if (err)
			break;

		msleep(20);
		err = max96724_read_slave_reg(priv, target_addr,
					     MAX96717_DEV_ADDR, &reg_val);
		if (err || reg_val != target_addr << 1) {
			dev_err(dev,
				"failed to assign serializer on link %c address 0x%02x\n",
				'A' + link, target_addr);
			if (!err)
				err = -ENODEV;
			break;
		}

		dev_info(dev, "serializer on link %c assigned address 0x%02x\n",
			 'A' + link, target_addr);
	}

	restore_err = max96724_enable_links(dev, priv->link_mask, true);
	if (!err)
		err = restore_err;

	return err;
}

int max96724_recover_link(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv;
	struct max96724_source_ctx *source;
	unsigned int source_idx;
	unsigned int reg_val = 0;
	unsigned int link;
	u8 source_link_mask;
	u8 target_addr;
	int restore_err;
	int err;

	if (!dev || !s_dev)
		return -EINVAL;

	priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);

	err = max96724_get_sdev_idx_locked(priv, s_dev, &source_idx);
	if (err) {
		dev_err(dev, "%s: no source found for %s\n",
			__func__, dev_name(s_dev));
		goto out;
	}

	source = &priv->sources[source_idx];
	if (!source->g_ctx) {
		err = -EINVAL;
		goto out;
	}

	/*
	 * gmsl-link.h represents the link as a one-hot value (A=BIT(0),
	 * B=BIT(1)). Convert it to the MAX96724 link index and reject malformed
	 * source descriptions before touching the shared deserializer.
	 */
	source_link_mask = (u8)source->serdes_csi_link;
	if (!source_link_mask ||
	    (source_link_mask & (source_link_mask - 1)) ||
	    (source_link_mask & ~((u8)GENMASK(MAX96724_MAX_LINKS - 1, 0)))) {
		dev_err(dev, "%s: invalid source link 0x%x\n",
			__func__, source->serdes_csi_link);
		err = -EINVAL;
		goto out;
	}

	for (link = 0; link < MAX96724_MAX_LINKS; link++)
		if (source_link_mask & BIT(link))
			break;

	if (!(priv->link_mask & BIT(link))) {
		dev_err(dev, "%s: link %c is not enabled (mask 0x%x)\n",
			__func__, 'A' + link, priv->link_mask);
		err = -EINVAL;
		goto out;
	}

	target_addr = (u8)source->g_ctx->ser_reg;
	if (!target_addr || target_addr > 0x7f) {
		dev_err(dev, "%s: invalid serializer address 0x%x\n",
			__func__, source->g_ctx->ser_reg);
		err = -EINVAL;
		goto out;
	}

	dev_info(dev,
		 "%s: recovering GMSL link %c serializer at 0x%02x\n",
		 __func__, 'A' + link, target_addr);

	/*
	 * Isolate the affected link while accessing the serializer's default
	 * address. This avoids an address collision with a healthy link-A
	 * serializer when a serializer on link B/C/D returns to 0x40.
	 */
	err = max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR, 0x00);
	if (err)
		goto out;

	err = max96724_enable_links(dev, BIT(link), true);
	if (err)
		goto restore;

	err = max96724_wait_link_lock(dev, link);
	if (err)
		goto restore;

	/* Link lock may precede the remote I2C port becoming usable. */
	msleep(20);
	err = max96724_read_slave_reg(priv, target_addr,
					 MAX96717_DEV_ADDR, &reg_val);
	if (!err && reg_val == target_addr << 1) {
		dev_dbg(dev, "%s: serializer already responds at 0x%02x\n",
			__func__, target_addr);
		goto restore;
	}

	err = max96724_write_slave_reg(priv, MAX96717_DEFAULT_ADDR,
					  MAX96717_DEV_ADDR,
					  target_addr << 1);
	if (err)
		goto restore;

	msleep(20);
	err = max96724_read_slave_reg(priv, target_addr,
					 MAX96717_DEV_ADDR, &reg_val);
	if (err || reg_val != target_addr << 1) {
		dev_err(dev,
			"%s: serializer address restore failed on link %c (reg 0x%x, err %d)\n",
			__func__, 'A' + link, reg_val, err);
		if (!err)
			err = -ENODEV;
		goto restore;
	}

	dev_info(dev, "%s: serializer address 0x%02x restored on link %c\n",
		 __func__, target_addr, 'A' + link);

restore:
	/*
	 * Restore every configured link even on failure. The deserializer itself
	 * did not lose power, so its CSI mappings remain valid; only force the
	 * next stream start to re-arm the tunnel datapath.
	 */
	restore_err = max96724_enable_links(dev, priv->link_mask, true);
	if (!err)
		err = restore_err;
	restore_err = max96724_write_reg(dev, MAX96724_BACKTOP_EN_ADDR,
					 MAX96724_BACKTOP_CSIB_EN);
	if (!err)
		err = restore_err;
	if (!err)
		err = max96724_wait_link_lock(dev, link);

	priv->datapath_retriggered = false;
	priv->retriggered_pipe_mask = 0;

out:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_recover_link);

int max96724_setup_link(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err;

	mutex_lock(&priv->lock);
	err = max96724_get_sdev_idx_locked(priv, s_dev, NULL);
	if (err) {
		dev_err(dev, "%s: no sdev found\n", __func__);
		goto ret;
	}

	if (!priv->poc_enabled) {
		err = max96724_board_poc_enable(dev);
		if (err)
			goto ret;
		priv->poc_enabled = true;
	}

	if (!priv->link_setup) {
		err = max96724_assign_serializer_addresses(dev);
		if (err)
			goto ret;

		priv->link_setup = true;
	}

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_setup_link);

int max96724_setup_control(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct max96724_source_ctx *source;
	bool source_found;
	int err = 0;
	unsigned int i;
	unsigned int source_idx;

	mutex_lock(&priv->lock);
	err = max96724_get_sdev_idx_locked(priv, s_dev, &source_idx);
	if (err) {
		dev_err(dev, "%s: no sdev found\n", __func__);
		goto error;
	}
	source = &priv->sources[source_idx];

	if (!priv->link_setup) {
		dev_err(dev, "%s: invalid state\n", __func__);
		err = -EINVAL;
		goto error;
	}

	if (source->control_setup)
		goto error;

	if (!source->g_ctx) {
		dev_err(dev, "%s: source context is not live\n", __func__);
		err = -EINVAL;
		goto error;
	}

	source_found = source->g_ctx->serdev_found;

	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		err = max96724_write_reg(dev,
					 MAX96724_PIPE0_TUN_EN_ADDR + 0x40 * i,
					 MAX96724_TUN_EN);
		err |= max96724_write_reg(dev,
			MAX96724_PIPE0_TUN_DEST_ADDR + 0x40 * i,
			MAX96724_TUN_DEST_CTRL1);
		if (err)
			goto error;
	}

	err = max96724_write_reg(dev, MAX96724_SRAM_LCRC_ERR_ADDR, 0x00);
	if (err)
		goto error;

	source->control_setup = true;
	source->serdev_found = source_found;
	priv->num_src_found += source_found;
	priv->sdev_ref++;

error:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_setup_control);

int max96724_reset_control(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int i;
	int err = 0;

	mutex_lock(&priv->lock);
	err = max96724_get_sdev_idx_locked(priv, s_dev, &i);
	if (err) {
		dev_err(dev, "%s: no sdev found\n", __func__);
		goto ret;
	}

	if (!priv->sources[i].control_setup) {
		dev_info(dev, "%s: source is already in reset state\n", __func__);
		goto ret;
	}

	err = max96724_reset_control_locked(dev, priv, i);

ret:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_reset_control);

int max96724_sdev_register(struct device *dev, struct gmsl_link_ctx *g_ctx)
{
	struct max96724 *priv = NULL;
	unsigned int i;
	int err = 0;

	if (!dev || !g_ctx || !g_ctx->s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	if (priv->csi_mode == MAX96724_CSI_MODE_2X4) {
		if (g_ctx->csi_mode != GMSL_CSI_1X4_MODE &&
		    g_ctx->csi_mode != GMSL_CSI_2X4_MODE) {
			dev_err(dev, "%s: csi mode not supported\n", __func__);
			err = -EINVAL;
			goto done;
		}
	} else {
		dev_err(dev, "%s: only csi 2x4 mode is supported\n", __func__);
		err = -EINVAL;
		goto done;
	}

	for (i = 0; i < priv->num_src; i++) {
		struct max96724_source_ctx *source = &priv->sources[i];

		if (source->s_dev == g_ctx->s_dev) {
			if (source->g_ctx == g_ctx)
				goto done;

			dev_err(dev,
				"%s: source %s is already registered\n",
				__func__, dev_name(g_ctx->s_dev));
			err = -EBUSY;
			goto done;
		}

		if (g_ctx->serdes_csi_link == source->serdes_csi_link) {
			dev_err(dev,
				"%s: serdes csi link %u is already registered\n",
				__func__, g_ctx->serdes_csi_link);
			err = -EBUSY;
			goto done;
		}
		if (g_ctx->num_csi_lanes != source->num_csi_lanes) {
			dev_err(dev,
				"%s: csi num lanes mismatch\n", __func__);
			err = -EINVAL;
			goto done;
		}
	}

	if (priv->num_src >= priv->max_src ||
	    priv->num_src >= MAX96724_MAX_SOURCES) {
		dev_err(dev,
			"%s: MAX96724 inputs size exhausted\n", __func__);
		err = -ENOMEM;
		goto done;
	}

	memset(&priv->sources[priv->num_src], 0,
	       sizeof(priv->sources[priv->num_src]));
	priv->sources[priv->num_src].g_ctx = g_ctx;
	priv->sources[priv->num_src].s_dev = g_ctx->s_dev;
	priv->sources[priv->num_src].serdes_csi_link =
		g_ctx->serdes_csi_link;
	priv->sources[priv->num_src].num_csi_lanes =
		g_ctx->num_csi_lanes;
	priv->num_src++;

done:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_sdev_register);

int max96724_sdev_unregister(struct device *dev, struct device *s_dev)
{
	struct max96724 *priv = NULL;
	int err = 0;
	unsigned int i = 0;

	if (!dev || !s_dev) {
		dev_err(dev, "%s: invalid input params\n", __func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);
	mutex_lock(&priv->lock);

	if (priv->num_src == 0) {
		dev_dbg(dev, "%s: no source registered\n", __func__);
		goto done;
	}

	for (i = 0; i < priv->num_src; i++) {
		if (s_dev == priv->sources[i].s_dev)
			break;
	}

	if (i == priv->num_src) {
		dev_dbg(dev,
			"%s: requested device is not registered\n", __func__);
		goto done;
	}

	if (priv->sources[i].control_setup) {
		err = max96724_reset_control_locked(dev, priv, i);
		if (err)
			dev_warn(dev,
				 "%s: failed to reset source control: %d\n",
				 __func__, err);
	}

	for (; i + 1 < priv->num_src; i++)
		priv->sources[i] = priv->sources[i + 1];

	priv->num_src--;
	memset(&priv->sources[priv->num_src], 0,
	       sizeof(priv->sources[priv->num_src]));

done:
	mutex_unlock(&priv->lock);
	return err;
}
EXPORT_SYMBOL(max96724_sdev_unregister);

int max96724_get_available_pipe_id(struct device *dev, int vc_id)
{
	int i;
	int pipe_id = -ENOMEM;
	struct max96724 *priv = dev_get_drvdata(dev);

	if (vc_id < 0 || vc_id >= MAX96724_MAX_PIPES)
		return -EINVAL;

	mutex_lock(&priv->lock);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		if (i == vc_id && !priv->pipe[i].st_count) {
			priv->pipe[i].st_count++;
			pipe_id = i;
			break;
		}
	}
	mutex_unlock(&priv->lock);

	return pipe_id;
}
EXPORT_SYMBOL(max96724_get_available_pipe_id);

int max96724_release_pipe(struct device *dev, int pipe_id)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int i;

	if (pipe_id < 0 || pipe_id >= MAX96724_MAX_PIPES)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->pipe[pipe_id].st_count = 0;
	priv->retriggered_pipe_mask &= ~BIT(pipe_id);
	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		if (priv->pipe[i].st_count)
			break;
	}
	if (i == MAX96724_MAX_PIPES) {
		priv->datapath_retriggered = false;
		priv->retriggered_pipe_mask = 0;
	}
	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL(max96724_release_pipe);

void max96724_reset_oneshot(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	mutex_lock(&priv->lock);
	priv->datapath_retriggered = false;
	priv->retriggered_pipe_mask = 0;
	err = max96724_enable_links(dev, priv->link_mask, true);

	if (!err)
		err = max96724_configure_datapath(dev, false, true);
	msleep(200);
	if (!err)
		err = max96724_enable_error_channels(dev, priv->link_mask);
	if (err)
		dev_warn(dev, "failed to restore datapath: %d\n", err);
	else
		priv->datapath_retriggered = true;
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_reset_oneshot);

static int __max96724_set_pipe(struct device *dev, int pipe_id,
			       u8 data_type1, u8 data_type2, u32 vc_id)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	u8 en_mapping_num = 0x0F;
	u8 mapping_ctrl = MAX96724_ALL_MAP_CTRL1;
	unsigned int i;
	int err;
	struct reg_pair map_pipe_control[] = {
		{MAX96724_TX11_PIPE_X_EN_ADDR, 0x0F},
		{MAX96724_PIPE_X_SRC_0_MAP_ADDR, 0x1E},
		{MAX96724_PIPE_X_DST_0_MAP_ADDR, 0x1E},
		{MAX96724_PIPE_X_SRC_1_MAP_ADDR, 0x00},
		{MAX96724_PIPE_X_DST_1_MAP_ADDR, 0x00},
		{MAX96724_PIPE_X_SRC_2_MAP_ADDR, 0x01},
		{MAX96724_PIPE_X_DST_2_MAP_ADDR, 0x01},
		{MAX96724_PIPE_X_SRC_3_MAP_ADDR, 0x12},
		{MAX96724_PIPE_X_DST_3_MAP_ADDR, 0x12},
		{MAX96724_TX45_PIPE_X_DST_CTRL_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX46_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX47_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX48_PIPE_X_ADDR, MAX96724_ALL_MAP_CTRL1},
		{MAX96724_TX49_PIPE_X_ADDR, 0x00},
		{MAX96724_VID_RX0_P0_ADDR, MAX96724_VID_RX0_CFG_VAL},
		{MAX96724_VID_RX6_P0_ADDR, MAX96724_VID_RX6_CFG_VAL},
		{MAX96724_PIPE0_TUN_EN_ADDR, MAX96724_TUN_EN},
	};

	for (i = 0; i < 14; i++)
		map_pipe_control[i].addr += 0x40 * pipe_id;
	map_pipe_control[14].addr += MAX96724_VID_RX_STRIDE * pipe_id;
	map_pipe_control[15].addr += MAX96724_VID_RX_STRIDE * pipe_id;
	map_pipe_control[16].addr += 0x40 * pipe_id;

	if (!data_type2) {
		en_mapping_num = 0x07;
		mapping_ctrl = MAX96724_MAP3_CTRL1;
	}

	map_pipe_control[0].val = en_mapping_num;
	map_pipe_control[1].val = (vc_id << 6) | data_type1;
	map_pipe_control[2].val = (vc_id << 6) | data_type1;
	map_pipe_control[3].val = vc_id << 6;
	map_pipe_control[4].val = vc_id << 6;
	map_pipe_control[5].val = (vc_id << 6) | 0x01;
	map_pipe_control[6].val = (vc_id << 6) | 0x01;
	map_pipe_control[7].val = (vc_id << 6) | data_type2;
	map_pipe_control[8].val = (vc_id << 6) | data_type2;
	map_pipe_control[9].val = mapping_ctrl;

	err = max96724_set_registers(dev, map_pipe_control,
				     ARRAY_SIZE(map_pipe_control));
	if (!err) {
		priv->pipe[pipe_id].dt_type = data_type1;
		priv->pipe[pipe_id].dt_type2 = data_type2;
		priv->pipe[pipe_id].vc_id = vc_id;
		priv->pipe[pipe_id].map_configured = true;
	}

	return err;
}

void max96724_retrigger_datapath(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	unsigned int pipe_en = MAX96724_PIPE_EN_4;
	u8 active_mask = 0;
	u8 retrigger_mask;
	bool full_retrigger;
	unsigned int i;
	int err = 0;

	mutex_lock(&priv->lock);
	for (i = 0; i < MAX96724_MAX_PIPES; i++)
		if (priv->pipe[i].st_count)
			active_mask |= BIT(i);

	if (!active_mask) {
		dev_warn(dev, "datapath retrigger requested with no active pipes\n");
		goto out;
	}

	/*
	 * reset_oneshot() restores the static datapath registers, but with no
	 * active pipe it cannot arm the tunnel detector against real CSI data.
	 * The first camera stream therefore still needs a full retrigger.
	 */
	full_retrigger = !priv->datapath_retriggered ||
			 !priv->retriggered_pipe_mask;
	retrigger_mask = active_mask & ~priv->retriggered_pipe_mask;
	if (!full_retrigger && !retrigger_mask)
		goto out;

	if (!full_retrigger && priv->retriggered_pipe_mask) {
		/*
		 * All logical pipes share one tunnel detector. Once the first
		 * stream has armed it, set_pipe() is enough for streams joining
		 * an already-live tunnel; toggling PIPE_EN would disrupt them.
		 */
		priv->retriggered_pipe_mask |= retrigger_mask;
		goto out;
	}

	/*
	 * The camera's START status becomes visible before its first CSI packet
	 * reaches the deserializer. Give the tunnel detector time to see that
	 * input before cycling the affected pipe.
	 */
	msleep(20);

	if (full_retrigger) {
		retrigger_mask = active_mask;
		err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR, 0x00);
		msleep(20);
		if (!err)
			err = max96724_configure_datapath(dev, false, false);
	} else {
		if (regmap_read(priv->regmap, MAX96724_PIPE_EN_ADDR, &pipe_en))
			pipe_en = MAX96724_PIPE_EN_4;
		err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
					 pipe_en & ~retrigger_mask);
		msleep(20);
	}

	for (i = 0; i < MAX96724_MAX_PIPES; i++) {
		struct pipe_ctx *pipe = &priv->pipe[i];

		if (!(retrigger_mask & BIT(i)) || !pipe->st_count ||
		    !pipe->map_configured)
			continue;
		err |= __max96724_set_pipe(dev, i, pipe->dt_type,
					   pipe->dt_type2, pipe->vc_id);
	}

	if (!err) {
		if (full_retrigger)
			err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
						 MAX96724_PIPE_EN_4);
		else
			err = max96724_write_reg(dev, MAX96724_PIPE_EN_ADDR,
						 pipe_en | retrigger_mask);
	}

	if (err) {
		dev_warn(dev, "failed to retrigger CSI datapath: %d\n", err);
	} else {
		if (full_retrigger)
			priv->datapath_retriggered = true;
		priv->retriggered_pipe_mask |= retrigger_mask;
	}

out:
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(max96724_retrigger_datapath);

int max96724_init_settings(struct device *dev)
{
	int err = 0;
	int i;
	struct max96724 *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->lock);

	for (i = 0; i < MAX96724_MAX_PIPES; i++)
		err |= __max96724_set_pipe(dev, i, GMSL_CSI_DT_YUV422_8,
					   GMSL_CSI_DT_EMBED, i);
	if (!err)
		err = max96724_configure_datapath(dev, true, true);

	mutex_unlock(&priv->lock);

	if (err)
		return err;

	msleep(1500);
	max96724_reset_oneshot(dev);

	return err;
}
EXPORT_SYMBOL(max96724_init_settings);

int max96724_bind_ser_to_dser_pipe(struct device *dev, int dser_pipe_id,
				   int ser_pipe_id, u32 vc_id)
{
	/* In Tunnel Mode, pipe mapping is 1:1 (each camera → one pipe) */
	return 0;
}
EXPORT_SYMBOL(max96724_bind_ser_to_dser_pipe);

int max96724_set_pipe(struct device *dev, int pipe_id,
		      u8 data_type1, u8 data_type2, u32 vc_id)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	int err = 0;

	if (pipe_id < 0 || pipe_id >= MAX96724_MAX_PIPES ||
	    vc_id >= MAX96724_MAX_PIPES) {
		dev_info(dev, "%s: input pipe_id: %d exceeds max96724 max pipes\n",
			 __func__, pipe_id);
		return -EINVAL;
	}

	dev_dbg(dev, "%s pipe_id %d, data_type1 %u, data_type2 %u, vc_id %u\n",
		__func__, pipe_id, data_type1, data_type2, vc_id);

	mutex_lock(&priv->lock);

	err = __max96724_set_pipe(dev, pipe_id, data_type1, data_type2, vc_id);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_set_pipe);

/* Internal FSYNC generator, broadcast to serializers on GPIO channel 23. */
#define MAX96724_FSYNC_0_ADDR		0x04A0	/* mode / method / enable      */
#define MAX96724_FSYNC_PER_L_ADDR	0x04A5	/* FSYNC_PERIOD[7:0]           */
#define MAX96724_FSYNC_PER_M_ADDR	0x04A6	/* FSYNC_PERIOD[15:8]          */
#define MAX96724_FSYNC_PER_H_ADDR	0x04A7	/* FSYNC_PERIOD[23:16]         */
#define MAX96724_FSYNC_15_ADDR		0x04AF	/* link select / XTAL time base*/
#define MAX96724_FSYNC_TXID_ADDR	0x04B1	/* FSYNC_TX_ID[7:3]            */

#define MAX96724_FSYNC_XTAL_HZ		25000000U
#define MAX96724_FSYNC_FPS_DEFAULT	30U
#define MAX96724_FSYNC_PERIOD_MAX	0x00FFFFFFU
#define MAX96724_FSYNC_TX_CH		23
#define MAX96724_FSYNC_15_ALL		0xCF
#define MAX96724_FSYNC_TXID_VAL		(MAX96724_FSYNC_TX_CH << 3)
#define MAX96724_FSYNC_0_ENABLE		0x04
#define MAX96724_FSYNC_0_DISABLE	0x0C

int max96724_setup_fsync(struct device *dev, u32 fps)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct reg_pair map[6];
	u32 period;
	int err;

	if (!fps)
		fps = MAX96724_FSYNC_FPS_DEFAULT;

	/* Convert frame rate to a 24-bit period in 25MHz XTAL cycles. */
	period = MAX96724_FSYNC_XTAL_HZ / fps;
	if (!period || period > MAX96724_FSYNC_PERIOD_MAX) {
		dev_err(dev, "%s: fps %u out of range (period %u)\n",
			__func__, fps, period);
		return -EINVAL;
	}

	/* Link select + 25MHz XTAL time base (all 4 links). */
	map[0].addr = MAX96724_FSYNC_15_ADDR;
	map[0].val  = MAX96724_FSYNC_15_ALL;
	/* FSYNC period (24-bit, in XTAL cycles). */
	map[1].addr = MAX96724_FSYNC_PER_L_ADDR;
	map[1].val  = period & 0xFF;
	map[2].addr = MAX96724_FSYNC_PER_M_ADDR;
	map[2].val  = (period >> 8) & 0xFF;
	map[3].addr = MAX96724_FSYNC_PER_H_ADDR;
	map[3].val  = (period >> 16) & 0xFF;
	/* Transmit FSYNC over GMSL reverse channel 23 to all SERs. */
	map[4].addr = MAX96724_FSYNC_TXID_ADDR;
	map[4].val  = MAX96724_FSYNC_TXID_VAL;
	/* Enable internal FSYNC, manual mode (must be written last). */
	map[5].addr = MAX96724_FSYNC_0_ADDR;
	map[5].val  = MAX96724_FSYNC_0_ENABLE;

	mutex_lock(&priv->lock);

	err = max96724_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: FSYNC setup failed (%d)\n", __func__, err);
	else
		dev_info(dev,
			 "%s: internal FSYNC enabled (%u Hz, period %u, broadcast ch%u to all links)\n",
			 __func__, fps, period, MAX96724_FSYNC_TX_CH);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_setup_fsync);

int max96724_disable_fsync(struct device *dev)
{
	struct max96724 *priv = dev_get_drvdata(dev);
	struct reg_pair map[] = {
		{MAX96724_FSYNC_0_ADDR, MAX96724_FSYNC_0_DISABLE},
	};
	int err;

	mutex_lock(&priv->lock);

	err = max96724_set_registers(dev, map, ARRAY_SIZE(map));
	if (err)
		dev_err(dev, "%s: FSYNC disable failed (%d)\n", __func__, err);
	else
		dev_dbg(dev, "%s: internal FSYNC disabled\n", __func__);

	mutex_unlock(&priv->lock);

	return err;
}
EXPORT_SYMBOL(max96724_disable_fsync);

static const struct of_device_id max96724_of_match[] = {
	{ .compatible = "adi,max96724", },
	{ },
};
MODULE_DEVICE_TABLE(of, max96724_of_match);

static int max96724_parse_dt(struct max96724 *priv,
			     struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	int err = 0;
	const char *str_value;
	u32 value;
	const struct of_device_id *match;

	if (!node)
		return -EINVAL;

	match = of_match_device(max96724_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return -EFAULT;
	}

	/* CSI mode: "2x4" or "4x2" */
	err = of_property_read_string(node, "csi-mode", &str_value);
	if (err < 0) {
		dev_err(&client->dev, "csi-mode property not found\n");
		return err;
	}

	if (!strcmp(str_value, "2x4")) {
		priv->csi_mode = MAX96724_CSI_MODE_2X4;
		priv->lane_mp1 = MAX96724_LANE_MAP1_2X4;
	} else {
		dev_err(&client->dev, "invalid csi mode\n");
		return -EINVAL;
	}

	/* Max sources */
	err = of_property_read_u32(node, "max-src", &value);
	if (err < 0) {
		dev_err(&client->dev, "No max-src info\n");
		return err;
	}
	priv->max_src = value;
	if (!priv->max_src || priv->max_src > MAX96724_MAX_SOURCES) {
		dev_err(&client->dev, "invalid max-src %u\n", priv->max_src);
		return -EINVAL;
	}

	err = of_property_read_u32(node, "link-mask", &value);
	if (err < 0) {
		value = BIT(0);
		dev_warn(&client->dev,
			 "link-mask not specified, defaulting to 0x1\n");
	} else if (!value || value > GENMASK(MAX96724_MAX_LINKS - 1, 0)) {
		dev_err(&client->dev, "invalid link-mask 0x%x\n", value);
		return -EINVAL;
	}
	priv->link_mask = value;

	priv->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						   GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset GPIO\n");

	/* GMSL link speed from DT: 3 or 6 (Gbps) */
	err = of_property_read_u32(node, "gmsl-link-speed", &value);
	if (err < 0) {
		/* Default to 3 Gbps if not specified */
		priv->link_speed = 3;
		dev_info(&client->dev,
			 "gmsl-link-speed not specified, defaulting to 3 Gbps\n");
	} else {
		if (value != 3 && value != 6) {
			dev_err(&client->dev,
				"invalid gmsl-link-speed %d (must be 3 or 6)\n",
				value);
			return -EINVAL;
		}
		priv->link_speed = value;
	}

	priv->vdd_cam_1v2 = devm_regulator_get_optional(&client->dev,
							"vdd_cam_1v2");
	if (IS_ERR(priv->vdd_cam_1v2)) {
		err = PTR_ERR(priv->vdd_cam_1v2);
		priv->vdd_cam_1v2 = NULL;
		if (err != -ENODEV)
			return dev_err_probe(&client->dev, err,
					     "failed to get vdd_cam_1v2\n");
	}

	return 0;
}

static struct regmap_config max96724_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int max96724_probe(struct i2c_client *client)
#else
static int max96724_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
#endif
{
	struct max96724 *priv;
	int err = 0;

	dev_info(&client->dev,
		 "[MAX96724]: probing Quad GMSL2 Deserializer (Tunnel Mode)\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(priv->i2c_client,
					    &max96724_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	err = max96724_parse_dt(priv, client);
	if (err) {
		dev_err(&client->dev, "unable to parse dt\n");
		return -EFAULT;
	}

	max96724_pipes_reset(priv);

	mutex_init(&priv->lock);

	dev_set_drvdata(&client->dev, priv);

	dev_info(&client->dev,
		 "%s: success (link_speed=%d Gbps, max_src=%d, link_mask=0x%x)\n",
		 __func__, priv->link_speed, priv->max_src, priv->link_mask);

	return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
static int max96724_remove(struct i2c_client *client)
#else
static void max96724_remove(struct i2c_client *client)
#endif
{
	struct max96724 *priv = dev_get_drvdata(&client->dev);

	mutex_destroy(&priv->lock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
	return 0;
#endif
}

static const struct i2c_device_id max96724_id[] = {
	{ "max96724", 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, max96724_id);

static struct i2c_driver max96724_i2c_driver = {
	.driver = {
		.name = "max96724",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(max96724_of_match),
	},
	.probe = max96724_probe,
	.remove = max96724_remove,
	.id_table = max96724_id,
};

module_i2c_driver(max96724_i2c_driver);

MODULE_DESCRIPTION("Quad GMSL2 Deserializer driver max96724 (Tunnel Mode)");
MODULE_AUTHOR("RealSense AI");
MODULE_LICENSE("GPL v2");

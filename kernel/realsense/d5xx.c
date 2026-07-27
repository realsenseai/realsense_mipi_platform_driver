// SPDX-License-Identifier: GPL-2.0
/*
 * RealSense D5XX camera driver
 *
 * Copyright (c) 2026, RealSense, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swab.h>
#include <linux/videodev2.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <asm/unaligned.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mediabus.h>

#ifdef CONFIG_VIDEO_D5XX_SERDES
#include <media/max96717.h>
#include <media/max96724.h>

/* Deserializer interface structure for abstraction */
struct dser_interface {
	/* Pipeline management */
	int (*get_available_pipe_id)(struct device *dev, int vc_id);
	int (*get_ser_pipe_id)(struct device *dev, int dser_pipe_id, int vc_id);
	int (*bind_ser_to_dser_pipe)(struct device *dev, int dser_pipe_id, int ser_pipe_id, u32 vc_id);

	int (*set_pipe)(struct device *dev, int pipe_id, u8 data_type1, u8 data_type2, u32 vc_id);
	int (*release_pipe)(struct device *dev, int pipe_id);
	void (*reset_oneshot)(struct device *dev);
	void (*retrigger_datapath)(struct device *dev);
	int (*get_active_pipe_count)(struct device *dev);
	int (*get_link_locked)(struct device *dev);
	
	/* Setup and control */
	int (*setup_link)(struct device *dev, struct device *s_dev);
	int (*setup_control)(struct device *dev, struct device *s_dev);
	int (*reset_control)(struct device *dev, struct device *s_dev);

	/* Frame sync (multi-camera).  fps is the V4L2-negotiated frame rate. */
	int (*setup_fsync)(struct device *dev, u32 fps);
	int (*disable_fsync)(struct device *dev);
	
	/* Device registration */
	int (*sdev_register)(struct device *dev, struct gmsl_link_ctx *g_ctx);
	int (*sdev_unregister)(struct device *dev, struct device *s_dev);
	
	/* Power management */
	int (*power_on)(struct device *dev);
	void (*power_off)(struct device *dev);
	int (*init_settings)(struct device *dev);

	/* Identification */
	const char *name;
};

#else
#include <media/gmsl-link.h>
#define GMSL_CSI_DT_YUV422_8        0x1E
#define GMSL_CSI_DT_RGB_888         0x24
#define GMSL_CSI_DT_RAW_8           0x2A
#define GMSL_CSI_DT_EMBED           0x12
#endif

#ifndef GMSL_CSI_DT_RAW_10
#define GMSL_CSI_DT_RAW_10          0x2B
#endif

#ifndef GMSL_CSI_DT_RAW_16
#define GMSL_CSI_DT_RAW_16          0x2E
#endif

#ifndef GMSL_CSI_DT_USER_1
#define GMSL_CSI_DT_USER_1          0x30
#endif

#ifndef MEDIA_BUS_FMT_RS_SGRBG10_1X16
#define MEDIA_BUS_FMT_RS_SGRBG10_1X16 0x5003
#endif

#ifndef MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16
#define MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16 0x5004
#endif

/*
 * D5x RGB ISYS stores GRBG10P as 50 little-endian 10-bit pixels per
 * 64-byte cache line. This is neither standard CSI RAW10, V4L2 IPU3
 * SGRBG10, nor 10-bit-in-16 BA10.
 */
#ifndef MEDIA_BUS_FMT_RS_SGRBG10P_1X10
#define MEDIA_BUS_FMT_RS_SGRBG10P_1X10 0x5002
#endif

#ifndef V4L2_PIX_FMT_RS_SGRBG10P
#define V4L2_PIX_FMT_RS_SGRBG10P v4l2_fourcc('G', 'R', '0', 'P')
#endif

#ifndef V4L2_PIX_FMT_SGRBG10P
#define V4L2_PIX_FMT_SGRBG10P v4l2_fourcc('p', 'g', 'A', 'A')
#endif

/*
 * D5X_BYPASS_CAMERA_I2C: Bypass all I2C communication to the D5XX camera
 * sensor while keeping GMSL SERDES (MAX96717/MAX96724) fully operational.
 * Use this when the camera is already streaming MIPI data independently
 * and only the SERDES link + V4L2 registration is needed on the Orin side.
 */
#define D5X_BYPASS_CAMERA_I2C 0

//#define D5X_DRIVER_NAME "DS5 RealSense camera driver"
#define D5X_DRIVER_NAME "d5xx"
#define D5X_DRIVER_NAME_AWG "d5xx-awg"
#define D5X_DRIVER_NAME_ASR "d5xx-asr"
/* Keep legacy d4xx sysfs names so librealsense can discover the correct node. */
#define D5X_DRIVER_NAME_CLASS "d4xx-class"
#define D5X_DRIVER_NAME_DFU "d4xx-dfu"
/* Keep legacy DS5 mux naming so the installed rs-enum/udev rules still match. */
#define D5X_DRIVER_NAME_MUX "DS5 mux"
/* Keep legacy D4XX sensor entity naming so rs-enum media graph parsing still matches. */
#define D5X_DRIVER_NAME_SENSOR "D4XX"

#define D5X_MIPI_SUPPORT_LINES		0x0300
#define D5X_MIPI_SUPPORT_PHY		0x0304
#define D5X_MIPI_DATARATE_MIN		0x0308
#define D5X_MIPI_DATARATE_MAX		0x030A
#define D5X_FW_VERSION				0x030C
#define D5X_FW_BUILD				0x030E
#define D5X_DEVICE_TYPE				0x0310
#define D5X_DEVICE_TYPE_D58X		9
#define D5X_DEVICE_TYPE_UNKNOWN		0

#define D5X_MIPI_LANE_NUMS			0x0400
#define D5X_MIPI_LANE_DATARATE		0x0402
#define D5X_MIPI_CONF_STATUS		0x0500

#define D5X_START_STOP_STREAM		0x1000
#define D5X_DEPTH_STREAM_STATUS		0x1004
#define D5X_RGB_STREAM_STATUS		0x1008
#define D5X_IMU_STREAM_STATUS		0x100C
#define D5X_IR_STREAM_STATUS		0x1014

#define D5X_STREAM_DEPTH			0x0
#define D5X_STREAM_RGB				0x1
#define D5X_STREAM_IMU				0x2
#define D5X_STREAM_IR				0x4
#define D5X_STREAM_STOP				0x100
#define D5X_STREAM_START			0x200
#define D5X_STREAM_IDLE				0x1
#define D5X_STREAM_STREAMING		0x2

#define D5X_DEPTH_STREAM_DT		 	0x4000
#define D5X_DEPTH_STREAM_MD		 	0x4002
#define D5X_DEPTH_RES_WIDTH		 	0x4004
#define D5X_DEPTH_RES_HEIGHT	 	0x4008
#define D5X_DEPTH_FPS			 	0x400C
#define D5X_DEPTH_OVERRIDE		 	0x401C
#define D5X_DEPTH_CONTROL_STATUS 	0x401E

#define D5X_RGB_STREAM_DT			0x4020
#define D5X_RGB_STREAM_MD			0x4022
#define D5X_RGB_RES_WIDTH			0x4024
#define D5X_RGB_RES_HEIGHT			0x4028
#define D5X_RGB_FPS					0x402C
#define D5X_RGB_CONTROL_STATUS 		0x402E

#define D5X_IMU_STREAM_DT			0x4040
#define D5X_IMU_STREAM_MD			0x4042
#define D5X_IMU_RES_WIDTH			0x4044
#define D5X_IMU_RES_HEIGHT			0x4048
#define D5X_IMU_FPS					0x404C
#define D5X_IMU_CONTROL_STATUS 		0x404E

#define D5X_IR_STREAM_DT			0x4080
#define D5X_IR_STREAM_MD			0x4082
#define D5X_IR_RES_WIDTH			0x4084
#define D5X_IR_RES_HEIGHT			0x4088
#define D5X_IR_FPS					0x408C
#define D5X_IR_OVERRIDE				0x409C
#define D5X_IR_CONTROL_STATUS 		0x409E

#define D5X_DEPTH_CONTROL_BASE		0x4100
#define D5X_RGB_CONTROL_BASE		0x4200
#define D5X_MANUAL_EXPOSURE_LSB		0x0000
#define D5X_MANUAL_EXPOSURE_MSB		0x0002
#define D5X_MANUAL_GAIN				0x0004
#define D5X_LASER_POWER				0x0008
#define D5X_AUTO_EXPOSURE_MODE		0x000C
#define D5X_EXPOSURE_ROI_TOP		0x0010
#define D5X_EXPOSURE_ROI_LEFT		0x0014
#define D5X_EXPOSURE_ROI_BOTTOM		0x0018
#define D5X_EXPOSURE_ROI_RIGHT		0x001C
#define D5X_MANUAL_LASER_POWER		0x0024

#define D5X_DEPTH_CONFIG_STATUS		0x4800
#define D5X_RGB_CONFIG_STATUS		0x4802
#define D5X_IMU_CONFIG_STATUS		0x4804
#define D5X_IR_CONFIG_STATUS		0x4808

#define D5X_STATUS_STREAMING		0x1
#define D5X_STATUS_INVALID_DT		0x2
#define D5X_STATUS_INVALID_RES		0x4
#define D5X_STATUS_INVALID_FPS		0x8
#define D5X_STATUS_VALID_MASK		0xf
#define D5X_STATUS_UNAVAILABLE		0xffff

#define MIPI_LANE_RATE				1300
#define D5X_CSI_METADATA_MAGIC		0x484B524D
#define D5X_CSI_METADATA_HEADER_SIZE	20
#define D5X_CSI_METADATA_PAYLOAD_OFFSET	8
#define D5X_CSI_METADATA_MAX_WC		4096

#define MAX_DEPTH_EXP				200000
#define MAX_RGB_EXP					10000
#define DEF_DEPTH_EXP				33000
#define DEF_RGB_EXP					1660

enum d5x_mux_pad {
	D5X_MUX_PAD_EXTERNAL,
	D5X_MUX_PAD_DEPTH,
	D5X_MUX_PAD_RGB,
	D5X_MUX_PAD_IR,
	D5X_MUX_PAD_IMU,
	D5X_MUX_PAD_COUNT,
};

#define D5X_N_CONTROLS		 		8

#define D5X_MAX_STREAMS				4

#define PIPE_NOT_CONFIGURED			-1

#define DFU_WAIT_RET_LEN 			6

#define D5X_START_POLL_TIME			10
#define D5X_START_POLL_MAX_TIME			40
#define D5X_START_MAX_TIME			2000
#define D5X_STOP_MAX_TIME			7000
#define D5X_START_MAX_COUNT	(D5X_START_MAX_TIME / D5X_START_POLL_TIME)
#define D5X_DSER_REARM_COOLDOWN_MS		1800
#define MAX_D5X_CONFIG_RETRIES		5

/* I2C retry configuration */
#define D5X_I2C_RETRY_COUNT			5
#define D5X_I2C_RETRY_DELAY_US		5000

/* DFU definition section */
#define DFU_MAGIC_NUMBER "/0x01/0x02/0x03/0x04"
#define DFU_BLOCK_SIZE 1024
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
#define DFU_I2C_STANDARD_MODE		100000
#define DFU_I2C_FAST_MODE			400000
#define DFU_I2C_BUS_CLK_RATE		DFU_I2C_FAST_MODE
#endif
#define d5x_read_with_check(state, addr, val) {\
	if (d5x_read(state, addr, val))	\
		return -EINVAL; }
#define d5x_raw_read_with_check(state, addr, buf, size)	{\
	if (d5x_raw_read(state, addr, buf, size))	\
		return -EINVAL; }
#define d5x_write_with_check(state, addr, val) {\
	if (d5x_write(state, addr, val))	\
		return -EINVAL; }
#define d5x_raw_write_with_check(state, addr, buf, size) {\
	if (d5x_raw_write(state, addr, buf, size)) \
		return -EINVAL; }

enum dfu_fw_state {
	appIDLE                = 0x0000,
	appDETACH              = 0x0001,
	dfuIDLE                = 0x0002,
	dfuDNLOAD_SYNC         = 0x0003,
	dfuDNBUSY              = 0x0004,
	dfuDNLOAD_IDLE         = 0x0005,
	dfuMANIFEST_SYNC       = 0x0006,
	dfuMANIFEST            = 0x0007,
	dfuMANIFEST_WAIT_RESET = 0x0008,
	dfuUPLOAD_IDLE         = 0x0009,
	dfuERROR               = 0x000a
};

enum dfu_state {
	D5X_DFU_IDLE = 0,
	D5X_DFU_RECOVERY,
	D5X_DFU_OPEN,
	D5X_DFU_IN_PROGRESS,
	D5X_DFU_DONE,
	D5X_DFU_ERROR
} d5x_dfu_state_t;

struct hwm_cmd {
	u16 header;
	u16 magic_word;
	u32 opcode;
	u32 param1;
	u32 param2;
	u32 param3;
	u32 param4;
	unsigned char Data[];
};

static const struct hwm_cmd cmd_switch_to_dfu = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x1e,
	.param1 = 0x01,
};

enum table_id {
	COEF_CALIBRATION_ID = 0x19,
	DEPTH_CALIBRATION_ID = 0x1f,
	RGB_CALIBRATION_ID = 0x20,
	IMU_CALIBRATION_ID = 0x22
} d5x_table_id_t;

static const struct hwm_cmd get_calib_data = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x15,
	.param1 = 0x00,	//table_id
};

static const struct hwm_cmd set_calib_data = {
	.header = 0x0114,
	.magic_word = 0xCDAB,
	.opcode = 0x62,
	.param1 = 0x00,	//table_id
	.param2 = 0x02,	//region
};

static const struct hwm_cmd gvd = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x10,
};

static const struct hwm_cmd set_ae_roi = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x44,
};

static const struct hwm_cmd get_ae_roi = {
	.header = 0x014,
	.magic_word = 0xCDAB,
	.opcode = 0x45,
};

static const struct hwm_cmd set_ae_setpoint = {
	.header = 0x18,
	.magic_word = 0xCDAB,
	.opcode = 0x2B,
	.param1 = 0xa, // AE control
};

static const struct hwm_cmd get_ae_setpoint = {
	.header = 0x014,
	.magic_word = 0xCDAB,
	.opcode = 0x2C,
	.param1 = 0xa, // AE control
	.param2 = 0, // get current
};

static const struct hwm_cmd erb = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x17,
};

static const struct hwm_cmd ewb = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x18,
};

static const struct hwm_cmd cmd_hw_reset = {
	.header = 0x14,
	.magic_word = 0xCDAB,
	.opcode = 0x20,  /* HW reset opcode */
};

#define D5X_HWMC_OPCODE_SET_CAM_SYNC	0x69
#define D5X_HWMC_OPCODE_GET_CAM_SYNC	0x6A

static const struct hwm_cmd log_prepare = {
	.header = 0x014,
	.magic_word = 0xCDAB,
	.opcode = 0xf,
	.param1 = 0x400, .param2 = 0, .param3 = 0, .param4 = 0,
};

#define D5X_HWMC_OPCODE_UAMG		0x30

struct __fw_status {
	uint32_t	spare1;
	uint32_t	FW_lastVersion;
	uint32_t	FW_highestVersion;
	uint16_t	FW_DownloadStatus;
	uint16_t	DFU_isLocked;
	uint16_t	DFU_version;
	uint8_t		ivcamSerialNum[8];
	uint8_t		spare2[42];
};

/*************************/

struct d5x_ctrls {
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl_handler handler_depth;
	struct v4l2_ctrl_handler handler_rgb;
	struct v4l2_ctrl_handler handler_y8;
	struct v4l2_ctrl_handler handler_imu;
	struct {
		struct v4l2_ctrl *log;
		struct v4l2_ctrl *fw_version;
		struct v4l2_ctrl *device_type;
		struct v4l2_ctrl *gvd;
		struct v4l2_ctrl *get_depth_calib;
		struct v4l2_ctrl *set_depth_calib;
		struct v4l2_ctrl *get_coeff_calib;
		struct v4l2_ctrl *set_coeff_calib;
		struct v4l2_ctrl *ae_roi_get;
		struct v4l2_ctrl *ae_roi_set;
		struct v4l2_ctrl *ae_setpoint_get;
		struct v4l2_ctrl *ae_setpoint_set;
		struct v4l2_ctrl *erb;
		struct v4l2_ctrl *ewb;
		struct v4l2_ctrl *hwmc;
		struct v4l2_ctrl *hwmc_rw;
		struct v4l2_ctrl *laser_power;
		struct v4l2_ctrl *manual_laser_power;
		struct v4l2_ctrl *auto_exp;
		struct v4l2_ctrl *exposure;
		/* In D5xx, manual gain only works with manual exposure. */
		struct v4l2_ctrl *gain;
		struct v4l2_ctrl *link_freq;
		struct v4l2_ctrl *query_sub_stream;
		struct v4l2_ctrl *set_sub_stream;
		struct v4l2_ctrl *sync_mode;
	};
};

struct d5x_resolution {
	u16 width;
	u16 height;
	u8 n_framerates;
	const u16 *framerates;
};

struct d5x_format {
	unsigned int n_resolutions;
	const struct d5x_resolution *resolutions;
	u32 mbus_code;
	u8 data_type;
};

struct d5x_sensor {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt format;
	u16 mux_pad;
	struct {
		const struct d5x_format *format;
		const struct d5x_resolution *resolution;
		u16 framerate;
	} config;
	bool streaming;
	const struct d5x_format *formats;
	unsigned int n_formats;
	int pipe_id;
	u16 pipe_data_type1;
	u16 pipe_data_type2;
	u32 pipe_vc_id;
	unsigned int pipe_reapply_gen;
	u16 cached_dt_value;
	u16 cached_md_value;
	u16 cached_override_value;
	u16 cached_fps_value;
	u16 cached_width_value;
	u16 cached_height_value;
};

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
#include <media/camera_common.h>
#define d5x_mux_subdev camera_common_data
#else
struct d5x_mux_subdev {
	struct v4l2_subdev subdev;
};
#endif

struct d5x_variant {
	const struct d5x_format *formats;
	unsigned int n_formats;
};

struct d5x_dfu_dev {
	struct cdev d5x_cdev;
	struct class *d5x_class;
	int device_open_count;
	enum dfu_state dfu_state_flag;
	unsigned char *dfu_msg;
	u16 msg_write_once;
	u32 bus_clk_rate;
};

enum {
	D5X_DS5U,
	D5X_ASR,
	D5X_AWG,
};

struct d5x {
	struct { struct d5x_sensor sensor; } depth;
	struct { struct d5x_sensor sensor; } ir;
	struct { struct d5x_sensor sensor; } rgb;
	struct { struct d5x_sensor sensor; } imu;
	struct {
		struct d5x_mux_subdev sd;
		struct media_pad pads[D5X_MUX_PAD_COUNT];
		struct d5x_sensor *last_set;
	} mux;
	struct d5x_ctrls ctrls;
	struct d5x_dfu_dev dfu_dev;
	bool power;
	struct i2c_client *client;
	/* All below pointers are used for writing, cannot be const */
	struct mutex lock;
	struct regmap *regmap;
	struct regulator *vcc;
	const struct d5x_variant *variant;
	int is_depth, is_y8, is_rgb, is_imu;
	bool metadata_enabled;
	int aggregated;
	int reset_ref_d5x;
	int reset_ref_dser;
	u16 fw_version;
	u16 fw_build;
	u16 control_base;
	u16 control_status_reg;
#ifdef CONFIG_VIDEO_D5XX_SERDES
	struct gmsl_link_ctx g_ctx;
	struct device *ser_dev;
	struct device *dser_dev;
	struct i2c_client *ser_i2c;
	struct i2c_client *dser_i2c;
	const struct dser_interface *dser_ops;
	bool serdes_primary; /* true for the instance that ran SERDES setup */
	bool ser_control_setup;
	bool dser_control_setup;
#endif
	struct d5x_dev *d5x_dev; /* D5xx device state */
};

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
static void d5x_swap_raw16_to_ba10(void *data, size_t bytesused)
{
	u8 *cursor = data;

	while (bytesused >= sizeof(u64)) {
		u64 value = get_unaligned_le64(cursor);

		value = ((value & 0x00ff00ff00ff00ffULL) << 8) |
			((value & 0xff00ff00ff00ff00ULL) >> 8);
		put_unaligned_le64(value, cursor);
		cursor += sizeof(value);
		bytesused -= sizeof(value);
	}
	if (bytesused >= sizeof(u32)) {
		u32 value = get_unaligned_le32(cursor);

		put_unaligned_le32(swahb32(value), cursor);
		cursor += sizeof(value);
		bytesused -= sizeof(value);
	}
	if (bytesused)
		put_unaligned_le16(swab16(get_unaligned_le16(cursor)), cursor);
}

static int d5x_pack_raw16_to_sgrbg10p(void *data, size_t bytesused,
				      u32 width, u32 height,
				      u32 bytesperline)
{
	u8 *frame = data;
	u32 y;

	if (WARN_ON_ONCE(width & 3U) ||
	    WARN_ON_ONCE(bytesperline < width * sizeof(u16)) ||
	    WARN_ON_ONCE(bytesused < (size_t)bytesperline * height))
		return -EINVAL;

	for (y = 0; y < height; y++) {
		const u8 *src = frame + (size_t)y * bytesperline;
		u8 *dst = frame + (size_t)y * bytesperline;
		u32 x;

		/*
		 * Each 8-byte source group is loaded before its 5-byte output is
		 * stored. The destination cursor never advances beyond the source
		 * cursor, so the forward transform cannot overwrite unread input.
		 */
		for (x = 0; x < width; x += 4) {
			u64 raw = get_unaligned_le64(src);
			u64 lanes =
				((raw & 0x00ff00ff00ff00ffULL) << 8) |
				((raw & 0xff00ff00ff00ff00ULL) >> 8);
			u32 high =
				((lanes >> 2) & 0x000000ff) |
				((lanes >> 10) & 0x0000ff00) |
				((lanes >> 18) & 0x00ff0000) |
				((lanes >> 26) & 0xff000000);

			put_unaligned_le32(high, dst);
			dst[4] = (lanes & 0x03) |
				 ((lanes >> 14) & 0x0c) |
				 ((lanes >> 28) & 0x30) |
				 ((lanes >> 42) & 0xc0);
			src += 4 * sizeof(u16);
			dst += 5;
		}
	}

	return 0;
}

static int d5x_postprocess_csi_frame(struct camera_common_data *s_data,
				    void *data, size_t bytesused,
				    u32 mbus_code)
{
	struct d5x *state = container_of(s_data, struct d5x, mux.sd);

	if (mbus_code != MEDIA_BUS_FMT_RS_SGRBG10_1X16 &&
	    mbus_code != MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16)
		return 0;
	if (!data || !bytesused || WARN_ON_ONCE(bytesused & 1U))
		return -EFAULT;

	switch (mbus_code) {
	case MEDIA_BUS_FMT_RS_SGRBG10_1X16:
		d5x_swap_raw16_to_ba10(data, bytesused);
		return 0;
	case MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16:
		return d5x_pack_raw16_to_sgrbg10p(
			data, bytesused,
			state->rgb.sensor.format.width,
			state->rgb.sensor.format.height,
			state->rgb.sensor.format.width * sizeof(u16));
	default:
		return 0;
	}
}

static const struct tegra_frame_postprocess_ops d5x_frame_postprocess_ops = {
	.process = d5x_postprocess_csi_frame,
};
#endif

struct d5x_dev {
	struct mutex lock;
	struct mutex stream_ctrl_lock;

	/*
	* Per-camera reset generation counter.
	* Incremented whenever the camera undergoes a HW reset,
	*  either from its own instance or a sibling's instance.
	* Each instance stores the last seen generation count
	*  and compares it on each access to detect if a reset has occurred
	*  and invalidates its own state if so.
	*/
	atomic_t reset_gen;

	/* 
	* Cached device type from post-reset device-type polling.
	* During probe the first instance resets the camera, causing D5X_DEVICE_TYPE
	* to temporarily return 0. The reset path polls until the register is valid
	* and stores the result here so that all four probe instances (and any
	* post-reset code path) can use the confirmed value even if the register
	* read still returns 0.
	*/
	u16 cached_device_type;

	/* 
	* Timestamp (jiffies) of last completed HW reset.
	* Used to enforce D5X_HW_RESET_COOLDOWN_MS between consecutive resets
	* and prevent GMSL link degradation from rapid reset cycles.
	*/
	unsigned long last_reset_jiffies;

#ifdef CONFIG_VIDEO_D5XX_SERDES
 	/* Pointer to the deserializer dev */
	struct dser_control *dser_control;
#endif

	/* Pointer to the primary D5xx state */
	struct d5x *d5x_primary;

	int sync_mode;
	bool depth_streaming;
	bool ir_streaming;
	bool rgb_streaming;
	bool imu_streaming;
};

#ifdef CONFIG_VIDEO_D5XX_SERDES
static DEFINE_MUTEX(serdes_lock__);
static bool d5x_slots_inited;

#define MAX_DSER_NUM 4
struct dser_control {
	struct mutex lock;

	/*
	* Per-deserializer reset generation counter.
	* Replaces the old global d5x_reset_gen for SERDES builds so that
	* resetting camera A does not force camera B (on a different deserializer)
	* to invalidate its state.  Cameras sharing the same deserializer still
	* see each other's resets through the shared counter.
	*/
	atomic_t reset_gen;
	struct device *dser_dev;
	bool datapath_armed;
	u8 datapath_pipe_mask;
	bool post_start_kick_pending;
	unsigned int rearm_gen;
	unsigned long last_idle_jiffies;
};
static struct dser_control dser_inited[MAX_DSER_NUM];

#define MAX_D5X_NUM (MAX_DSER_NUM * 4) /* Up to four D5xx cameras per deserializer. */
static struct d5x_dev d5x_inited[MAX_D5X_NUM];

static void d5x_init_global_slots_once(void)
{
	int i;

	if (d5x_slots_inited) {
		return;
	}

	for (i = 0; i < MAX_D5X_NUM; i++)
		mutex_init(&d5x_inited[i].lock);
	for (i = 0; i < MAX_D5X_NUM; i++)
		mutex_init(&d5x_inited[i].stream_ctrl_lock);

	for (i = 0; i < MAX_DSER_NUM; i++)
		mutex_init(&dser_inited[i].lock);

	d5x_slots_inited = true;
}

static inline atomic_t *d5x_get_reset_gen(struct d5x *state)
{
	return &state->d5x_dev->reset_gen;
}

static inline atomic_t *dser_get_reset_gen(struct d5x *state)
{
	return &state->d5x_dev->dser_control->reset_gen;
}

static bool d5x_dser_datapath_armed(struct d5x *state)
{
	bool armed;
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	armed = ctrl->datapath_armed;
	mutex_unlock(&ctrl->lock);

	return armed;
}

static void d5x_set_dser_datapath_armed(struct d5x *state, bool armed)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	ctrl->datapath_armed = armed;
	if (!armed)
		ctrl->datapath_pipe_mask = 0;
	mutex_unlock(&ctrl->lock);
}

static bool d5x_dser_datapath_matches(struct d5x *state, u8 pipe_mask)
{
	bool matches;
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	matches = ctrl->datapath_armed &&
		  ctrl->datapath_pipe_mask == pipe_mask;
	mutex_unlock(&ctrl->lock);

	return matches;
}

static void d5x_set_dser_datapath_pipe_mask(struct d5x *state, u8 pipe_mask)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	ctrl->datapath_armed = true;
	ctrl->datapath_pipe_mask = pipe_mask;
	mutex_unlock(&ctrl->lock);
}

static bool d5x_dser_post_start_kick_pending(struct d5x *state)
{
	bool pending;
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	pending = ctrl->post_start_kick_pending;
	mutex_unlock(&ctrl->lock);

	return pending;
}

static void d5x_set_dser_post_start_kick_pending(struct d5x *state,
						 bool pending)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	ctrl->post_start_kick_pending = pending;
	mutex_unlock(&ctrl->lock);
}

static void d5x_clear_dser_start_state(struct d5x *state)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	ctrl->post_start_kick_pending = false;
	mutex_unlock(&ctrl->lock);
}

static void d5x_finish_dser_prestart(struct d5x *state, bool reset_performed)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	ctrl->datapath_armed = reset_performed;
	if (reset_performed)
		ctrl->rearm_gen++;
	mutex_unlock(&ctrl->lock);
}

static unsigned int d5x_dser_rearm_gen(struct d5x *state)
{
	unsigned int gen;
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	mutex_lock(&ctrl->lock);
	gen = ctrl->rearm_gen;
	mutex_unlock(&ctrl->lock);

	return gen;
}

static unsigned int d5x_started_mipi_streams(struct d5x *state)
{
	unsigned int count = 0;

	mutex_lock(&state->d5x_dev->lock);
	if (state->d5x_dev->depth_streaming)
		count++;
	if (state->d5x_dev->rgb_streaming)
		count++;
	if (state->d5x_dev->ir_streaming)
		count++;
	if (state->d5x_dev->imu_streaming)
		count++;
	mutex_unlock(&state->d5x_dev->lock);

	return count;
}

static u8 d5x_started_mipi_pipe_mask(struct d5x *state)
{
	u8 mask = 0;

	mutex_lock(&state->d5x_dev->lock);
	if (state->d5x_dev->depth_streaming)
		mask |= BIT(0);
	if (state->d5x_dev->rgb_streaming)
		mask |= BIT(1);
	if (state->d5x_dev->ir_streaming)
		mask |= BIT(2);
	if (state->d5x_dev->imu_streaming)
		mask |= BIT(3);
	mutex_unlock(&state->d5x_dev->lock);

	return mask;
}

static void d5x_post_start_dser_datapath_kick(struct d5x *state, u16 stream_id)
{
	unsigned int started_streams;
	u8 active_pipe_mask;
	int active_pipes;
	int link_locked;

	mutex_lock(&serdes_lock__);
	link_locked = state->dser_ops->get_link_locked ?
		state->dser_ops->get_link_locked(state->dser_dev) : -1;
	if (link_locked <= 0)
		goto out;

	active_pipes = state->dser_ops->get_active_pipe_count ?
		state->dser_ops->get_active_pipe_count(state->dser_dev) : 1;
	started_streams = d5x_started_mipi_streams(state);
	active_pipe_mask = d5x_started_mipi_pipe_mask(state);
	if (started_streams < active_pipes) {
		dev_info(&state->client->dev,
			 "stream %u START ok; locked DES datapath unchanged until remaining streams start (%u/%d)\n",
			 stream_id, started_streams, active_pipes);
		goto out;
	}

	if (d5x_dser_datapath_matches(state, active_pipe_mask) &&
	    !d5x_dser_post_start_kick_pending(state)) {
		dev_info(&state->client->dev,
			 "stream %u START ok; keeping locked GMSL Link A unchanged for pipe mask 0x%02x (%u/%d streams started)\n",
			 stream_id, active_pipe_mask, started_streams, active_pipes);
		goto out;
	}

	dev_info(&state->client->dev,
		 "stream %u START ok; retriggering DES CSI datapath without resetting locked GMSL Link A for pipe mask 0x%02x (%u/%d streams started)\n",
		 stream_id, active_pipe_mask, started_streams, active_pipes);
	/* PIPE_EN/mapping retrigger is local to the DES CSI datapath.  ONESHOT
	 * would reset the whole Link A PHY/data path and invalidate queued VI
	 * frames, while leaving the datapath untouched does not resynchronize
	 * tunnel detection after the camera starts transmitting. */
	state->dser_ops->retrigger_datapath(state->dser_dev);
	d5x_clear_dser_start_state(state);
	d5x_set_dser_datapath_pipe_mask(state, active_pipe_mask);
	d5x_set_dser_post_start_kick_pending(state, false);

out:
	mutex_unlock(&serdes_lock__);
}

static void d5x_wait_for_dser_rearm_cooldown(struct d5x *state, const char *reason)
{
	struct dser_control *ctrl = state->d5x_dev->dser_control;
	unsigned long last_idle;
	unsigned long ready;
	unsigned int wait_ms;

	mutex_lock(&ctrl->lock);
	last_idle = ctrl->last_idle_jiffies;
	mutex_unlock(&ctrl->lock);

	if (!last_idle)
		return;

	ready = last_idle + msecs_to_jiffies(D5X_DSER_REARM_COOLDOWN_MS);
	if (!time_before(jiffies, ready))
		return;

	wait_ms = jiffies_to_msecs(ready - jiffies);
	dev_info(&state->client->dev,
		 "waiting %u ms before DES datapath re-arm (%s)\n",
		 wait_ms, reason);
	msleep(wait_ms);
}

static bool d5x_rearm_dser_datapath_before_start(struct d5x *state, int pipe_id,
						 int active_pipes,
						 int link_locked,
						 const char *reason)
{
	/* ONESHOT resets the complete Link A PHY and data path, including the
	 * tunnel carrying remote I2C.  A locked link does not need recovery;
	 * the post-START path will retrigger only the DES CSI data path. */
	if (link_locked > 0) {
		dev_info(&state->client->dev,
			 "pipe %d pre-start preserving locked Link A (%s, %d pipes active)\n",
			 pipe_id, reason, active_pipes);
		return false;
	}

	d5x_wait_for_dser_rearm_cooldown(state, reason);
	dev_info(&state->client->dev,
		 "pipe %d pre-start resetting GMSL Link A PHY and datapath with ONESHOT (%s, %d pipes active, link %s)\n",
		 pipe_id, reason, active_pipes,
		 link_locked > 0 ? "locked" :
		 (link_locked == 0 ? "unlocked" : "unknown"));
	state->dser_ops->reset_oneshot(state->dser_dev);
	max96717_retrigger_tx(state->ser_dev);
	msleep(200);
	d5x_set_dser_post_start_kick_pending(state, true);
	return true;
}

static void d5x_disarm_dser_datapath_if_idle(struct d5x *state)
{
	int active_pipes;
	int link_locked;
	bool armed;
	struct dser_control *ctrl = state->d5x_dev->dser_control;

	if (!state->dser_ops->get_active_pipe_count)
		return;

	active_pipes = state->dser_ops->get_active_pipe_count(state->dser_dev);
	if (active_pipes != 0)
		return;

	link_locked = state->dser_ops->get_link_locked ?
		state->dser_ops->get_link_locked(state->dser_dev) : -1;
	mutex_lock(&ctrl->lock);
	armed = ctrl->datapath_armed;
	ctrl->datapath_armed = false;
	ctrl->datapath_pipe_mask = 0;
	ctrl->post_start_kick_pending = false;
	ctrl->last_idle_jiffies = jiffies;
	mutex_unlock(&ctrl->lock);

	if (link_locked > 0) {
		if (armed)
			dev_info(&state->client->dev,
				 "all DES pipes released; preserving locked Link A for next start\n");
		return;
	}

	if (armed)
		dev_info(&state->client->dev,
			 "all DES pipes released; datapath marked unarmed for cooldown\n");
}

/* MAX96724 deserializer interface implementation */
static const struct dser_interface max96724_interface = {
	.get_available_pipe_id = max96724_get_available_pipe_id,
	.get_ser_pipe_id = max96724_get_ser_pipe_id,
	.bind_ser_to_dser_pipe = max96724_bind_ser_to_dser_pipe,
	.set_pipe = max96724_set_pipe,
	.release_pipe = max96724_release_pipe,
	.reset_oneshot = max96724_reset_oneshot,
	.retrigger_datapath = max96724_retrigger_datapath,
	.get_active_pipe_count = max96724_active_pipe_count,
	.get_link_locked = max96724_link_locked,
	.setup_link = max96724_setup_link,
	.setup_control = max96724_setup_control,
	.reset_control = max96724_reset_control,
	.setup_fsync = max96724_setup_fsync,
	.disable_fsync = max96724_disable_fsync,
	.sdev_register = max96724_sdev_register,
	.sdev_unregister = max96724_sdev_unregister,
	.power_on = max96724_power_on,
	.power_off = max96724_power_off,
	.init_settings = max96724_init_settings,
	.name = "max96724",
};

#else /* !CONFIG_VIDEO_D5XX_SERDES */

static atomic_t d5x_reset_gen = ATOMIC_INIT(0);
static inline atomic_t *d5x_get_reset_gen(struct d5x *state)
{
	return &d5x_reset_gen;
}

#define MAX_D5X_NUM (1)
static struct d5x_dev d5x_inited[MAX_D5X_NUM];
static bool d5x_slots_inited;
static DEFINE_MUTEX(d5x_slots_lock__);

static void d5x_init_global_slots_once(void)
{
	mutex_lock(&d5x_slots_lock__);
	if (!d5x_slots_inited) {
		mutex_init(&d5x_inited[0].lock);
		mutex_init(&d5x_inited[0].stream_ctrl_lock);
		d5x_slots_inited = true;
	}
	mutex_unlock(&d5x_slots_lock__);
}

#endif /* !CONFIG_VIDEO_D5XX_SERDES */

static inline u16 d5x_dev_type(struct d5x *state, u16 dev_type)
{
	if (dev_type == 0 && state->d5x_dev->cached_device_type != 0) {
		dev_info(&state->client->dev,
			"%s(): device type register returned 0, using cached type 0x%x\n",
			__func__, state->d5x_dev->cached_device_type);
		dev_type = state->d5x_dev->cached_device_type;
	}
	return dev_type;
}

static bool d5x_is_valid_device_type(u16 dev_type)
{
	switch (dev_type) {
	case D5X_DEVICE_TYPE_D58X:
		return true;
	default:
		return false;
	}
}

#define d5x_from_depth_sd(sd) container_of(sd, struct d5x, depth.sd)
#define d5x_from_ir_sd(sd) container_of(sd, struct d5x, ir.sd)
#define d5x_from_rgb_sd(sd) container_of(sd, struct d5x, rgb.sd)
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 136)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 148)
static inline void msleep_range(unsigned int delay_base)
{
	usleep_range(delay_base * 1000, delay_base * 1000 + 500);
}
#endif
#endif

static unsigned int d5x_stream_poll_delay(bool starting, unsigned int retry)
{
	unsigned int delay = retry * D5X_START_POLL_TIME;

	if (starting)
		return min(delay, (unsigned int)D5X_START_POLL_MAX_TIME);
	return delay;
}

static int d5x_write(struct d5x *state, u16 reg, u16 val)
{
	int ret;
	int retry;
	u8 value[2];

	value[1] = val >> 8;
	value[0] = val & 0x00FF;

	dev_dbg(&state->client->dev,
			"%s(): writing to register: 0x%04x, value1: 0x%x, value2:0x%x\n",
			__func__, reg, value[1], value[0]);

	for (retry = 0; retry < D5X_I2C_RETRY_COUNT; retry++) {
		ret = regmap_raw_write(state->regmap, reg, value, sizeof(value));
		if (ret == 0)
			break;
		if (retry < D5X_I2C_RETRY_COUNT - 1) {
			dev_warn(&state->client->dev,
				"%s(): i2c write retry %d, 0x%04x = 0x%x, err %d\n",
				__func__, retry + 1, reg, val, ret);
			usleep_range(D5X_I2C_RETRY_DELAY_US,
				     D5X_I2C_RETRY_DELAY_US + 500);
		}
	}
	if (ret < 0)
		dev_err(&state->client->dev,
			"%s(): i2c write failed after %d retries, 0x%04x = 0x%x, err %d\n",
			__func__, D5X_I2C_RETRY_COUNT, reg, val, ret);
	else if (state->dfu_dev.dfu_state_flag == D5X_DFU_IDLE)
		dev_dbg(&state->client->dev, "%s(): i2c write 0x%04x: 0x%x\n",
			__func__, reg, val);

	return ret;
}

static int d5x_write_then_read(struct d5x *state, u16 write_reg, u16 write_val,
			       u16 read_reg, u16 *read_val)
{
	struct i2c_client *client = state->client;
	u8 write_buf[4] = {
		write_reg & 0xff, write_reg >> 8,
		write_val & 0xff, write_val >> 8,
	};
	u8 read_reg_buf[2] = { read_reg & 0xff, read_reg >> 8 };
	u8 read_buf[2];
	u16 flags = client->flags & I2C_M_TEN;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = flags,
			.len = sizeof(write_buf),
			.buf = write_buf,
		},
		{
			.addr = client->addr,
			.flags = flags,
			.len = sizeof(read_reg_buf),
			.buf = read_reg_buf,
		},
		{
			.addr = client->addr,
			.flags = flags | I2C_M_RD,
			.len = sizeof(read_buf),
			.buf = read_buf,
		},
	};
	int retry;
	int ret = -EIO;

	/* Keep the command and its first status read in one adapter transaction.
	 * Repeated STARTs provide an unambiguous boundary to the remote slave even
	 * when its polling task is scheduled after all address bytes have arrived. */
	for (retry = 0; retry < D5X_I2C_RETRY_COUNT; retry++) {
		ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret == ARRAY_SIZE(msgs)) {
			*read_val = read_buf[0] | ((u16)read_buf[1] << 8);
			return 0;
		}
		if (ret >= 0)
			ret = -EIO;
		if (retry < D5X_I2C_RETRY_COUNT - 1)
			usleep_range(D5X_I2C_RETRY_DELAY_US,
				     D5X_I2C_RETRY_DELAY_US + 500);
	}

	dev_err(&client->dev,
		"%s(): i2c write/read failed after %d retries, 0x%04x = 0x%x -> 0x%04x, err %d\n",
		__func__, D5X_I2C_RETRY_COUNT, write_reg, write_val, read_reg, ret);
	return ret;
}

static int d5x_raw_write(struct d5x *state, u16 reg,
			const void *val, size_t val_len)
{
	int ret;
	int retry;

	for (retry = 0; retry < D5X_I2C_RETRY_COUNT; retry++) {
		ret = regmap_raw_write(state->regmap, reg, val, val_len);
		if (ret == 0)
			break;
		if (retry < D5X_I2C_RETRY_COUNT - 1) {
			dev_warn(&state->client->dev,
				"%s(): i2c raw write retry %d, 0x%04x size(%d), err %d\n",
				__func__, retry + 1, reg, (int)val_len, ret);
			usleep_range(D5X_I2C_RETRY_DELAY_US,
				     D5X_I2C_RETRY_DELAY_US + 500);
		}
	}
	if (ret < 0)
		dev_err(&state->client->dev,
			"%s(): i2c raw write failed after %d retries, 0x%04x size(%d), err %d\n",
			__func__, D5X_I2C_RETRY_COUNT, reg, (int)val_len, ret);
	else if (state->dfu_dev.dfu_state_flag == D5X_DFU_IDLE)
		dev_dbg(&state->client->dev,
			"%s(): i2c raw write 0x%04x: %d bytes\n",
			__func__, reg, (int)val_len);

	return ret;
}

static int d5x_read(struct d5x *state, u16 reg, u16 *val)
{
	int ret;
	int retry;

	for (retry = 0; retry < D5X_I2C_RETRY_COUNT; retry++) {
		ret = regmap_raw_read(state->regmap, reg, val, 2);
		if (ret == 0)
			break;
		if (retry < D5X_I2C_RETRY_COUNT - 1) {
			dev_warn(&state->client->dev,
				"%s(): i2c read retry %d, 0x%04x, err %d\n",
				__func__, retry + 1, reg, ret);
			usleep_range(D5X_I2C_RETRY_DELAY_US,
				     D5X_I2C_RETRY_DELAY_US + 500);
		}
	}
	if (ret < 0)
		dev_err(&state->client->dev,
			"%s(): i2c read failed after %d retries, 0x%04x, err %d\n",
			__func__, D5X_I2C_RETRY_COUNT, reg, ret);
	else if (state->dfu_dev.dfu_state_flag == D5X_DFU_IDLE)
		dev_dbg(&state->client->dev, "%s(): i2c read 0x%04x: 0x%x\n",
			__func__, reg, *val);

	return ret;
}

static int d5x_read_poll(struct d5x *state, u16 reg, u16 *val)
{
	return regmap_raw_read(state->regmap, reg, val, 2);
}

static int d5x_raw_read(struct d5x *state, u16 reg, void *val, size_t val_len)
{
	int ret;
	int retry;

	for (retry = 0; retry < D5X_I2C_RETRY_COUNT; retry++) {
		ret = regmap_raw_read(state->regmap, reg, val, val_len);
		if (ret == 0)
			break;
		if (retry < D5X_I2C_RETRY_COUNT - 1) {
			dev_warn(&state->client->dev,
				"%s(): i2c raw read retry %d, 0x%04x size(%d), err %d\n",
				__func__, retry + 1, reg, (int)val_len, ret);
			usleep_range(D5X_I2C_RETRY_DELAY_US,
				     D5X_I2C_RETRY_DELAY_US + 500);
		}
	}
	if (ret < 0)
		dev_err(&state->client->dev,
			"%s(): i2c raw read failed after %d retries, 0x%04x size(%d), err %d\n",
			__func__, D5X_I2C_RETRY_COUNT, reg, (int)val_len, ret);

	return ret;
}

/* Pad ops */

static const u16 d5x_default_framerate = 30;

#define D5X_FRAMERATE_DEFAULT_IDX 1

static const u16 d5x_framerate_30 = 30;
static const u16 d5x_depth_framerate_to_30[] = {5, 15, 30};
static const u16 d5x_framerate_to_60[] = {5, 15, 30, 60};
static const u16 d5x_framerate_to_90[] = {5, 15, 30, 60, 90};
static const u16 d5x_framerate_15_30[] = {15, 30};
static const u16 d5x_framerate_15_25[] = {15, 25};
static const u16 d5x_framerate_25[] = {25};
static const u16 d5x_framerate_90[] = {90};
static const u16 d5x_imu_framerates[] = {100, 200, 400};

/* Helper macro to define resolution entries concisely. */
#define D5X_RES(w, h, fr) \
    { .width = (w), .height = (h), .framerates = (fr), .n_framerates = ARRAY_SIZE(fr) },

/* D58x depth resolutions: DEPTH (Z16) + DEPTH_RAW (Z16) */
static const struct d5x_resolution d58x_depth_sizes[] = {
	D5X_RES(640, 360, d5x_framerate_to_90)	/* default */
	D5X_RES(1280, 960, d5x_framerate_to_60)
	D5X_RES(1280, 720, d5x_framerate_to_60)
	D5X_RES(896, 504, d5x_framerate_to_60)
	D5X_RES(848, 480, d5x_framerate_to_60)
	D5X_RES(640, 480, d5x_framerate_to_90)
	D5X_RES(480, 270, d5x_framerate_to_90)
	D5X_RES(424, 240, d5x_framerate_to_90)
};

/* D58x IR/Y8 resolutions: IR (Y8/L8R8) + IR_RAW (Y8) */
static const struct d5x_resolution d58x_y8_sizes[] = {
	D5X_RES(640, 360, d5x_framerate_to_90)	/* default */
	D5X_RES(1280, 960, d5x_framerate_to_60)
	D5X_RES(1280, 720, d5x_framerate_to_60)
	D5X_RES(896, 504, d5x_framerate_to_60)
	D5X_RES(848, 480, d5x_framerate_to_60)
	D5X_RES(640, 480, d5x_framerate_to_90)
	D5X_RES(480, 270, d5x_framerate_to_90)
	D5X_RES(424, 240, d5x_framerate_to_90)
};

/* D58x calibration resolutions: IR_RAW Y12I (24-bit) + Self-Calibration/Tare */
static const struct d5x_resolution d58x_calibration_sizes[] = {
	D5X_RES(1600, 1300, d5x_framerate_15_25)
	D5X_RES(256, 144, d5x_framerate_90)
};

/* D58x RGB resolutions: COLOR (YUY2) */
static const struct d5x_resolution d58x_rgb_sizes[] = {
	D5X_RES(640, 360, d5x_framerate_to_90)	/* default */
	D5X_RES(1280, 960, d5x_framerate_to_60)
	D5X_RES(1280, 720, d5x_framerate_to_60)
	D5X_RES(896, 504, d5x_framerate_to_60)
	D5X_RES(848, 480, d5x_framerate_to_60)
	D5X_RES(640, 480, d5x_framerate_to_90)
	D5X_RES(480, 270, d5x_framerate_to_90)
	D5X_RES(424, 240, d5x_framerate_to_90)
};

/* D58x packed Bayer RGB profile carried as an opaque CSI-2 UD30 byte stream. */
static const struct d5x_resolution d58x_rgb_raw_sizes[] = {
	D5X_RES(1600, 1300, d5x_framerate_25)
};

static const struct d5x_resolution d5x_size_imu[] = {
	{
	.width = 32,
	.height = 1,
	.framerates = d5x_imu_framerates,
	.n_framerates = ARRAY_SIZE(d5x_imu_framerates),
	},
};

/* 32 bit IMU introduced with IMU sensitivity attribute Firmware */
static const struct d5x_resolution d5x_size_imu_extended[] = {
	{
	.width = 38,
	.height = 1,
	.framerates = d5x_imu_framerates,
	.n_framerates = ARRAY_SIZE(d5x_imu_framerates),
	},
};

static const struct d5x_format d5x_depth_formats_d58x[] = {
	{
		.data_type = GMSL_CSI_DT_YUV422_8,	/* Z16 */
		.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
		.n_resolutions = ARRAY_SIZE(d58x_depth_sizes),
		.resolutions = d58x_depth_sizes,
	}, {
		.data_type = GMSL_CSI_DT_RAW_8,	/* Y8 */
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.n_resolutions = ARRAY_SIZE(d58x_depth_sizes),
		.resolutions = d58x_depth_sizes,
	}, {
		.data_type = GMSL_CSI_DT_RGB_888,	/* 24-bit Calibration */
		.mbus_code = MEDIA_BUS_FMT_RGB888_1X24,
		.n_resolutions = ARRAY_SIZE(d58x_calibration_sizes),
		.resolutions = d58x_calibration_sizes,
	},
};

#define D5X_DEPTH_N_FORMATS 1

static const struct d5x_format d5x_y_formats_d58x[] = {
	{
		/* First format: default */
		.data_type = GMSL_CSI_DT_RAW_8,	/* Y8 */
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.n_resolutions = ARRAY_SIZE(d58x_y8_sizes),
		.resolutions = d58x_y8_sizes,
	}, {
		.data_type = GMSL_CSI_DT_YUV422_8,	/* Y8I */
		.mbus_code = MEDIA_BUS_FMT_VYUY8_1X16,
		.n_resolutions = ARRAY_SIZE(d58x_y8_sizes),
		.resolutions = d58x_y8_sizes,
	}, {
		.data_type = GMSL_CSI_DT_RGB_888,	/* Y12I, 24-bit Calibration */
		.mbus_code = MEDIA_BUS_FMT_RGB888_1X24,
		.n_resolutions = ARRAY_SIZE(d58x_calibration_sizes),
		.resolutions = d58x_calibration_sizes,
	},
};

static const struct d5x_format d5x_rgb_formats_d58x[] = {
	{
		/* CSI2_DT 0x1E uses MC-FCVT UYVY bytes; V4L2 exposes YUYV. */
		.data_type = GMSL_CSI_DT_YUV422_8,
		.mbus_code = MEDIA_BUS_FMT_YUYV8_1X16,
		.n_resolutions = ARRAY_SIZE(d58x_rgb_sizes),
		.resolutions = d58x_rgb_sizes,
	}, {
		/* Flat NV12 surface carried as width x (height * 3 / 2) RAW8. */
		.data_type = GMSL_CSI_DT_RAW_8,
		.mbus_code = MEDIA_BUS_FMT_UYYVYY8_0_5X24,
		.n_resolutions = ARRAY_SIZE(d58x_rgb_sizes),
		.resolutions = d58x_rgb_sizes,
	}, {
		/*
		 * HKR emits one 10-bit GRBG sample in each 16-bit container.
		 * RAW16 preserves those containers on wire; V4L2 exposes BA10.
		 */
		.data_type = GMSL_CSI_DT_RAW_16,
		.mbus_code = MEDIA_BUS_FMT_RS_SGRBG10_1X16,
		.n_resolutions = ARRAY_SIZE(d58x_rgb_raw_sizes),
		.resolutions = d58x_rgb_raw_sizes,
	}, {
		/*
		 * This Host-output option reuses the same RAW16 transport as
		 * BA10, then packs each completed T_R16 row into standard pgAA.
		 */
		.data_type = GMSL_CSI_DT_RAW_16,
		.mbus_code = MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16,
		.n_resolutions = ARRAY_SIZE(d58x_rgb_raw_sizes),
		.resolutions = d58x_rgb_raw_sizes,
	}, {
		/*
		 * HKR FMT_SGRBG10P bytes are carried unchanged as CSI DT 0x30.
		 * The private mbus code prevents this surface from being exposed
		 * as standard BA10 or V4L2 IPU3 SGRBG10.
		 */
		.data_type = GMSL_CSI_DT_USER_1,
		.mbus_code = MEDIA_BUS_FMT_RS_SGRBG10P_1X10,
		.n_resolutions = ARRAY_SIZE(d58x_rgb_raw_sizes),
		.resolutions = d58x_rgb_raw_sizes,
	},
};

static const struct d5x_variant d5x_variants[] = {
	[D5X_DS5U] = {
		.formats = d5x_y_formats_d58x,
		.n_formats = ARRAY_SIZE(d5x_y_formats_d58x),
	},
};

static const struct d5x_format d5x_imu_formats[] = {
	{
		/* First format: default */
		.data_type = GMSL_CSI_DT_RAW_8,	/* IMU DT */
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.n_resolutions = ARRAY_SIZE(d5x_size_imu),
		.resolutions = d5x_size_imu,
	},
};

static const struct d5x_format d5x_imu_formats_extended[] = {
	{
		/* First format: default */
		.data_type = GMSL_CSI_DT_RAW_8,	/* IMU DT */
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.n_resolutions = ARRAY_SIZE(d5x_size_imu_extended),
		.resolutions = d5x_size_imu_extended,
	},
};

static const struct v4l2_mbus_framefmt d5x_mbus_framefmt_template = {
	.width = 0,
	.height = 0,
	.code = MEDIA_BUS_FMT_FIXED,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_DEFAULT,
	.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
	.quantization = V4L2_QUANTIZATION_DEFAULT,
	.xfer_func = V4L2_XFER_FUNC_DEFAULT,
};

/* Get readable sensor name */
static const char *d5x_get_sensor_name(struct d5x *state)
{
	static const char *sensor_name[] = {"unknown", "RGB", "DEPTH", "Y8", "IMU"};
	int sensor_id = state->is_rgb * 1 + state->is_depth * 2 + \
			state->is_y8 * 3 + state->is_imu * 4;
	if (sensor_id >= (sizeof(sensor_name)/sizeof(*sensor_name)))
		sensor_id = 0;

	return sensor_name[sensor_id];
}

static void d5x_set_state_last_set(struct d5x *state)
{
	 dev_dbg(&state->client->dev, "%s(): %s\n",
		__func__, d5x_get_sensor_name(state));

	if (state->is_depth)
		state->mux.last_set = &state->depth.sensor;
	else if (state->is_rgb)
		state->mux.last_set = &state->rgb.sensor;
	else if (state->is_y8)
		state->mux.last_set = &state->ir.sensor;
	else
		state->mux.last_set = &state->imu.sensor;
}

static void d5x_sensor_format_init(struct d5x_sensor *sensor)
{
	const struct d5x_format *fmt;
	struct v4l2_mbus_framefmt *ffmt;
	unsigned int i;

	if (sensor->config.format)
		return;

	dev_dbg(sensor->sd.dev, "%s(): on pad %u\n", __func__, sensor->mux_pad);

	ffmt = &sensor->format;
	*ffmt = d5x_mbus_framefmt_template;
	/* Use the first format */
	fmt = sensor->formats;
	ffmt->code = fmt->mbus_code;
	/* and the first resolution */
	ffmt->width = fmt->resolutions->width;
	ffmt->height = fmt->resolutions->height;

	sensor->config.format = fmt;
	sensor->config.resolution = fmt->resolutions;
	/* Set default framerate to 30, or to 1st one if not supported */
	for (i = 0; i < fmt->resolutions->n_framerates; i++) {
		if (fmt->resolutions->framerates[i] == d5x_framerate_30 /* fps */) {
			sensor->config.framerate = d5x_framerate_30;
			return;
		}
	}
	sensor->config.framerate = fmt->resolutions->framerates[0];
}

/* No locking needed for enumeration methods */
static int d5x_sensor_enum_mbus_code(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
				     struct v4l2_subdev_pad_config *cfg,
#else
				     struct v4l2_subdev_state *v4l2_state,
#endif
				     struct v4l2_subdev_mbus_code_enum *mce)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);

	dev_dbg(sensor->sd.dev, "%s(): sensor %s pad: %d index: %d\n",
		__func__, sensor->sd.name, mce->pad, mce->index);
	if (mce->pad)
		return -EINVAL;

	if (mce->index >= sensor->n_formats)
		return -EINVAL;

	mce->code = sensor->formats[mce->index].mbus_code;

	return 0;
}

static int d5x_sensor_enum_frame_size(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_frame_size_enum *fse)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);
	struct d5x *state = v4l2_get_subdevdata(sd);
	const struct d5x_format *fmt;
	unsigned int i;

	dev_dbg(sensor->sd.dev, "%s(): sensor %s is %s\n",
		__func__, sensor->sd.name, d5x_get_sensor_name(state));

	for (i = 0, fmt = sensor->formats; i < sensor->n_formats; i++, fmt++)
		if (fse->code == fmt->mbus_code)
			break;

	if (i == sensor->n_formats)
		return -EINVAL;

	if (fse->index >= fmt->n_resolutions)
		return -EINVAL;

	fse->min_width = fse->max_width = fmt->resolutions[fse->index].width;
	fse->min_height = fse->max_height = fmt->resolutions[fse->index].height;

	return 0;
}

static int d5x_sensor_enum_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_frame_interval_enum *fie)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);
	const struct d5x_format *fmt;
	const struct d5x_resolution *res;
	unsigned int i;

	for (i = 0, fmt = sensor->formats; i < sensor->n_formats; i++, fmt++)
		if (fie->code == fmt->mbus_code)
			break;

	if (i == sensor->n_formats)
		return -EINVAL;

	for (i = 0, res = fmt->resolutions; i < fmt->n_resolutions; i++, res++)
		if (res->width == fie->width && res->height == fie->height)
			break;

	if (i == fmt->n_resolutions)
		return -EINVAL;

	if (fie->index >= res->n_framerates)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = res->framerates[fie->index];

	return 0;
}

static int d5x_sensor_get_fmt(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_format *fmt)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);
	struct d5x *state = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (fmt->pad)
		return -EINVAL;

	mutex_lock(&state->lock);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
		fmt->format = *v4l2_subdev_get_try_format(sd, v4l2_state, fmt->pad);
#else
	{
		struct v4l2_mbus_framefmt* framefmt;
		framefmt = v4l2_subdev_state_get_format(v4l2_state, fmt->pad);
		if (framefmt)
			fmt->format = *framefmt;
		else
			ret = -EINVAL;
	}
#endif
	else
		fmt->format = sensor->format;

	mutex_unlock(&state->lock);

	dev_dbg(sd->dev, "%s(): pad %x, code %x, res %ux%u\n",
			__func__, fmt->pad, fmt->format.code,
			fmt->format.width, fmt->format.height);

	return ret;
}

/* Called with lock held */
static const struct d5x_format *d5x_sensor_find_format(
		struct d5x_sensor *sensor,
		struct v4l2_mbus_framefmt *ffmt,
		const struct d5x_resolution **best)
{
	const struct d5x_resolution *res;
	const struct d5x_format *fmt;
	unsigned long best_delta = ~0;
	unsigned int i;

	for (i = 0, fmt = sensor->formats; i < sensor->n_formats; i++, fmt++) {
		if (fmt->mbus_code == ffmt->code)
			break;
	}
	dev_dbg(sensor->sd.dev, "%s(): mbus_code = %x, code = %x \n",
		__func__, fmt->mbus_code, ffmt->code);

	if (i == sensor->n_formats) {
		/* Not found, use default */
		dev_dbg(sensor->sd.dev, "%s:%d Not found, use default\n",
			__func__, __LINE__);
		fmt = sensor->formats;
	}
	for (i = 0, res = fmt->resolutions; i < fmt->n_resolutions; i++, res++) {
		unsigned long delta = abs(ffmt->width * ffmt->height -
				res->width * res->height);
		if (delta < best_delta) {
			best_delta = delta;
			*best = res;
		}
	}

	ffmt->code = fmt->mbus_code;
	ffmt->width = (*best)->width;
	ffmt->height = (*best)->height;

	ffmt->field = V4L2_FIELD_NONE;
	ffmt->colorspace = V4L2_COLORSPACE_SRGB;

	return fmt;
}

#define MIPI_CSI2_TYPE_NULL	0x10
#define MIPI_CSI2_TYPE_BLANKING		0x11
#define MIPI_CSI2_TYPE_EMBEDDED8	0x12
#define MIPI_CSI2_TYPE_YUV422_8		0x1e
#define MIPI_CSI2_TYPE_YUV422_10	0x1f
#define MIPI_CSI2_TYPE_RGB565	0x22
#define MIPI_CSI2_TYPE_RGB888	0x24
#define MIPI_CSI2_TYPE_RAW6	0x28
#define MIPI_CSI2_TYPE_RAW7	0x29
#define MIPI_CSI2_TYPE_RAW8	0x2a
#define MIPI_CSI2_TYPE_RAW10	0x2b
#define MIPI_CSI2_TYPE_RAW12	0x2c
#define MIPI_CSI2_TYPE_RAW14	0x2d
/* 1-8 */
#define MIPI_CSI2_TYPE_USER_DEF(i)	(0x30 + (i) - 1)

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
static void d5x_tegra_update_mipi_clock(struct sensor_signal_properties *signal,
					u32 bit_depth)
{
	u64 rate;

	if (!signal || !signal->num_lanes)
		return;

	rate = signal->serdes_pixel_clock.val ?
		signal->serdes_pixel_clock.val : signal->pixel_clock.val;
	rate = div_u64(rate * bit_depth, signal->num_lanes);

	if (signal->phy_mode == CSI_PHY_MODE_DPHY)
		signal->mipi_clock.val = div_u64(rate, 2);
	else if (signal->phy_mode == CSI_PHY_MODE_CPHY)
		signal->mipi_clock.val = div_u64(rate * 7, 16);
	else
		signal->mipi_clock.val = rate;
}

static void d5x_tegra_update_rgb_mode(struct d5x *state,
				      const struct d5x_sensor *sensor)
{
	struct sensor_mode_properties *mode;
	struct sensor_image_properties *image;
	u32 pixel_format;
	u32 bit_depth;

	if (!state->mux.sd.sensor_props.sensor_modes ||
	    !sensor->config.format || !sensor->config.resolution)
		return;

	switch (sensor->config.format->mbus_code) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
		pixel_format = V4L2_PIX_FMT_NV12;
		bit_depth = 12;
		break;
	case MEDIA_BUS_FMT_YUYV8_1X16:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bit_depth = 16;
		break;
	case MEDIA_BUS_FMT_RS_SGRBG10_1X16:
		pixel_format = V4L2_PIX_FMT_SGRBG10;
		/* The CSI carrier transmits the complete 16-bit container. */
		bit_depth = 16;
		break;
	case MEDIA_BUS_FMT_RS_SGRBG10P_RAW16_1X16:
		pixel_format = V4L2_PIX_FMT_SGRBG10P;
		/* pgAA is packed after VI; the CSI carrier remains RAW16. */
		bit_depth = 16;
		break;
	case MEDIA_BUS_FMT_RS_SGRBG10P_1X10:
		pixel_format = V4L2_PIX_FMT_RS_SGRBG10P;
		bit_depth = 10;
		break;
	default:
		return;
	}

	mode = &state->mux.sd.sensor_props.sensor_modes[0];
	image = &mode->image_properties;
	image->width = sensor->format.width;
	image->height = sensor->format.height;
	image->line_length = sensor->format.width;
	image->pixel_format = pixel_format;
	image->embedded_metadata_height = state->metadata_enabled ? 1 : 0;

	d5x_tegra_update_mipi_clock(&mode->signal_properties, bit_depth);

	state->mux.sd.mode_prop_idx = 0;
	state->mux.sd.mode = state->mux.sd.def_mode;
	state->mux.sd.fmt_width = image->width;
	state->mux.sd.fmt_height = image->height;
}
#endif

static int __d5x_sensor_set_fmt(struct d5x *state, struct d5x_sensor *sensor,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf;// = &fmt->format;
	int ret = 0;
	//unsigned r;

	dev_dbg(sensor->sd.dev, "%s(): state %p, "
		"sensor %p, fmt %p, fmt->format %p\n",
		__func__, state, sensor, fmt,  &fmt->format);

	mf = &fmt->format;

	if (fmt->pad)
		return -EINVAL;

	mutex_lock(&state->lock);

	sensor->config.format = d5x_sensor_find_format(sensor, mf,
						&sensor->config.resolution);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
	if (cfg && fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		*v4l2_subdev_get_try_format(&sensor->sd, cfg, fmt->pad) = *mf;
#else
	if (v4l2_state && fmt->which == V4L2_SUBDEV_FORMAT_TRY)
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
		*v4l2_subdev_get_try_format(&sensor->sd, v4l2_state, fmt->pad) = *mf;
#else
	{
		struct v4l2_mbus_framefmt* framefmt = v4l2_subdev_state_get_format(v4l2_state, fmt->pad);
		if (framefmt)
			*framefmt = *mf;
		else
			ret = -EINVAL;
	}
#endif
#endif

	else {
		sensor->format = *mf;
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
		if (state->is_rgb) {
			d5x_tegra_update_rgb_mode(state, sensor);
		} else if (state->is_y8) {
			if (sensor->config.format->mbus_code == MEDIA_BUS_FMT_Y8_1X8)
				state->mux.sd.mode_prop_idx = 0;
			else
				state->mux.sd.mode_prop_idx = 1;
		}
#endif
	}

	state->mux.last_set = sensor;

	mutex_unlock(&state->lock);
	return ret;
}

static int d5x_sensor_set_fmt(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_format *fmt)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);
	struct d5x *state = v4l2_get_subdevdata(sd);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
	return __d5x_sensor_set_fmt(state, sensor, cfg, fmt);
#else
	return __d5x_sensor_set_fmt(state, sensor, v4l2_state, fmt);
#endif
}

#ifdef CONFIG_VIDEO_D5XX_SERDES
static int d5x_setup_pipeline(struct d5x *state, u8 data_type1, u8 data_type2,
			      int pipe_id, u32 vc_id)
{
	int ret = 0;
	/* While some deserializers can support up to 8 pipes, the serializer only supports
	 * four pipes and four vc_ids (0 - 3).
	 * To use multiple cameras under this restriction, a second camera connected
	 * to a deserializer will have its vc_id 0 - 3 mapped to outside vc_id 4 - 7 etc.
	 * The ser_pipe to dser_pipe mapping depends on the deserializer.
	 */
	int ser_vc_id = vc_id % D5X_MAX_STREAMS;
	int ser_pipe_id = state->dser_ops->get_ser_pipe_id(state->dser_dev, pipe_id, ser_vc_id);
	int active_pipes = (state->dser_ops->get_active_pipe_count != NULL) ?
		state->dser_ops->get_active_pipe_count(state->dser_dev) : 1;
	int link_locked = (state->dser_ops->get_link_locked != NULL) ?
		state->dser_ops->get_link_locked(state->dser_dev) : -1;
	bool datapath_armed = d5x_dser_datapath_armed(state);

	if (link_locked > 0 && !datapath_armed && active_pipes <= 1)
		dev_info(&state->client->dev,
			 "link locked; DES datapath retrigger will run after camera START\n");

	ret |= state->dser_ops->bind_ser_to_dser_pipe(state->dser_dev, pipe_id, ser_pipe_id, vc_id);
	dev_dbg(&state->client->dev,
			"set ser pipe %d, dser pipe %d, data_type1: 0x%x, data_type2: 0x%x, ser_vc_id: %u, vc_id: %u\n",
			ser_pipe_id, pipe_id, data_type1, data_type2, ser_vc_id, vc_id);
	ret |= max96717_set_pipe(state->ser_dev, ser_pipe_id,
				data_type1, data_type2, ser_vc_id);
	ret |= state->dser_ops->set_pipe(state->dser_dev, pipe_id,
				data_type1, data_type2, vc_id);
	if (ret >= 0 && link_locked > 0 && datapath_armed) {
		unsigned int started_streams = d5x_started_mipi_streams(state);

		dev_info(&state->client->dev,
			 "pipe %d mapping updated while DES datapath armed (%u streams active); keeping datapath armed\n",
			 pipe_id, started_streams);
	}
	/* Pipe setup is serialized by serdes_lock__, so the first active pipe can
	 * perform the pre-start recovery decision directly.  Waiting for a fixed
	 * pipe owner delays the first frame and can exceed the VI startup timeout
	 * when RGB intentionally starts before the stereo streams. */
	if (active_pipes <= 1 && !datapath_armed) {
		bool reset_performed;

		dev_info(&state->client->dev,
			 "pipe %d evaluating DES datapath before camera I2C config (link %s, active_pipes=%d)\n",
			 pipe_id,
			 link_locked > 0 ? "locked" :
			 (link_locked == 0 ? "unlocked" : "unknown"),
			 active_pipes);
		reset_performed = d5x_rearm_dser_datapath_before_start(state,
									 pipe_id, active_pipes,
									 link_locked,
									 "first active pipe");
		d5x_finish_dser_prestart(state, reset_performed);
	} else {
		dev_info(&state->client->dev,
			 "pipe %d mapping updated w/o link reset (%d pipes active, link %s)\n",
			 pipe_id, active_pipes,
			 link_locked > 0 ? "locked" :
			 (link_locked == 0 ? "unlocked" : "unknown"));
	}
	if (ret)
		dev_warn(&state->client->dev,
			 "failed to set pipe %d, data_type1: 0x%x, data_type2: 0x%x, vc_id: %u\n",
			 pipe_id, data_type1, data_type2, vc_id);

	return ret;
}
#endif

static void d5x_config_cache_clear(struct d5x_sensor *sensor)
{
	sensor->cached_dt_value = 0xFFFF;
	sensor->cached_md_value = 0xFFFF;
	sensor->cached_override_value = 0xFFFF;
	sensor->cached_fps_value = 0xFFFF;
	sensor->cached_width_value = 0xFFFF;
	sensor->cached_height_value = 0xFFFF;
}

static void d5x_invalidate_sensor(struct d5x *state, struct d5x_sensor *sensor)
{
	d5x_config_cache_clear(sensor);
	/* 
	 * Do NOT release SERDES pipes or clear pipe_id here.
	 * Preserve the existing pipe_id so that d5x_configure() can
	 * release-then-reallocate the pipe at stream-start time.
	 * Clearing pipe_data_type forces d5x_configure() to enter the
	 * re-allocation path (data_type mismatch triggers pipe setup).
	 */
	sensor->pipe_data_type1 = 0;
	sensor->pipe_data_type2 = 0;
	sensor->pipe_vc_id = 0xFFFF;
	sensor->pipe_reapply_gen = 0;
}

static int d5x_configure(struct d5x *state)
{
	struct d5x_sensor *sensor;
	u16 md_fmt, vc_id;
#ifdef CONFIG_VIDEO_D5XX_SERDES
	u16 data_type1, data_type2;
	bool is_calib = 0;
	unsigned int pipe_reapply_gen;
#endif
#if !D5X_BYPASS_CAMERA_I2C
	u16 dt_addr, md_addr, override_addr, fps_addr, width_addr, height_addr;
	u16 dt_value = 0;
	u16 md_value = 0;
	u16 fps_value = 0;
	u16 width_value = 0;
	u16 height_value = 0;
#endif
	int ret;

	if (state->is_depth) {
		sensor = &state->depth.sensor;
#if !D5X_BYPASS_CAMERA_I2C
		dt_addr = D5X_DEPTH_STREAM_DT;
		md_addr = D5X_DEPTH_STREAM_MD;
		override_addr = D5X_DEPTH_OVERRIDE;
		fps_addr = D5X_DEPTH_FPS;
		width_addr = D5X_DEPTH_RES_WIDTH;
		height_addr = D5X_DEPTH_RES_HEIGHT;
#endif
	} else if (state->is_rgb) {
		sensor = &state->rgb.sensor;
#if !D5X_BYPASS_CAMERA_I2C
		dt_addr = D5X_RGB_STREAM_DT;
		md_addr = D5X_RGB_STREAM_MD;
		override_addr = 0;
		fps_addr = D5X_RGB_FPS;
		width_addr = D5X_RGB_RES_WIDTH;
		height_addr = D5X_RGB_RES_HEIGHT;
#endif
	} else if (state->is_y8) {
		sensor = &state->ir.sensor;
#if !D5X_BYPASS_CAMERA_I2C
		dt_addr = D5X_IR_STREAM_DT;
		md_addr = D5X_IR_STREAM_MD;
		override_addr = D5X_IR_OVERRIDE;
		fps_addr = D5X_IR_FPS;
		width_addr = D5X_IR_RES_WIDTH;
		height_addr = D5X_IR_RES_HEIGHT;
#endif
	} else if (state->is_imu) {
		sensor = &state->imu.sensor;
#if !D5X_BYPASS_CAMERA_I2C
		dt_addr = D5X_IMU_STREAM_DT;
		md_addr = D5X_IMU_STREAM_MD;
		override_addr = 0;
		fps_addr = D5X_IMU_FPS;
		width_addr = D5X_IMU_RES_WIDTH;
		height_addr = D5X_IMU_RES_HEIGHT;
#endif
	} else {
		return -EINVAL;
	}

	md_fmt = (state->metadata_enabled) ? GMSL_CSI_DT_EMBED : 0x00;

#ifdef CONFIG_VIDEO_D5XX_SERDES
	data_type1 = sensor->config.format->data_type;
	data_type2 = md_fmt;
	is_calib = (state->is_y8 && (data_type1 == GMSL_CSI_DT_RGB_888));

	vc_id = state->g_ctx.dst_vc;
	dev_info(&state->client->dev,
		"d5x_configure: sensor=%s dt1=0x%x dt2=0x%x vc=%u pipe_id=%d\n",
		d5x_get_sensor_name(state), data_type1, data_type2, vc_id, sensor->pipe_id);
    if (PIPE_NOT_CONFIGURED == sensor->pipe_id ||
			sensor->pipe_data_type1 != data_type1 ||
			sensor->pipe_data_type2 != data_type2 ||
			sensor->pipe_vc_id != vc_id) {
		if (sensor->pipe_id >= 0) {
			mutex_lock(&serdes_lock__);
			ret = state->dser_ops->release_pipe(state->dser_dev, sensor->pipe_id);
			if (ret >= 0)
				d5x_disarm_dser_datapath_if_idle(state);
			mutex_unlock(&serdes_lock__);
			dev_warn(&state->client->dev, "release pipe %d (%d)\n", sensor->pipe_id, ret);
			sensor->pipe_reapply_gen = 0;
		}
		/*
		* Serialize SERDES pipe allocation and configuration
		* across all d4xx instances sharing the same GMSL link.
		* Without this, concurrent pipe setups race on the shared
		* Serializer/Deserializer hardware, causing I2C NACKs (-121) that
		* take down the entire bus.
		*/
		mutex_lock(&serdes_lock__);
		sensor->pipe_id =
			state->dser_ops->get_available_pipe_id(state->dser_dev, (int)state->g_ctx.dst_vc);
		mutex_unlock(&serdes_lock__);
		if (sensor->pipe_id < 0) {
			dev_err(&state->client->dev, "No free pipe in %s\n",state->dser_ops->name);
			return -ENOSR;
		}
		pipe_reapply_gen = d5x_dser_rearm_gen(state);
		sensor->pipe_reapply_gen = pipe_reapply_gen;
		mutex_lock(&serdes_lock__);
		ret = d5x_setup_pipeline(state, data_type1, data_type2,
					 sensor->pipe_id, vc_id);
		/* reset data path when switching to Y12I */
		if (is_calib)
			state->dser_ops->reset_oneshot(state->dser_dev);
		mutex_unlock(&serdes_lock__);
		if (ret < 0) {
			dev_err(&state->client->dev,
				"pipe %d setup FAILED (dt1=0x%x dt2=0x%x vc=%u) ret=%d\n",
				sensor->pipe_id, data_type1, data_type2, vc_id, ret);
			return ret;
		}
		dev_info(&state->client->dev,
				"pipe %d new  (dt1=0x%x dt2=0x%x vc=%u)\n",
				sensor->pipe_id, data_type1, data_type2, vc_id);
		sensor->pipe_data_type1 = data_type1;
		sensor->pipe_data_type2 = data_type2;
		sensor->pipe_vc_id = vc_id;
	} else {
		dev_info(&state->client->dev,
				"pipe %d already configured (dt1=0x%x dt2=0x%x vc=%u)\n",
				sensor->pipe_id, data_type1, data_type2, vc_id);
	}

	pipe_reapply_gen = d5x_dser_rearm_gen(state);
	if (sensor->pipe_reapply_gen != pipe_reapply_gen) {
		mutex_lock(&serdes_lock__);
		ret = d5x_setup_pipeline(state, data_type1, data_type2,
					 sensor->pipe_id, vc_id);
		mutex_unlock(&serdes_lock__);
		if (ret < 0) {
			dev_err(&state->client->dev,
				"pipe %d re-apply after DES re-arm FAILED (dt1=0x%x dt2=0x%x vc=%u gen=%u->%u) ret=%d\n",
				sensor->pipe_id, data_type1, data_type2, vc_id,
				sensor->pipe_reapply_gen, pipe_reapply_gen, ret);
			return ret;
		}
		dev_info(&state->client->dev,
			 "pipe %d re-applied after DES re-arm (dt1=0x%x dt2=0x%x vc=%u gen=%u)\n",
			 sensor->pipe_id, data_type1, data_type2, vc_id,
			 pipe_reapply_gen);
		sensor->pipe_reapply_gen = pipe_reapply_gen;
	} else {
		dev_dbg(&state->client->dev,
			"pipe %d already applied for DES re-arm gen %u (dt1=0x%x dt2=0x%x vc=%u)\n",
			sensor->pipe_id, pipe_reapply_gen, data_type1, data_type2, vc_id);
	}
#else /* Non-SERDES configuration */
	vc_id = (state->is_depth) ? 0 : (state->is_rgb) ? 1 : (state->is_y8) ? 2 : 3;
#endif

#if D5X_BYPASS_CAMERA_I2C
	/* Skip camera I2C config writes - camera is already streaming */
	return 0;
#else
	/* Determine desired data-type (special cases for depth/IR), then write
	 * it only when it differs from cached value. This avoids overwriting a
	 * correct DT with 0 (which caused INVALID_DT on subsequent attempts).
	 */
	dt_value = sensor->config.format->data_type;
	if (state->is_depth && dt_value != 0)
		dt_value = 0x31;
	else if (state->is_y8 && dt_value == GMSL_CSI_DT_YUV422_8)
		dt_value = 0x32;

	dev_dbg(&state->client->dev,
		"sensor %p: dt_value=0x%x, cached_dt_value=0x%x, cached_fps_value=%u, framerate=%u\n",
		sensor, dt_value, sensor->cached_dt_value, sensor->cached_fps_value, sensor->config.framerate);

	if (sensor->cached_dt_value != dt_value) {
		ret = d5x_write(state, dt_addr, dt_value);
		if (ret < 0)
			return ret;
		sensor->cached_dt_value = dt_value;
	}

	md_value = (vc_id << 8) | md_fmt;
	if (sensor->cached_md_value != md_value) {
		ret = d5x_write(state, md_addr, md_value);
		if (ret < 0)
			return ret;
		sensor->cached_md_value = md_value;
	}

	if (override_addr != 0) {
		dt_value = sensor->config.format->data_type;
		if (sensor->cached_override_value != dt_value) {
			ret = d5x_write(state, override_addr, dt_value);
			if (ret < 0)
				return ret;
			sensor->cached_override_value = dt_value;
		}
	}

	fps_value = sensor->config.framerate;
	if (sensor->cached_fps_value != fps_value) {
		ret = d5x_write(state, fps_addr, fps_value);
		if (ret < 0)
			return ret;
		sensor->cached_fps_value = fps_value;
	}

	width_value = sensor->config.resolution->width;
	if (sensor->cached_width_value != width_value) {
		ret = d5x_write(state, width_addr, width_value);
		if (ret < 0)
			return ret;
		sensor->cached_width_value = width_value;
	}

	height_value = sensor->config.resolution->height;
	if (sensor->cached_height_value != height_value) {
		ret = d5x_write(state, height_addr, height_value);
		if (ret < 0)
			return ret;
		sensor->cached_height_value = height_value;
	}

	return 0;
#endif /* !D5X_BYPASS_CAMERA_I2C */
}

static int d5x_sensor_g_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
		struct v4l2_subdev_state *state,
#endif
		struct v4l2_subdev_frame_interval *fi)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);

	if (NULL == sd || NULL == fi)
		return -EINVAL;

	fi->interval.numerator = 1;
	fi->interval.denominator = sensor->config.framerate;

	dev_dbg(sd->dev, "%s(): %s %u\n", __func__, sd->name,
			fi->interval.denominator);

	return 0;
}
static u16 __d5x_probe_framerate(const struct d5x_resolution *res, u16 target);

static int d5x_sensor_s_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
		struct v4l2_subdev_state *state,
#endif
		struct v4l2_subdev_frame_interval *fi)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);
	u16 framerate = 1;

	if (NULL == sd || NULL == fi || fi->interval.numerator == 0)
		return -EINVAL;

	framerate = fi->interval.denominator / fi->interval.numerator;
	framerate = __d5x_probe_framerate(sensor->config.resolution, framerate);
	sensor->config.framerate = framerate;
	fi->interval.numerator = 1;
	fi->interval.denominator = framerate;

	dev_dbg(sd->dev, "%s(): %s %u\n", __func__, sd->name, framerate);

	return 0;
}

static int d5x_sensor_s_stream(struct v4l2_subdev *sd, int on)
{
	struct d5x_sensor *sensor = container_of(sd, struct d5x_sensor, sd);

	dev_dbg(sensor->sd.dev, "%s(): sensor: name=%s state=%d\n",
		__func__, sensor->sd.name, on);

	sensor->streaming = on;

	return 0;
}

static const struct v4l2_subdev_video_ops d5x_sensor_video_ops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
	.g_frame_interval	= d5x_sensor_g_frame_interval,
	.s_frame_interval	= d5x_sensor_s_frame_interval,
#endif
	.s_stream		= d5x_sensor_s_stream,
};

static const struct v4l2_subdev_pad_ops d5x_pad_ops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	.get_frame_interval	= d5x_sensor_g_frame_interval,
	.set_frame_interval	= d5x_sensor_s_frame_interval,
#endif
	.enum_mbus_code		= d5x_sensor_enum_mbus_code,
	.enum_frame_size	= d5x_sensor_enum_frame_size,
	.enum_frame_interval	= d5x_sensor_enum_frame_interval,
	.get_fmt		= d5x_sensor_get_fmt,
	.set_fmt		= d5x_sensor_set_fmt,
};

static const struct v4l2_subdev_ops d5x_subdev_ops = {
	.pad = &d5x_pad_ops,
	.video = &d5x_sensor_video_ops,
};

/* InfraRed stream Y8/Y16 */

static int d5x_hw_set_auto_exposure(struct d5x *state, u32 base, s32 val)
{
	if (val != V4L2_EXPOSURE_APERTURE_PRIORITY &&
		val != V4L2_EXPOSURE_MANUAL)
		return -EINVAL;

	/*
	 * In firmware color auto exposure setting follow the uvc_menu_info
	 * exposure_auto_controls numbers, in drivers/media/usb/uvc/uvc_ctrl.c.
	 */
	if (state->is_rgb && val == V4L2_EXPOSURE_APERTURE_PRIORITY)
		val = 8;

	/*
	 * In firmware depth auto exposure on: 1, off: 0.
	 */
	if (!state->is_rgb) {
		if (val == V4L2_EXPOSURE_APERTURE_PRIORITY)
			val = 1;
		else if (val == V4L2_EXPOSURE_MANUAL)
			val = 0;
	}

	return d5x_write(state, base | D5X_AUTO_EXPOSURE_MODE, (u16)val);
}

/*
 * Manual exposure in us
 * Depth/Y8: between 100 and 200000 (200ms)
 * Color: between 100 and 1000000 (1s)
 */
static int d5x_hw_set_exposure(struct d5x *state, u32 base, s32 val)
{
	int ret = -1;

	if (val < 1)
		val = 1;
	if ((state->is_depth || state->is_y8) && val > MAX_DEPTH_EXP)
		val = MAX_DEPTH_EXP;
	if (state->is_rgb && val > MAX_RGB_EXP)
		val = MAX_RGB_EXP;

	/*
	 * Color and depth uses different unit:
	 *	Color: 1 is 100 us
	 *	Depth: 1 is 1 us
	 */

	ret = d5x_write(state, base | D5X_MANUAL_EXPOSURE_MSB, (u16)(val >> 16));
	if (!ret)
		ret = d5x_write(state, base | D5X_MANUAL_EXPOSURE_LSB,
				(u16)(val & 0xffff));

	return ret;
}

#define D5X_MAX_LOG_WAIT 200
#define D5X_MAX_LOG_SLEEP 10
#define D5X_MAX_LOG_POLL (D5X_MAX_LOG_WAIT / D5X_MAX_LOG_SLEEP)

#define D5X_CAMERA_CID_BASE	(V4L2_CTRL_CLASS_CAMERA | D5X_DEPTH_STREAM_DT)

#define D5X_CAMERA_CID_LOG			(D5X_CAMERA_CID_BASE+0)
#define D5X_CAMERA_CID_LASER_POWER		(D5X_CAMERA_CID_BASE+1)
#define D5X_CAMERA_CID_MANUAL_LASER_POWER	(D5X_CAMERA_CID_BASE+2)
#define D5X_CAMERA_DEPTH_CALIBRATION_TABLE_GET	(D5X_CAMERA_CID_BASE+3)
#define D5X_CAMERA_DEPTH_CALIBRATION_TABLE_SET	(D5X_CAMERA_CID_BASE+4)
#define D5X_CAMERA_COEFF_CALIBRATION_TABLE_GET	(D5X_CAMERA_CID_BASE+5)
#define D5X_CAMERA_COEFF_CALIBRATION_TABLE_SET	(D5X_CAMERA_CID_BASE+6)
#define D5X_CAMERA_CID_FW_VERSION		(D5X_CAMERA_CID_BASE+7)
#define D5X_CAMERA_CID_GVD			(D5X_CAMERA_CID_BASE+8)
#define D5X_CAMERA_CID_DEVICE_TYPE		(D5X_CAMERA_CID_BASE+23)
#define D5X_CAMERA_CID_AE_ROI_GET		(D5X_CAMERA_CID_BASE+9)
#define D5X_CAMERA_CID_AE_ROI_SET		(D5X_CAMERA_CID_BASE+10)
#define D5X_CAMERA_CID_AE_SETPOINT_GET		(D5X_CAMERA_CID_BASE+11)
#define D5X_CAMERA_CID_AE_SETPOINT_SET		(D5X_CAMERA_CID_BASE+12)
#define D5X_CAMERA_CID_ERB			(D5X_CAMERA_CID_BASE+13)
#define D5X_CAMERA_CID_EWB			(D5X_CAMERA_CID_BASE+14)
#define D5X_CAMERA_CID_HWMC			(D5X_CAMERA_CID_BASE+15)
#define D5X_CAMERA_CID_SYNC_MODE		(D5X_CAMERA_CID_BASE+16)

/*
 * D5xx HWM sync-mode values. Value 1 was the former RGB-master mode and is
 * intentionally unsupported; HKR accepts only Disabled, PWM Master, External.
 */
enum d5x_sync_mode {
	D5X_SYNC_MODE_DISABLED = 0,
	D5X_SYNC_MODE_RGB_MASTER_UNSUPPORTED = 1,
	D5X_SYNC_MODE_PWM_MASTER = 2,
	D5X_SYNC_MODE_EXTERNAL = 3,
};

/* 
 * The HWMC will remain for legacy tools compatibility,
 * HWMC_RW used for UVC compatibility
 */
#define D5X_CAMERA_CID_HWMC_RW		(D5X_CAMERA_CID_BASE+32)

/* HW reset with recovery for GMSL connections */
#define D5X_CAMERA_CID_HW_RESET		(D5X_CAMERA_CID_BASE+33)

#define D5X_HWMC_DATA			0x4900
#define D5X_HWMC_STATUS			0x4904
#define D5X_HWMC_RESP_LEN		0x4908
#define D5X_HWMC_EXEC			0x490C

#define D5X_HWMC_STATUS_OK		0
#define D5X_HWMC_STATUS_ERR		1
#define D5X_HWMC_STATUS_WIP		2
#define D5X_HWMC_BUFFER_SIZE	1024

#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
#define D5X_HWMC_RW_CTRL_FLAGS	(V4L2_CTRL_FLAG_VOLATILE | \
				 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE | \
				 V4L2_CTRL_FLAG_DYNAMIC_ARRAY)
#else
#define D5X_HWMC_RW_CTRL_FLAGS	(V4L2_CTRL_FLAG_VOLATILE | \
				 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE)
#endif

enum D5X_HWMC_ERR {
	D5X_HWMC_ERR_SUCCESS = 0,
	D5X_HWMC_ERR_CMD     = -1,
	D5X_HWMC_ERR_PARAM   = -6,
	D5X_HWMC_ERR_NODATA  = -21,
	D5X_HWMC_ERR_UNKNOWN = -64,
	D5X_HWMC_ERR_LAST,
};

static int d5x_hwmc_wait(struct d5x *state)
{
	int ret = 0;
	u16 status = D5X_HWMC_STATUS_WIP;
	int retries = 100;
	int errorCode;
	do {
		if (retries != 100)
			msleep_range(1);
		ret = d5x_read_poll(state, D5X_HWMC_STATUS, &status);
		if (ret) {
			dev_dbg(&state->client->dev,
				"%s(): I2C read failed (%d), retries left: %d\n",
				__func__, ret, retries);
		}
	} while (retries-- && (ret || status == D5X_HWMC_STATUS_WIP));
	dev_dbg(&state->client->dev,
		"%s(): ret: 0x%x, status: 0x%x\n",
		__func__, ret, status);
	if (!ret) {
		if (status == D5X_HWMC_STATUS_ERR) {
			d5x_raw_read(state, D5X_HWMC_DATA, &errorCode, sizeof(errorCode));
			ret = errorCode;
		} else if (status == D5X_HWMC_STATUS_WIP) {
			ret = -ETIMEDOUT;
			dev_warn(&state->client->dev,
				"%s(): HWMC command timed out\n", __func__);
		}
	} else {
		ret = D5X_HWMC_ERR_LAST;
	}
	return ret;
}

static int d5x_get_hwmc(struct d5x *state, unsigned char *data,
		u16 cmdDataLen, u16 *dataLen)
{
	int ret = 0;
	u16 tmp_len = 0;

	if (!data)
		return -ENOBUFS;

	memset(data, 0, cmdDataLen);
	ret = d5x_hwmc_wait(state);
	if (ret) {
		dev_dbg(&state->client->dev,
			"%s(): HWMC status not clear, ret: %d\n",
			__func__, ret);
		if (ret != D5X_HWMC_ERR_LAST) {
			int *p = (int *)data;
			*p = ret;
			return 0;
		} else {
			return ret;
		}
	}

	ret = d5x_raw_read(state, D5X_HWMC_RESP_LEN,
			&tmp_len, sizeof(tmp_len)); /* Read response length */
	if (ret)
		return -EBADMSG;

	if (tmp_len > cmdDataLen)
		return -ENOBUFS;

	if (tmp_len == 0) {
		dev_err(&state->client->dev,
			"%s(): HWMC response length is 0\n", __func__);
		return -ENODATA;
	}

	dev_dbg(&state->client->dev,
			"%s(): HWMC read len: %d, lrs_len: %d\n",
			__func__, tmp_len, tmp_len - 4);

	d5x_raw_read_with_check(state, D5X_HWMC_DATA, data, tmp_len); /* Read response data */
	if (dataLen)
		*dataLen = tmp_len;
	return ret;
}

static int d5x_hwmc_send(struct d5x *state,
			u16 cmdLen,
			const struct hwm_cmd *cmd)
{
	dev_dbg(&state->client->dev,
			"%s(): HWMC header: 0x%x, magic: 0x%x, opcode: 0x%x, "
			"cmdLen: %d, param1: %d, param2: %d, param3: %d, param4: %d\n",
			__func__, cmd->header, cmd->magic_word, cmd->opcode,
			cmdLen,	cmd->param1, cmd->param2, cmd->param3, cmd->param4);

	d5x_raw_write_with_check(state, D5X_HWMC_DATA, cmd, cmdLen); /* Write command data */

	d5x_write_with_check(state, D5X_HWMC_EXEC, 0x01); /* execute cmd */

	return 0;
}

static int d5x_hwmc_set_sync_mode(struct d5x *state, u16 sync_mode)
{
	struct hwm_cmd cmd = {
		.header = 0x14,
		.magic_word = 0xCDAB,
		.opcode = D5X_HWMC_OPCODE_SET_CAM_SYNC,
		.param1 = sync_mode,
	};
	int ret;

	if (sync_mode == D5X_SYNC_MODE_RGB_MASTER_UNSUPPORTED ||
	    sync_mode > D5X_SYNC_MODE_EXTERNAL)
		return -EINVAL;

	ret = d5x_hwmc_send(state, sizeof(cmd), &cmd);
	if (!ret)
		ret = d5x_hwmc_wait(state);

	return ret;
}

static int d5x_hwmc_get_sync_mode(struct d5x *state, u16 *sync_mode)
{
	struct hwm_cmd cmd = {
		.header = 0x14,
		.magic_word = 0xCDAB,
		.opcode = D5X_HWMC_OPCODE_GET_CAM_SYNC,
	};
	unsigned char response[6];
	u16 response_len = 0;
	__le16 sync_mode_le;
	int ret;

	if (!sync_mode)
		return -EINVAL;

	ret = d5x_hwmc_send(state, sizeof(cmd), &cmd);
	if (ret)
		return ret;

	ret = d5x_get_hwmc(state, response, sizeof(response), &response_len);
	if (ret)
		return ret;
	if (response_len < sizeof(response))
		return -EBADMSG;

	memcpy(&sync_mode_le, response + 4, sizeof(sync_mode_le));
	*sync_mode = le16_to_cpu(sync_mode_le);
	if (*sync_mode == D5X_SYNC_MODE_RGB_MASTER_UNSUPPORTED ||
	    *sync_mode > D5X_SYNC_MODE_EXTERNAL)
		return -ERANGE;

	return 0;
}

#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
static int d5x_prime_hwmc_rw_ctrl(struct v4l2_ctrl *ctrl)
{
	size_t bytes;
	void *buffer;

	if (!ctrl || !ctrl->is_array || !ctrl->is_dyn_array)
		return -EINVAL;

	bytes = D5X_HWMC_BUFFER_SIZE * ctrl->elem_size;
	buffer = kvzalloc(2 * bytes, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	kvfree(ctrl->p_array);
	ctrl->p_array = buffer;
	ctrl->p_new.p = buffer;
	ctrl->p_cur.p = buffer + bytes;
	ctrl->p_array_alloc_elems = D5X_HWMC_BUFFER_SIZE;
	ctrl->elems = D5X_HWMC_BUFFER_SIZE;
	ctrl->new_elems = D5X_HWMC_BUFFER_SIZE;

	return 0;
}
#endif

static u16 d5x_hwmc_rw_buf_len(const struct v4l2_ctrl *ctrl)
{
#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
	if (ctrl->is_dyn_array && ctrl->p_array_alloc_elems)
		return ctrl->p_array_alloc_elems;
#endif
	return ctrl->dims[0];
}

static void d5x_fix_empty_uamg_response(struct d5x *state,
		unsigned char *data, u16 buf_len, u16 *data_len)
{
	if (!data || !data_len || *data_len != 4 || buf_len <= 4)
		return;

	if (data[0] != D5X_HWMC_OPCODE_UAMG || data[1] || data[2] || data[3])
		return;

	/*
	 * D5xx MIPI firmware can acknowledge UAMG with just the opcode header.
	 * librealsense expects a one-byte payload with the advanced-mode state,
	 * so synthesize a disabled response to preserve device enumeration.
	 */
	data[4] = 0;
	*data_len = 5;
	dev_dbg(&state->client->dev,
		"%s(): synthesized disabled payload for header-only UAMG response\n",
		__func__);
}

static int d5x_set_calibration_data(struct d5x *state,
		const struct hwm_cmd *cmd, u16 length)
{
	int ret;

	ret = d5x_hwmc_send(state, length, cmd);
	if (ret)
		return ret;

	ret = d5x_hwmc_wait(state);
	if (ret) {
		dev_err(&state->client->dev,
				"%s(): Failed to set calibration table %d, error: %d\n",
				__func__, cmd->param1, ret);
	}

	return ret;
}

/* HW reset timeout and polling parameters */
#define D5X_HW_RESET_INITIAL_DELAY_MS	500
#define D5X_HW_RESET_POLL_INTERVAL_MS	200
#define D5X_HW_RESET_TIMEOUT_MS			10000
#define D5X_HW_RESET_MAX_RETRIES	(D5X_HW_RESET_TIMEOUT_MS / D5X_HW_RESET_POLL_INTERVAL_MS)

/* 
 * Minimum interval between consecutive HW resets (ms).
 * Rapid back-to-back resets degrade the GMSL link.
 */
#define D5X_HW_RESET_COOLDOWN_MS		2000

/* Reset readiness handshake:
 * 1) write scratch value before reset,
 * 2) wait for FW to restore control-status registers to default 0.
 */
#define D5X_HW_RESET_READY_SCRATCH_VAL	0x00AD
#define D5X_HW_RESET_READY_EXPECTED_VAL	0x0000

/*
 * Register holding DFU magic (0x5020).
 * In non-DFU mode this register is not defined.
 * - 0x04030201: Device in DFU mode (DFU magic bytes, little-endian)
 */
#define D5X_DFU_MAGIC_REG				0x5020
#define D5X_DFU_MAGIC_LSW				0x0201

static int d5x_wait_device_type(struct d5x *state, u16 *dev_type)
{
	int ret = -ETIMEDOUT;
	int retry;
	u16 cached_type;
	u16 probed_type = D5X_DEVICE_TYPE_UNKNOWN;

	for (retry = 0; retry < D5X_HW_RESET_MAX_RETRIES;
	     retry++, msleep(D5X_HW_RESET_POLL_INTERVAL_MS)) {
		cached_type = READ_ONCE(state->d5x_dev->cached_device_type);
		if (d5x_is_valid_device_type(cached_type)) {
			*dev_type = cached_type;
			return 0;
		}

		ret = d5x_read_poll(state, D5X_DEVICE_TYPE, &probed_type);
		if (!ret && d5x_is_valid_device_type(probed_type)) {
			WRITE_ONCE(state->d5x_dev->cached_device_type, probed_type);
			*dev_type = probed_type;
			return 0;
		}
	}

	*dev_type = probed_type;
	return ret ? ret : -ETIMEDOUT;
}

static void d5x_reset_streaming_flags(struct d5x_dev *d5x_dev)
{
	mutex_lock(&d5x_dev->lock);
	d5x_dev->depth_streaming = false;
	d5x_dev->ir_streaming = false;
	d5x_dev->rgb_streaming = false;
	d5x_dev->imu_streaming = false;
	mutex_unlock(&d5x_dev->lock);
}

#ifdef CONFIG_VIDEO_D5XX_SERDES
static void d5x_release_serdes_pipe(struct d5x *state,
				    struct d5x_sensor *sensor,
				    const char *reason)
{
	int ret;

	if (!sensor || sensor->pipe_id < 0)
		return;

	mutex_lock(&serdes_lock__);
	ret = state->dser_ops->release_pipe(state->dser_dev, sensor->pipe_id);
	if (ret < 0) {
		dev_warn(&state->client->dev,
			 "release pipe %d failed after %s (%d)\n",
			 sensor->pipe_id, reason, ret);
	} else {
		d5x_disarm_dser_datapath_if_idle(state);
		sensor->pipe_id = PIPE_NOT_CONFIGURED;
		sensor->pipe_reapply_gen = 0;
	}
	mutex_unlock(&serdes_lock__);
}
#endif

static int d5x_set_ser_esync_tunneling(struct d5x *state, bool enable)
{
#ifdef CONFIG_VIDEO_D5XX_SERDES
	struct d5x *owner;
	int ret;

	if (!state || !state->client || !state->d5x_dev)
		return -EINVAL;

	owner = state->d5x_dev->d5x_primary ? state->d5x_dev->d5x_primary : state;
	if (!owner || !owner->client || !owner->ser_dev || !owner->dser_ops)
		return -EINVAL;
	if (owner->dser_ops != &max96724_interface)
		return 0;

	dev_info(&state->client->dev,
		"%s(): serializer GPIO tunnel %s requested via %s owner %s\n",
		__func__, enable ? "enable" : "disable",
		dev_name(&state->client->dev), dev_name(&owner->client->dev));

	if (enable)
		ret = max96717_setup_gpio_tunneling(owner->ser_dev);
	else
		ret = max96717_disable_gpio_tunneling(owner->ser_dev);

	if (ret)
		dev_warn(&state->client->dev,
			"%s(): serializer GPIO tunnel %s failed (%d)\n",
			__func__, enable ? "enable" : "disable", ret);
	else
		dev_info(&state->client->dev,
			"%s(): serializer GPIO tunnel %s OK\n",
			__func__, enable ? "enable" : "disable");

	return ret;
#else
	return 0;
#endif
}

static int d5x_set_des_fsync(struct d5x *state, bool enable)
{
#ifdef CONFIG_VIDEO_D5XX_SERDES
	struct d5x *owner;
	u32 fps = 0;
	int ret = 0;

	if (!state || !state->client || !state->d5x_dev)
		return -EINVAL;

	owner = state->d5x_dev->d5x_primary ? state->d5x_dev->d5x_primary : state;
	if (!owner || !owner->client || !owner->dser_dev || !owner->dser_ops)
		return -EINVAL;
	if (owner->dser_ops != &max96724_interface)
		return 0;

	if (state->mux.last_set)
		fps = state->mux.last_set->config.framerate;
	if (!fps)
		fps = state->depth.sensor.config.framerate;

	dev_info(&state->client->dev,
		"%s(): deserializer internal FSYNC %s requested via %s owner %s, fps=%u\n",
		__func__, enable ? "enable" : "disable",
		dev_name(&state->client->dev), dev_name(&owner->client->dev), fps);

	if (enable) {
		if (owner->dser_ops->setup_fsync)
			ret = owner->dser_ops->setup_fsync(owner->dser_dev, fps);
	} else if (owner->dser_ops->disable_fsync) {
		ret = owner->dser_ops->disable_fsync(owner->dser_dev);
	}

	if (ret)
		dev_warn(&state->client->dev,
			"%s(): deserializer internal FSYNC %s failed (%d)\n",
			__func__, enable ? "enable" : "disable", ret);
	else
		dev_info(&state->client->dev,
			"%s(): deserializer internal FSYNC %s OK\n",
			__func__, enable ? "enable" : "disable");

	return ret;
#else
	return 0;
#endif
}

static bool d5x_sync_mode_uses_esync(struct d5x *state)
{
	int sync_mode = D5X_SYNC_MODE_DISABLED;

	if (!state)
		return false;

	if (state->d5x_dev)
		sync_mode = READ_ONCE(state->d5x_dev->sync_mode);
	if (sync_mode == D5X_SYNC_MODE_DISABLED && state->ctrls.sync_mode)
		sync_mode = state->ctrls.sync_mode->cur.val;

	return sync_mode == D5X_SYNC_MODE_EXTERNAL;
}

/*
 * d5x_hw_reset_with_recovery - Perform hardware reset with readiness polling
 * @state: Driver state structure
 *
 * Sends a hardware reset command to the D4XX device and waits for it to
 * come back online.  Before resetting, stops active streams and invalidates
 * all driver-side sensor state (streaming flags, SERDES pipes, config cache).
 * After the device responds, waits for DEVICE_TYPE to become valid (GMSL
 * link recovery).  Per-pipe SERDES reconfiguration is deferred to
 * d5x_configure() at the next stream start.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int d5x_hw_reset_with_recovery(struct d5x *state)
{
	int ret;
	int retry;
	u16 dev_type = D5X_DEVICE_TYPE_UNKNOWN;
	u16 ready_status = 0;
	u16 ready_reg = state->control_status_reg;
	struct hwm_cmd reset_cmd;
	bool depth_streaming;
	bool rgb_streaming;
	bool ir_streaming;
	bool imu_streaming;
	unsigned long d5x_last_reset_jiffies = READ_ONCE(state->d5x_dev->last_reset_jiffies);
	unsigned long ts, timeout;

	dev_info(&state->client->dev, "%s(): Initiating HW reset with recovery\n",
		__func__);

	/* 0. Reset cooldown — prevent rapid consecutive resets.
	 *    Repeated HW resets without sufficient recovery time
	 *    progressively degrade the GMSL link.  Enforce a minimum
	 *    interval between resets.
	 *    Skip check on the very first reset (d5x_last_reset_jiffies == 0).
	 */
	if (d5x_last_reset_jiffies) {
		unsigned long elapsed = jiffies - d5x_last_reset_jiffies;
		unsigned long cooldown = msecs_to_jiffies(D5X_HW_RESET_COOLDOWN_MS);

		if (time_before(jiffies, d5x_last_reset_jiffies + cooldown)) {
			unsigned long remaining = cooldown - elapsed;

			dev_info(&state->client->dev,
				"%s(): Reset cooldown — last reset %u ms ago, waiting %u ms\n",
				__func__, jiffies_to_msecs(elapsed),
				jiffies_to_msecs(remaining));
			msleep(jiffies_to_msecs(remaining));
		}
	}

	/* 1. Stop active streams on the device before reset.
	 *    This ensures FW and SERDES are in a clean state.
	 *
	 *    In the D4XX architecture each physical camera has 4 driver
	 *    instances (Depth, RGB, IR, IMU) sharing the same ser_dev.
	 *    HW reset kills all streams on the camera ASIC, so we must
	 *    stop and invalidate all peer instances of the same camera.
	 */
	dev_info(&state->client->dev, "%s(): stopping streams before reset\n", __func__);
	mutex_lock(&state->d5x_dev->lock);
	depth_streaming = state->d5x_dev->depth_streaming;
	rgb_streaming = state->d5x_dev->rgb_streaming;
	ir_streaming = state->d5x_dev->ir_streaming;
	imu_streaming = state->d5x_dev->imu_streaming;
	mutex_unlock(&state->d5x_dev->lock);

	if (depth_streaming)
		d5x_write(state, D5X_START_STOP_STREAM,	D5X_STREAM_STOP | D5X_STREAM_DEPTH);
	if (rgb_streaming)
		d5x_write(state, D5X_START_STOP_STREAM,	D5X_STREAM_STOP | D5X_STREAM_RGB);
	if (ir_streaming)
		d5x_write(state, D5X_START_STOP_STREAM,	D5X_STREAM_STOP | D5X_STREAM_IR);
	if (imu_streaming)
		d5x_write(state, D5X_START_STOP_STREAM,	D5X_STREAM_STOP | D5X_STREAM_IMU);

	/* 2. Increment D5xx reset generation.
	 *    After HW reset the device loses all configuration, so driver
	 *    state must be brought in sync, like clearing streaming flags so that
	 *    d5x_mux_s_stream() won't silently skip the next stream-start.
	 *    Also clear cached device type so post-reset readiness polling
	 *    cannot be satisfied by stale pre-reset values.
	 *    Covers this instance AND all peer instances of the same camera.
	 *
	 *    Do NOT release SERDES pipes here — the D5XX FW may still
	 *    reconfigure MAX96717 while reset completion propagates.
	 *    Releasing + re-allocating pipes now would race with FW init.
	 *    Instead, clear pipe_data_type to force d5x_configure() to
	 *    release-then-reallocate at stream-start time, when the FW
	 *    has long finished its init (matching v1.0.1.33 behavior).
	 */
	atomic_inc(d5x_get_reset_gen(state));
	WRITE_ONCE(state->d5x_dev->cached_device_type, D5X_DEVICE_TYPE_UNKNOWN);
	d5x_reset_streaming_flags(state->d5x_dev);
#ifdef CONFIG_VIDEO_D5XX_SERDES
	d5x_set_dser_datapath_armed(state, false);
	d5x_clear_dser_start_state(state);
#endif

	/* 3. Scratch one control-status register before reset.
	 *    FW restores them to default 0x0000 only after reset completes.
	 */
	if (!ready_reg)
		ready_reg = D5X_DEPTH_CONTROL_STATUS;

	ret = d5x_write(state, ready_reg, D5X_HW_RESET_READY_SCRATCH_VAL);
	if (ret) {
		dev_err(&state->client->dev,
			"%s(): scratch write failed reg 0x%04x (%d)\n",
			__func__, ready_reg, ret);
		return ret;
	}

	/* 4. Send HW reset command */
	memcpy(&reset_cmd, &cmd_hw_reset, sizeof(reset_cmd));
	ret = d5x_hwmc_send(state, sizeof(reset_cmd), &reset_cmd);
	if (ret < 0) {
		dev_err(&state->client->dev,
			"%s(): Failed to send HW reset command: %d\n",
			__func__, ret);
		return ret;
	}

	dev_info(&state->client->dev, "%s(): HW reset command sent, waiting for device...\n",
		__func__);

	/* 5. Delay to allow reset to complete */
	ts = jiffies;
	msleep(D5X_HW_RESET_INITIAL_DELAY_MS);

	/* 6. Poll for control-status defaults to confirm reset completion. */
	for (retry = 0, timeout = ts + msecs_to_jiffies(D5X_HW_RESET_TIMEOUT_MS);
			; retry++, msleep_range(D5X_HW_RESET_POLL_INTERVAL_MS)) {
		if (!time_before(jiffies, timeout)) {
			dev_err(&state->client->dev,
				"%s(): Device isn't ready after %d ms (last control-status: 0x%04x, i2c ret: %d)\n",
				__func__, jiffies_to_msecs(jiffies - ts), ready_status, ret);

			return -ETIMEDOUT;
		}

		ret = d5x_read_poll(state, ready_reg, &ready_status);
		if (ret < 0) {
			dev_dbg(&state->client->dev,
				"%s(): Device not responding (resetting), retry %d\n",
				__func__, retry);
			continue;
		}
		if (ready_status == D5X_HW_RESET_READY_EXPECTED_VAL) {
			dev_info(&state->client->dev,
				"%s(): Device ready after %d ms (control-status default restored)\n",
				__func__, jiffies_to_msecs(jiffies - ts));
			break;
		}

		ret = d5x_read_poll(state, D5X_DFU_MAGIC_REG, &ready_status);
		if (!ret && ready_status == D5X_DFU_MAGIC_LSW) {
			dev_warn(&state->client->dev,
				"%s(): Device in DFU/recovery mode after reset\n", __func__);
			state->dfu_dev.dfu_state_flag = D5X_DFU_RECOVERY;
			return 0;
		}
	}


	/* 7. Wait for DEVICE_TYPE to confirm GMSL link recovery.
	 *    Step 6 confirmed reset completion via control-status defaults.
	 *    Wait for DEVICE_TYPE: if the register becomes valid,
	 *    the GMSL link recovered naturally and the firmware progressed
	 *    far enough for format-dependent paths (the common case).
	 *
	 *    Do NOT call max96717_init_settings() here.  That function writes
	 *    global serializer registers (0x02, 0x308, 0x311, 0x331) that
	 *    disrupt the active GMSL link.  Per-pipe reconfiguration is
	 *    handled by d5x_configure()->d5x_setup_pipeline()
	 *    at the next STREAMON for each stream.
	 */
	ret = d5x_wait_device_type(state, &dev_type);
	if (ret < 0) {
		dev_err(&state->client->dev,
			"%s(): device type not ready after reset (ret=%d, val=0x%x)\n",
			__func__, ret, dev_type);
		return ret;
	}
	dev_info(&state->client->dev,
		"%s(): GMSL link recovered (device type 0x%04x)\n",
		__func__, dev_type);

	/* 8. Verify device is operational by reading firmware version */
	ret = d5x_read(state, D5X_FW_VERSION, &state->fw_version);
	if (ret < 0) {
		dev_err(&state->client->dev,
			"%s(): Failed to read firmware version: %d\n", __func__, ret);
		return ret;
	}

	ret = d5x_read(state, D5X_FW_BUILD, &state->fw_build);
	if (ret < 0) {
		dev_err(&state->client->dev,
			"%s(): Failed to read firmware build: %d\n", __func__, ret);
		return ret;
	}

	dev_info(&state->client->dev,
		"%s(): HW reset complete. Device type 0x%04x, firmware: %d.%d.%d.%d\n",
		__func__,
		dev_type,
		(state->fw_version >> 8) & 0xff, state->fw_version & 0xff,
		(state->fw_build >> 8) & 0xff, state->fw_build & 0xff);

	/*
	 * ESYNC / FSYNC hardware is NOT re-applied here after a HW
	 * reset.  The cached sync_mode control is a pure policy flag;
	 * the actual SERDES programming happens at the next
	 * stream-on boundary (d5x_mux_s_stream).
	 */

	WRITE_ONCE(state->d5x_dev->last_reset_jiffies, jiffies);

	return 0;
}

static int d5x_mux_s_stream(struct v4l2_subdev *sd, int on);

#if defined(CONFIG_TEGRA_CAMERA_PLATFORM) && defined(CONFIG_TEGRA_EMBEDDED_METADATA_OPS)
/*
 * D5xx CSI metadata carries an HKRM transport header. The kernel only
 * validates the framing needed to publish the actual sidecar bytesused;
 * semantic metadata parsing stays in userspace.
 */
static size_t d5x_csi_metadata_bytesused(const u8 *data, size_t captured_size)
{
	u16 payload_size;
	size_t bytesused;

	if (!data || captured_size < D5X_CSI_METADATA_HEADER_SIZE)
		return 0;

	if (get_unaligned_le32(data) != D5X_CSI_METADATA_MAGIC)
		return 0;

	payload_size = get_unaligned_le16(data + D5X_CSI_METADATA_PAYLOAD_OFFSET);
	bytesused = D5X_CSI_METADATA_HEADER_SIZE + payload_size;
	if (!payload_size || bytesused > captured_size)
		return 0;

	return bytesused;
}

static const struct tegra_embedded_metadata_ops d5x_csi_metadata_ops = {
	.dataformat = V4L2_META_FMT_RSMD,
	.compat_dataformat = V4L2_META_FMT_D4XX,
	.max_buffer_size = D5X_CSI_METADATA_MAX_WC,
	.get_bytesused = d5x_csi_metadata_bytesused,
};
#endif

static int d5x_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct d5x *state = container_of(ctrl->handler, struct d5x,
					 ctrls.handler);
	struct v4l2_subdev *sd = &state->mux.sd.subdev;
	struct d5x_sensor *sensor = (struct d5x_sensor *)ctrl->priv;
	int ret = -EINVAL;
	u16 base;

	if (sensor) {
		switch (sensor->mux_pad) {
		case D5X_MUX_PAD_DEPTH:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_depth);
			break;
		case D5X_MUX_PAD_RGB:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_rgb);
			break;
		case D5X_MUX_PAD_IR:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_y8);
			break;
		case D5X_MUX_PAD_IMU:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_imu);
			break;
		default:
			break;
		}
	}

	base = state->control_base;
	v4l2_dbg(3, 1, sd, "ctrl: %s, value: %d\n", ctrl->name, ctrl->val);
	dev_dbg(&state->client->dev, "%s(): %s - ctrl: %s, value: %d\n",
		__func__, d5x_get_sensor_name(state), ctrl->name, ctrl->val);

	mutex_lock(&state->lock);

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = d5x_write(state, base | D5X_MANUAL_GAIN, ctrl->val);
		break;

	case V4L2_CID_EXPOSURE_AUTO:
		ret = d5x_hw_set_auto_exposure(state, base, ctrl->val);
		break;

	case V4L2_CID_EXPOSURE_ABSOLUTE:
		ret = d5x_hw_set_exposure(state, base, ctrl->val);
		break;
	case D5X_CAMERA_CID_LASER_POWER:
		if (!state->is_rgb)
			ret = d5x_write(state, base | D5X_LASER_POWER,
					ctrl->val);
		break;
	case D5X_CAMERA_CID_MANUAL_LASER_POWER:
		if (!state->is_rgb)
			ret = d5x_write(state, base | D5X_MANUAL_LASER_POWER,
					ctrl->val);
		break;
	case D5X_CAMERA_DEPTH_CALIBRATION_TABLE_SET:
		dev_dbg(&state->client->dev,
			"%s(): D5X_CAMERA_DEPTH_CALIBRATION_TABLE_SET \n",	__func__);
		if (ctrl->p_new.p) {
			struct hwm_cmd *calib_cmd;
			dev_dbg(&state->client->dev,
				"%s(): table id: 0x%x\n",
				__func__, *((u8 *)ctrl->p_new.p + 2));
			if (DEPTH_CALIBRATION_ID == *((u8 *)ctrl->p_new.p + 2)) {
				calib_cmd = devm_kzalloc(&state->client->dev,
					sizeof(struct hwm_cmd) + 256, GFP_KERNEL);
				if (!calib_cmd) {
					dev_err(&state->client->dev,
						"%s(): Can't allocate memory for 0x%x\n",
						__func__, ctrl->id);
					ret = -ENOMEM;
					break;
				}
				memcpy(calib_cmd, &set_calib_data, sizeof(set_calib_data));
				calib_cmd->header = 276;
				calib_cmd->param1 = DEPTH_CALIBRATION_ID;
				memcpy(calib_cmd->Data, (u8 *)ctrl->p_new.p, 256);
				ret = d5x_set_calibration_data(state, calib_cmd,
					sizeof(struct hwm_cmd) + 256);
				devm_kfree(&state->client->dev, calib_cmd);
			}
		}
		break;
	case D5X_CAMERA_COEFF_CALIBRATION_TABLE_SET:
			dev_dbg(&state->client->dev,
				"%s(): D5X_CAMERA_COEFF_CALIBRATION_TABLE_SET \n",
				__func__);
			if (ctrl->p_new.p) {
				struct hwm_cmd *calib_cmd;
				dev_dbg(&state->client->dev,
					"%s(): table id %d\n",
					__func__, *((u8 *)ctrl->p_new.p + 2));
				if (COEF_CALIBRATION_ID == *((u8 *)ctrl->p_new.p + 2)) {
					calib_cmd = devm_kzalloc(&state->client->dev,
						sizeof(struct hwm_cmd) + 512, GFP_KERNEL);
					if (!calib_cmd) {
						dev_err(&state->client->dev,
							"%s(): Can't allocate memory for 0x%x\n",
							__func__, ctrl->id);
						ret = -ENOMEM;
						break;
					}
				memcpy(calib_cmd, &set_calib_data, sizeof (set_calib_data));
				calib_cmd->header = 532;
				calib_cmd->param1 = COEF_CALIBRATION_ID;
				memcpy(calib_cmd->Data, (u8 *)ctrl->p_new.p, 512);
				ret = d5x_set_calibration_data(state, calib_cmd,
						sizeof(struct hwm_cmd) + 512);
				devm_kfree(&state->client->dev, calib_cmd);
			}
		}
		break;
	case D5X_CAMERA_CID_AE_ROI_SET:
		if (ctrl->p_new.p_u16) {
			struct hwm_cmd ae_roi_cmd;
			memcpy(&ae_roi_cmd, &set_ae_roi, sizeof(ae_roi_cmd));
			ae_roi_cmd.param1 = *((u16 *)ctrl->p_new.p_u16);
			ae_roi_cmd.param2 = *((u16 *)ctrl->p_new.p_u16 + 1);
			ae_roi_cmd.param3 = *((u16 *)ctrl->p_new.p_u16 + 2);
			ae_roi_cmd.param4 = *((u16 *)ctrl->p_new.p_u16 + 3);
			ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd),
				&ae_roi_cmd);
			if (!ret)
				ret = d5x_hwmc_wait(state);
		}
		break;
	case D5X_CAMERA_CID_AE_SETPOINT_SET:
		if (ctrl->p_new.p_s32) {
			struct hwm_cmd *ae_setpoint_cmd;
			dev_dbg(&state->client->dev, "%s():0x%x \n",
				__func__, *(ctrl->p_new.p_s32));
			ae_setpoint_cmd = devm_kzalloc(&state->client->dev,
					sizeof(struct hwm_cmd) + 4, GFP_KERNEL);
			if (!ae_setpoint_cmd) {
				dev_err(&state->client->dev,
					"%s(): Can't allocate memory for 0x%x\n",
					__func__, ctrl->id);
				ret = -ENOMEM;
				break;
			}
			memcpy(ae_setpoint_cmd, &set_ae_setpoint, sizeof (set_ae_setpoint));
			memcpy(ae_setpoint_cmd->Data, (u8 *)ctrl->p_new.p_s32, 4);
			ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd) + 4,
					ae_setpoint_cmd);
			if (!ret)
				ret = d5x_hwmc_wait(state);
			devm_kfree(&state->client->dev, ae_setpoint_cmd);
		}
		break;
	case D5X_CAMERA_CID_ERB:
		if (ctrl->p_new.p_u8) {
			u16 offset = 0;
			u16 size = 0;
			u16 len = 0;
			struct hwm_cmd *erb_cmd;

			offset = *(ctrl->p_new.p_u8) << 8;
			offset |= *(ctrl->p_new.p_u8 + 1);
			size = *(ctrl->p_new.p_u8 + 2) << 8;
			size |= *(ctrl->p_new.p_u8 + 3);

			dev_dbg(&state->client->dev, "%s(): offset %x, size: %x\n",
							__func__, offset, size);
			len = sizeof(struct hwm_cmd) + size;
			erb_cmd = devm_kzalloc(&state->client->dev,	len, GFP_KERNEL);
			if (!erb_cmd) {
				dev_err(&state->client->dev,
					"%s(): Can't allocate memory for 0x%x\n",
					__func__, ctrl->id);
				ret = -ENOMEM;
				break;
			}
			memcpy(erb_cmd, &erb, sizeof(struct hwm_cmd));
			erb_cmd->param1 = offset;
			erb_cmd->param2 = size;
			ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd), erb_cmd);
			if (!ret)
				ret = d5x_get_hwmc(state, erb_cmd->Data, len, &size);
			if (ret) {
				dev_err(&state->client->dev,
					"%s(): ERB cmd failed, ret: %d,"
					"requested size: %d, actual size: %d\n",
					__func__, ret, erb_cmd->param2, size);
				devm_kfree(&state->client->dev, erb_cmd);
				return -EAGAIN;
			}

			// Actual size returned from FW
			*(ctrl->p_new.p_u8 + 2) = (size & 0xFF00) >> 8;
			*(ctrl->p_new.p_u8 + 3) = (size & 0x00FF);

			memcpy(ctrl->p_new.p_u8 + 4, erb_cmd->Data + 4, size - 4);
			dev_dbg(&state->client->dev, "%s(): 0x%x 0x%x 0x%x 0x%x \n",
				__func__,
				*(ctrl->p_new.p_u8),
				*(ctrl->p_new.p_u8+1),
				*(ctrl->p_new.p_u8+2),
				*(ctrl->p_new.p_u8+3));
			devm_kfree(&state->client->dev, erb_cmd);
		}
		break;
	case D5X_CAMERA_CID_EWB:
		if (ctrl->p_new.p_u8) {
			u16 offset = 0;
			u16 size = 0;
			struct hwm_cmd *ewb_cmd;

			offset = *((u8 *)ctrl->p_new.p_u8) << 8;
			offset |= *((u8 *)ctrl->p_new.p_u8 + 1);
			size = *((u8 *)ctrl->p_new.p_u8 + 2) << 8;
			size |= *((u8 *)ctrl->p_new.p_u8 + 3);

			dev_dbg(&state->client->dev, "%s():0x%x 0x%x 0x%x 0x%x\n",
					__func__,
					*((u8 *)ctrl->p_new.p_u8),
					*((u8 *)ctrl->p_new.p_u8 + 1),
					*((u8 *)ctrl->p_new.p_u8 + 2),
					*((u8 *)ctrl->p_new.p_u8 + 3));

			ewb_cmd = devm_kzalloc(&state->client->dev,
					sizeof(struct hwm_cmd) + size,
					GFP_KERNEL);
			if (!ewb_cmd) {
				dev_err(&state->client->dev,
					"%s(): Can't allocate memory for 0x%x\n",
					__func__, ctrl->id);
				ret = -ENOMEM;
				break;
			}
			memcpy(ewb_cmd, &ewb, sizeof(ewb));
			ewb_cmd->header = 0x14 + size;
			ewb_cmd->param1 = offset; // start index
			ewb_cmd->param2 = size; // size
			memcpy(ewb_cmd->Data, (u8 *)ctrl->p_new.p_u8 + 4, size);
			ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd) + size, ewb_cmd);
			if (!ret)
				ret = d5x_hwmc_wait(state);
			if (ret) {
				dev_err(&state->client->dev,
					"%s(): EWB cmd failed, ret: %d,"
					"requested size: %d, actual size: %d\n",
					__func__, ret, ewb_cmd->param2, size);
				devm_kfree(&state->client->dev, ewb_cmd);
				return -EAGAIN;
			}

			devm_kfree(&state->client->dev, ewb_cmd);
		}
		break;
	case D5X_CAMERA_CID_HWMC:
		if (ctrl->p_new.p_u8) {
			u16 size = 0;
			struct hwm_cmd *cmd = (struct hwm_cmd *)ctrl->p_new.p_u8;
			size = *((u8 *)ctrl->p_new.p_u8 + 1) << 8;
			size |= *((u8 *)ctrl->p_new.p_u8 + 0);
			ret = d5x_hwmc_send(state, size + 4, cmd);
			ret = d5x_get_hwmc(state, cmd->Data, ctrl->dims[0], &size);
			if (ctrl->dims[0] < D5X_HWMC_BUFFER_SIZE) {
				ret = -ENODATA;
				break;
			}
			/*This is needed for legacy hwmc */
			size += 4;
			cmd->Data[1000] = (unsigned char)((size) & 0x00FF);
			cmd->Data[1001] = (unsigned char)(((size) & 0xFF00) >> 8);
		}
		break;
	case D5X_CAMERA_CID_HWMC_RW:
		if (ctrl->p_new.p_u8) {
			struct hwm_cmd *cmd = (struct hwm_cmd *)ctrl->p_new.p_u8;
			u16 size = *((u8 *)ctrl->p_new.p_u8 + 1) << 8;
			u16 buf_len = d5x_hwmc_rw_buf_len(ctrl);
			size |= *((u8 *)ctrl->p_new.p_u8 + 0);

			dev_info(&state->client->dev,
				"%s(): HWMC_RW set size=%u total=%u buf_len=%u opcode=0x%x first8=%*ph\n",
				__func__, size, size + 4, buf_len, cmd->opcode, 8,
				ctrl->p_new.p_u8);

#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
			if (ctrl->is_dyn_array && size + 4 > ctrl->new_elems) {
				dev_err(&state->client->dev,
					"%s(): HWMC_RW len %u exceeds payload %u\n",
					__func__, size + 4, ctrl->new_elems);
				ret = -EINVAL;
				break;
			}
#endif

			if (size + 4 > buf_len) {
				dev_err(&state->client->dev,
					"%s(): HWMC_RW len %u exceeds buffer %u\n",
					__func__, size + 4, buf_len);
				ret = -EMSGSIZE;
				break;
			}

			/* Check if this is a HW reset command (opcode 0x20) */
			if (cmd->opcode == 0x20) {
				dev_info(&state->client->dev,
					"%s(): HW reset detected via HWMC_RW, using recovery path\n",
					__func__);
				ret = d5x_hw_reset_with_recovery(state);
			} else {
				ret = d5x_hwmc_send(state, size + 4, cmd);
			}

#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
			if (!ret && ctrl->is_dyn_array)
				ctrl->new_elems = buf_len;
#endif
		}
		break;
	case D5X_CAMERA_CID_HW_RESET:
		dev_info(&state->client->dev, "%s(): HW reset requested via V4L2 control\n",
			__func__);
		ret = d5x_hw_reset_with_recovery(state);
		break;
	case D5X_CAMERA_CID_SYNC_MODE:
		dev_info(&state->client->dev, "%s(): HWM SYNC_MODE control received, value: %d\n",
			__func__, ctrl->val);
		if (state->is_depth) {
			ret = d5x_hwmc_set_sync_mode(state, ctrl->val);
			dev_info(&state->client->dev,
				"%s(): HWM SET_CAM_SYNC value: %d, ret: %d\n",
				__func__, ctrl->val, ret);
			if (!ret) {
				bool need_esync = ctrl->val == D5X_SYNC_MODE_EXTERNAL;

				if (state->d5x_dev)
					WRITE_ONCE(state->d5x_dev->sync_mode, ctrl->val);

				/*
				 * Match D4xx behavior for the serializer side:
				 * make GPIO tunneling visible immediately after
				 * camera_sync_mode changes.  The DES internal
				 * FSYNC generator still starts at stream-on so it
				 * can use the negotiated FPS.
				 */
				ret = d5x_set_ser_esync_tunneling(state, need_esync);
				if (!ret && !need_esync)
					ret = d5x_set_des_fsync(state, false);
			}
		}
		break;
	}

	mutex_unlock(&state->lock);

	return ret;
}

static int d5x_get_calibration_data(struct d5x *state, enum table_id id,
		unsigned char *table, unsigned int length)
{
	struct hwm_cmd *cmd;
	int ret;
	u16 table_length;

	cmd = devm_kzalloc(&state->client->dev,
			sizeof(struct hwm_cmd) + length + 4, GFP_KERNEL);
	if (!cmd) {
		dev_err(&state->client->dev, "%s(): Can't allocate memory\n", __func__);
		return -ENOMEM;
	}

	memcpy(cmd, &get_calib_data, sizeof(get_calib_data));
	cmd->param1 = id;
	ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd), cmd);
	if (ret) {
		devm_kfree(&state->client->dev, cmd);
		return ret;
	}

	ret = d5x_hwmc_wait(state);

	if (ret) {
		dev_err(&state->client->dev,
				"%s(): Failed to get calibration table %d, error: %d\n",
				__func__, id, ret);
		devm_kfree(&state->client->dev, cmd);
		return ret;
	}

	/* get table length from fw */
	ret = d5x_raw_read(state, D5X_HWMC_RESP_LEN,
			&table_length, sizeof(table_length));

	/* read table */
	d5x_raw_read_with_check(state, D5X_HWMC_DATA, cmd->Data, table_length);

	/* first 4 bytes are opcode HWM, not part of calibration table */
	memcpy(table, cmd->Data + 4, length);
	devm_kfree(&state->client->dev, cmd);
	return 0;
}

static int d5x_gvd(struct d5x *state, unsigned char *data)
{
	struct hwm_cmd cmd;
	int ret;
	u16 length = 0;

	memcpy(&cmd, &gvd, sizeof(gvd));
	ret = d5x_hwmc_send(state, sizeof(cmd), &cmd);
	if (ret)
		return ret;

	ret = d5x_hwmc_wait(state);
	if (ret) {
		dev_err(&state->client->dev,
			"%s(): Failed to read GVD, error: %d\n",
			__func__, ret);
		return -EIO;
	}
	/* Read response length */
	ret = d5x_raw_read(state, D5X_HWMC_RESP_LEN, &length, sizeof(length));
	/* Read response data */
	d5x_raw_read_with_check(state, D5X_HWMC_DATA, data, length);

	return ret;
}

static int d5x_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct d5x *state = container_of(ctrl->handler, struct d5x,
			ctrls.handler);
			
	u32 data;
	int ret = 0;
	struct d5x_sensor *sensor = (struct d5x_sensor *)ctrl->priv;
	u16 base;
	u16 reg;

	if (sensor) {
		switch (sensor->mux_pad) {
		case D5X_MUX_PAD_DEPTH:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_depth);
			break;
		case D5X_MUX_PAD_RGB:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_rgb);
			break;
		case D5X_MUX_PAD_IR:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_y8);
			break;
		case D5X_MUX_PAD_IMU:
			state = container_of(ctrl->handler, struct d5x, ctrls.handler_imu);
			break;
		default:
			break;
		}
	}
	base = state->control_base;

	dev_dbg(&state->client->dev, "%s(): %s - ctrl: %s \n",
		__func__, d5x_get_sensor_name(state), ctrl->name);

	switch (ctrl->id) {

	case V4L2_CID_ANALOGUE_GAIN:
		if (state->is_imu)
			return -EINVAL;
		ret = d5x_read(state, base | D5X_MANUAL_GAIN, ctrl->p_new.p_u16);
		break;

	case V4L2_CID_EXPOSURE_AUTO:
		if (state->is_imu)
			return -EINVAL;
		d5x_read(state, base | D5X_AUTO_EXPOSURE_MODE, &reg);
		*ctrl->p_new.p_u16 = reg;
		/* see d5x_hw_set_auto_exposure */
		if (!state->is_rgb) {
			if (reg == 1)
				*ctrl->p_new.p_u16 = V4L2_EXPOSURE_APERTURE_PRIORITY;
			else if (reg == 0)
				*ctrl->p_new.p_u16 = V4L2_EXPOSURE_MANUAL;
		}

		if (state->is_rgb && reg == 8)
			*ctrl->p_new.p_u16 = V4L2_EXPOSURE_APERTURE_PRIORITY;

		break;

	case V4L2_CID_EXPOSURE_ABSOLUTE:
		if (state->is_imu)
			return -EINVAL;
		/* see d5x_hw_set_exposure */
		d5x_read(state, base | D5X_MANUAL_EXPOSURE_MSB, &reg);
		data = ((u32)reg << 16) & 0xffff0000;
		d5x_read(state, base | D5X_MANUAL_EXPOSURE_LSB, &reg);
		data |= reg;
		*ctrl->p_new.p_u32 = data;
		break;

	case D5X_CAMERA_CID_LASER_POWER:
		if (!state->is_rgb)
			d5x_read(state, base | D5X_LASER_POWER, ctrl->p_new.p_u16);
		break;

	case D5X_CAMERA_CID_MANUAL_LASER_POWER:
		if (!state->is_rgb)
			d5x_read(state, base | D5X_MANUAL_LASER_POWER, ctrl->p_new.p_u16);
		break;

	case D5X_CAMERA_CID_LOG:
		ret = d5x_hwmc_send(state, sizeof(log_prepare), &log_prepare);
		if (ret)
			return ret;

		ret = d5x_hwmc_wait(state);
		if (ret)
			return ret;

		/* Read response length */
		ret = d5x_raw_read(state, D5X_HWMC_RESP_LEN, &data, sizeof(data));
		dev_dbg(&state->client->dev, "%s(): log size 0x%x\n", __func__, data);
		if (ret < 0)
			return ret;
		if (!data)
			return 0;
		if (data > 1024)
			return -ENOBUFS;
		ret = d5x_raw_read(state, D5X_HWMC_DATA,
				ctrl->p_new.p_u8, data);
		break;
	case D5X_CAMERA_DEPTH_CALIBRATION_TABLE_GET:
		ret = d5x_get_calibration_data(state, DEPTH_CALIBRATION_ID,
				ctrl->p_new.p_u8, 256);
		break;
	case D5X_CAMERA_COEFF_CALIBRATION_TABLE_GET:
		ret = d5x_get_calibration_data(state, COEF_CALIBRATION_ID,
				ctrl->p_new.p_u8, 512);
		break;
	case D5X_CAMERA_CID_FW_VERSION:
		ret = d5x_read(state, D5X_FW_VERSION, &state->fw_version);
		ret = d5x_read(state, D5X_FW_BUILD, &state->fw_build);
		*ctrl->p_new.p_u32 = state->fw_version << 16;
		*ctrl->p_new.p_u32 |= state->fw_build;
		break;
	case D5X_CAMERA_CID_DEVICE_TYPE: {
		u16 dev_type = D5X_DEVICE_TYPE_UNKNOWN;

		ret = d5x_read(state, D5X_DEVICE_TYPE, &dev_type);
		dev_type = d5x_dev_type(state, dev_type);
		if (ret && !d5x_is_valid_device_type(dev_type))
			break;
		if (d5x_is_valid_device_type(dev_type))
			WRITE_ONCE(state->d5x_dev->cached_device_type, dev_type);
		*ctrl->p_new.p_u32 = dev_type;
		ret = 0;
		break;
	}
	case D5X_CAMERA_CID_GVD:
		ret = d5x_gvd(state, ctrl->p_new.p_u8);
		break;
	case D5X_CAMERA_CID_AE_ROI_GET:
		if (ctrl->p_new.p_u16) {
			u16 len = sizeof(struct hwm_cmd) + 12;
			u16 dataLen = 0;
			struct hwm_cmd *ae_roi_cmd;
			ae_roi_cmd = devm_kzalloc(&state->client->dev, len, GFP_KERNEL);
			if (!ae_roi_cmd) {
				dev_err(&state->client->dev,
					"%s(): Can't allocate memory for 0x%x\n",
					__func__, ctrl->id);
				ret = -ENOMEM;
				break;
			}
			memcpy(ae_roi_cmd, &get_ae_roi, sizeof(struct hwm_cmd));
			ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd), ae_roi_cmd);
			if (ret) {
				devm_kfree(&state->client->dev, ae_roi_cmd);
				return ret;
			}
			ret = d5x_get_hwmc(state, ae_roi_cmd->Data, len, &dataLen);
			if (!ret && dataLen <= ctrl->dims[0])
				memcpy(ctrl->p_new.p_u16, ae_roi_cmd->Data + 4, 8);
			devm_kfree(&state->client->dev, ae_roi_cmd);
		}
		break;
	case D5X_CAMERA_CID_AE_SETPOINT_GET:
	if (ctrl->p_new.p_s32) {
		u16 len = sizeof(struct hwm_cmd) + 8;
		u16 dataLen = 0;
		struct hwm_cmd *ae_setpoint_cmd;
		ae_setpoint_cmd = devm_kzalloc(&state->client->dev,	len, GFP_KERNEL);
		if (!ae_setpoint_cmd) {
			dev_err(&state->client->dev,
					"%s(): Can't allocate memory for 0x%x\n",
					__func__, ctrl->id);
			ret = -ENOMEM;
			break;
		}
		memcpy(ae_setpoint_cmd, &get_ae_setpoint, sizeof(struct hwm_cmd));
		ret = d5x_hwmc_send(state, sizeof(struct hwm_cmd), ae_setpoint_cmd);
		if (ret) {		
			devm_kfree(&state->client->dev, ae_setpoint_cmd);
			return ret;
		}
		ret = d5x_get_hwmc(state, ae_setpoint_cmd->Data, len, &dataLen);
		memcpy(ctrl->p_new.p_s32, ae_setpoint_cmd->Data + 4, 4);
		dev_dbg(&state->client->dev, "%s(): len: %d, 0x%x \n",
			__func__, dataLen, *(ctrl->p_new.p_s32));
		devm_kfree(&state->client->dev, ae_setpoint_cmd);
		}
		break;
	case D5X_CAMERA_CID_HWMC_RW:
		if (ctrl->p_new.p_u8) {
			unsigned char *data = (unsigned char *)ctrl->p_new.p_u8;
			u16 dataLen = 0;
			u16 bufLen = d5x_hwmc_rw_buf_len(ctrl);
			ret = d5x_get_hwmc(state, data,	bufLen, &dataLen);
			if (!ret)
				d5x_fix_empty_uamg_response(state, data, bufLen, &dataLen);
			/* This is needed for librealsense, to align there code with UVC,
		 	 * last word is length - 4 bytes header length */
			if (dataLen >= 4)
				dataLen -= 4;
			else
				dataLen = 0;
			data[bufLen - 4] = (unsigned char)(dataLen & 0x00FF);
			data[bufLen - 3] = (unsigned char)((dataLen & 0xFF00) >> 8);
			data[bufLen - 2] = 0;
			data[bufLen - 1] = 0;
		}
		break;
	case D5X_CAMERA_CID_SYNC_MODE:
		if (state->is_depth && ctrl->p_new.p_u16) {
			u16 sync_mode;

			ret = d5x_hwmc_get_sync_mode(state, &sync_mode);
			if (!ret) {
				*ctrl->p_new.p_u16 = sync_mode;
				if (state->d5x_dev)
					WRITE_ONCE(state->d5x_dev->sync_mode, sync_mode);
			}
		}
		break;
	}
	return ret;
}

static const struct v4l2_ctrl_ops d5x_ctrl_ops = {
	.s_ctrl	= d5x_s_ctrl,
	.g_volatile_ctrl = d5x_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config d5x_ctrl_log = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_LOG,
	.name = "Logger",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {1024},
	.elem_size = sizeof(u8),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_laser_power = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_LASER_POWER,
	.name = "Laser power on/off",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 1,
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};

static const struct v4l2_ctrl_config d5x_ctrl_manual_laser_power = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_MANUAL_LASER_POWER,
	.name = "Manual laser power",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 360,
	.step = 30,
	.def = 150,
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};

static const struct v4l2_ctrl_config d5x_ctrl_fw_version = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_FW_VERSION,
	.name = "fw version",
	.type = V4L2_CTRL_TYPE_U32,
	.dims = {1},
	.elem_size = sizeof(u32),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_device_type = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_DEVICE_TYPE,
	.name = "device type",
	.type = V4L2_CTRL_TYPE_U32,
	.dims = {1},
	.elem_size = sizeof(u32),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_gvd = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_GVD,
	.name = "GVD",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {239},
	.elem_size = sizeof(u8),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_get_depth_calib = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_DEPTH_CALIBRATION_TABLE_GET,
	.name = "get depth calib",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {256},
	.elem_size = sizeof(u8),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_set_depth_calib = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_DEPTH_CALIBRATION_TABLE_SET,
	.name = "set depth calib",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {256},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_get_coeff_calib = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_COEFF_CALIBRATION_TABLE_GET,
	.name = "get coeff calib",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {512},
	.elem_size = sizeof(u8),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_set_coeff_calib = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_COEFF_CALIBRATION_TABLE_SET,
	.name = "set coeff calib",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {512},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_ae_roi_get = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_AE_ROI_GET,
	.name = "ae roi get",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {8},
	.elem_size = sizeof(u16),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_ae_roi_set = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_AE_ROI_SET,
	.name = "ae roi set",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {8},
	.elem_size = sizeof(u16),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_ae_setpoint_get = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_AE_SETPOINT_GET,
	.name = "ae setpoint get",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
	.min = 0,
	.max = 4095,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_ae_setpoint_set = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_AE_SETPOINT_SET,
	.name = "ae setpoint set",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 4095,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config d5x_ctrl_erb = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_ERB,
	.name = "ERB eeprom read",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {1020},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_ewb = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_EWB,
	.name = "EWB eeprom write",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {1020},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_hwmc = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_HWMC,
	.name = "HWMC",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {D5X_HWMC_BUFFER_SIZE + 4},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
	.step = 1,
};

static const struct v4l2_ctrl_config d5x_ctrl_hwmc_rw = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_HWMC_RW,
	.name = "HWMC_RW",
	.type = V4L2_CTRL_TYPE_U8,
	.dims = {D5X_HWMC_BUFFER_SIZE},
	.elem_size = sizeof(u8),
	.min = 0,
	.max = 0xFFFFFFFF,
	.def = 240,
	.step = 1,
	.flags = D5X_HWMC_RW_CTRL_FLAGS,
};

static const struct v4l2_ctrl_config d5x_ctrl_hw_reset = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_HW_RESET,
	.name = "HW Reset",
	.type = V4L2_CTRL_TYPE_BUTTON,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
	.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};

/* Sync mode menu arrays for different camera platforms */
static const char * const sync_mode_menu[] = {
	[D5X_SYNC_MODE_DISABLED] = "Disabled",
	[D5X_SYNC_MODE_RGB_MASTER_UNSUPPORTED] = "(unsupported)",
	[D5X_SYNC_MODE_PWM_MASTER] = "PWM Master",
	[D5X_SYNC_MODE_EXTERNAL] = "External",
};

static struct v4l2_ctrl_config d5x_ctrl_sync_mode = {
	.ops = &d5x_ctrl_ops,
	.id = D5X_CAMERA_CID_SYNC_MODE,
	.name = "Camera Sync Mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = D5X_SYNC_MODE_DISABLED,
	.max = D5X_SYNC_MODE_EXTERNAL,
	.def = D5X_SYNC_MODE_DISABLED,
	.qmenu = sync_mode_menu,
	.menu_skip_mask = BIT(D5X_SYNC_MODE_RGB_MASTER_UNSUPPORTED),
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};
static int d5x_mux_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct d5x *state = v4l2_get_subdevdata(sd);

	dev_dbg(sd->dev, "%s(): %s (%p)\n", __func__, sd->name, fh);

	mutex_lock(&state->lock);
	if (state->dfu_dev.dfu_state_flag)
	{
		mutex_unlock(&state->lock);
		return -EBUSY;
	}

	state->dfu_dev.device_open_count++;
	mutex_unlock(&state->lock);

	return 0;
};

static int d5x_mux_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct d5x *state = v4l2_get_subdevdata(sd);

	dev_dbg(sd->dev, "%s(): %s (%p)\n", __func__, sd->name, fh);
	mutex_lock(&state->lock);
	state->dfu_dev.device_open_count--;
	mutex_unlock(&state->lock);
	return 0;
};

static const struct v4l2_subdev_internal_ops d5x_sensor_internal_ops = {
	.open = d5x_mux_open,
	.close = d5x_mux_close,
};

static void d5x_init_d5x_dev(struct d5x *state, struct d5x_dev *d5x_dev)
{
	state->d5x_dev = d5x_dev;
	mutex_lock(&d5x_dev->lock);
	d5x_dev->d5x_primary = state;
	d5x_dev->cached_device_type = D5X_DEVICE_TYPE_UNKNOWN;
	d5x_dev->sync_mode = D5X_SYNC_MODE_DISABLED;
	mutex_unlock(&d5x_dev->lock);
	d5x_reset_streaming_flags(d5x_dev);
}

#ifdef CONFIG_VIDEO_D5XX_SERDES
static int d5x_setup_and_link(struct d5x *state)
{
	int i;
	int err = 0;
	struct device *dev = &state->client->dev;

	mutex_lock(&serdes_lock__);
	d5x_init_global_slots_once();
	state->d5x_dev = NULL;
	/* Look for existing D5xx instances. */
	for (i = 0; i < MAX_D5X_NUM; i++) {
		bool match;

		mutex_lock(&d5x_inited[i].lock);
		match = d5x_inited[i].d5x_primary &&
			d5x_inited[i].d5x_primary->ser_dev == state->ser_dev;
		mutex_unlock(&d5x_inited[i].lock);
		if (match) { /* Same camera, different stream instance. */
			state->serdes_primary = false;
			state->d5x_dev = &d5x_inited[i];
			break;
		}
	}
	if (NULL == state->d5x_dev) {
		/* First stream instance for this camera: set up and link a new D5xx. */
		for (i = 0; i < MAX_D5X_NUM; i++) {
			bool free_slot;

			mutex_lock(&d5x_inited[i].lock);
			free_slot = (NULL == d5x_inited[i].d5x_primary);
			mutex_unlock(&d5x_inited[i].lock);
			if (free_slot) {
				int j;
				d5x_init_d5x_dev(state, &d5x_inited[i]);
				state->serdes_primary = true;
				/* Look for matching deserializer */
				state->d5x_dev->dser_control = NULL;
				for (j = 0; j < MAX_DSER_NUM; j++) {
					mutex_lock(&dser_inited[j].lock);
					if (dser_inited[j].dser_dev == state->dser_dev)
						state->d5x_dev->dser_control = &dser_inited[j];
					mutex_unlock(&dser_inited[j].lock);
					if (state->d5x_dev->dser_control)
						break;
				}
				if (NULL == state->d5x_dev->dser_control) {
				/* Setup and link new deserializer */
					for (j = 0; j < MAX_DSER_NUM; j++) {
						mutex_lock(&dser_inited[j].lock);
						if (NULL == dser_inited[j].dser_dev) {
							dser_inited[j].dser_dev = state->dser_dev;
							dser_inited[j].datapath_armed = false;
							state->d5x_dev->dser_control = &dser_inited[j];
						}
						mutex_unlock(&dser_inited[j].lock);
						if (state->d5x_dev->dser_control)
							break;
					}
				}
				if (NULL == state->d5x_dev->dser_control) {
					dev_err(dev, "cannot handle more than %d deserializers\n", MAX_DSER_NUM);
					err = -ENOSPC;
					goto out_unlock;
				} else {
					dev_info(dev, "Deserializer %s linked\n", dev_name(state->dser_dev));
				}
				break;
			}
		}
	}
	if (NULL == state->d5x_dev) {
		err = -ENOSPC;
		dev_err(dev, "cannot handle more than %d D5xx cameras\n", MAX_D5X_NUM);
	}

out_unlock:
	mutex_unlock(&serdes_lock__);
	return err;
}

/*
 * FIXME
 * temporary solution before changing GMSL data structure or merging all 4 D457
 * sensors into one i2c device. Only first sensor node per max96717 sets up the
 * link.
 */
#ifdef CONFIG_OF
static int d5x_board_setup(struct d5x *state)
{
	struct device *dev = &state->client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *ser_node;
	struct i2c_client *ser_i2c = NULL;
	struct device_node *dser_node;
	struct i2c_client *dser_i2c = NULL;
	struct device_node *gmsl;
	int value = 0xFFFF;
	const char *str_value;
	int err;

	state->g_ctx.sdev_reg = state->client->addr;

	err = of_property_read_u32(node, "def-addr",
					&state->g_ctx.sdev_def);
	if (err < 0) {
		dev_err(dev, "def-addr not found\n");
		goto error;
	}

	ser_node = of_parse_phandle(node, "maxim,gmsl-ser-device", 0);
	if (ser_node == NULL) {
		/* check compatibility with jetpack */
		ser_node = of_parse_phandle(node, "nvidia,gmsl-ser-device", 0);
		if (ser_node == NULL) {
			dev_err(dev, "missing %s handle\n", "[maxim|nvidia],gmsl-ser-device");
			goto error;
		}
	}
	err = of_property_read_u32(ser_node, "reg", &state->g_ctx.ser_reg);
	dev_dbg(dev,  "serializer reg: 0x%x\n", state->g_ctx.ser_reg);
	if (err < 0) {
		dev_err(dev, "serializer reg not found\n");
		goto error;
	}

	ser_i2c = of_find_i2c_device_by_node(ser_node);
	of_node_put(ser_node);

	if (ser_i2c == NULL) {
		err = -EPROBE_DEFER;
		goto error;
	}
	if (ser_i2c->dev.driver == NULL) {
		dev_err(dev, "missing serializer driver\n");
		goto error;
	}

	state->ser_dev = &ser_i2c->dev;

	dser_node = of_parse_phandle(node, "maxim,gmsl-dser-device", 0);
	if (dser_node == NULL) {
		dser_node = of_parse_phandle(node, "nvidia,gmsl-dser-device", 0);
		if (dser_node == NULL) {
			dev_err(dev, "missing %s handle\n", "[maxim|nvidia],gmsl-dser-device");
			goto error;
		}
	}

	dser_i2c = of_find_i2c_device_by_node(dser_node);

	if (dser_i2c == NULL) {
		err = -EPROBE_DEFER;
		goto error;
	}
	if (dser_i2c->dev.driver == NULL) {
		dev_err(dev, "missing deserializer driver\n");
		goto error;
	}

	state->dser_dev = &dser_i2c->dev;
	/* Initialize deserializer interface */
	if (!strcmp(dser_node->name, "max96724")) {
		state->dser_ops = &max96724_interface;
	} else {
		dev_err(dev, "%s: Unsupported deserializer = %s\n", __func__, dser_node->name);
		state->dser_ops = &max96724_interface;
		goto error;
	}
	dev_info(dev, "Using deserializer %s\n", state->dser_ops->name);
	of_node_put(dser_node);

	/* populate g_ctx from DT */
	gmsl = of_get_child_by_name(node, "gmsl-link");
	if (gmsl == NULL) {
		dev_err(dev, "missing gmsl-link device node\n");
		err = -EINVAL;
		goto error;
	}

	err = of_property_read_string(gmsl, "dst-csi-port", &str_value);
	if (err < 0) {
		dev_err(dev, "No dst-csi-port found\n");
		goto error;
	}
	state->g_ctx.dst_csi_port =
		(!strcmp(str_value, "a")) ? GMSL_CSI_PORT_A : GMSL_CSI_PORT_B;

	err = of_property_read_string(gmsl, "src-csi-port", &str_value);
	if (err < 0) {
		dev_err(dev, "No src-csi-port found\n");
		goto error;
	}
	state->g_ctx.src_csi_port =
		(!strcmp(str_value, "a")) ? GMSL_CSI_PORT_A : GMSL_CSI_PORT_B;

	err = of_property_read_string(gmsl, "csi-mode", &str_value);
	if (err < 0) {
		dev_err(dev, "No csi-mode found\n");
		goto error;
	}

	if (!strcmp(str_value, "1x4")) {
		state->g_ctx.csi_mode = GMSL_CSI_1X4_MODE;
	} else if (!strcmp(str_value, "2x4")) {
		state->g_ctx.csi_mode = GMSL_CSI_2X4_MODE;
	} else if (!strcmp(str_value, "4x2")) {
		state->g_ctx.csi_mode = GMSL_CSI_4X2_MODE;
	} else if (!strcmp(str_value, "2x2")) {
		state->g_ctx.csi_mode = GMSL_CSI_2X2_MODE;
	} else {
		dev_err(dev, "invalid csi mode\n");
		goto error;
	}

	err = of_property_read_string(gmsl, "serdes-csi-link", &str_value);
	if (err < 0) {
		dev_err(dev, "No serdes-csi-link found\n");
		goto error;
	}
	state->g_ctx.serdes_csi_link =
		(!strcmp(str_value, "a")) ?
			GMSL_SERDES_CSI_LINK_A : GMSL_SERDES_CSI_LINK_B;

	err = of_property_read_u32(gmsl, "st-vc", &value);
	if (err < 0) {
		dev_err(dev, "No st-vc info\n");
		goto error;
	}
	state->g_ctx.st_vc = value;

	err = of_property_read_u32(gmsl, "vc-id", &value);
	if (err < 0) {
		dev_err(dev, "No vc-id info\n");
		goto error;
	}
	state->g_ctx.dst_vc = value;

	err = of_property_read_u32(gmsl, "num-lanes", &value);
	if (err < 0) {
		dev_err(dev, "No num-lanes info\n");
		goto error;
	}
	state->g_ctx.num_csi_lanes = value;
	state->g_ctx.s_dev = dev;

	err = d5x_setup_and_link(state);
error:
	return err;
}
#else /* CONFIG_OF */

static int d5x_board_setup(struct d5x *state)
{
	struct device *dev = &state->client->dev;
	struct d4xx_pdata *pdata = dev->platform_data;
	struct i2c_adapter *adapter = state->client->adapter;
	int bus = adapter->nr;
	int err = 0;
	int i;
	char suffix = pdata->suffix;
	static struct max96717_pdata max96717_pdata = {
		.is_prim_ser = 1, // todo: configurable
		.def_addr = 0x40, // todo: configurable
	};

	static struct max96724_pdata max96724_pdata = {
		.max_src = 2,
		.csi_mode = GMSL_CSI_2X4_MODE,
	};
	static struct i2c_board_info i2c_info_des = {
		I2C_BOARD_INFO("max96724", 0x27),
		.platform_data = &max96724_pdata,
	};
	static struct i2c_board_info i2c_info_ser = {
		I2C_BOARD_INFO("max96717", 0x60),
		.platform_data = &max96717_pdata,
	};

	i2c_info_ser.addr = pdata->subdev_info[0].ser_alias;
	state->ser_i2c = i2c_new_client_device(adapter, &i2c_info_ser);

	i2c_info_des.addr = pdata->subdev_info[0].board_info.addr;

	/* look for already registered max96724, use same context if found */
	mutex_lock(&serdes_lock__);
	d5x_init_global_slots_once();
	for (i = 0; i < MAX_D5X_NUM; i++) {
		struct d5x *primary;

		mutex_lock(&d5x_inited[i].lock);
		primary = d5x_inited[i].d5x_primary;
		if (primary && primary->dser_i2c) {
			dev_info(dev, "MAX96724 found device on %d@0x%x\n",
				primary->dser_i2c->adapter->nr, primary->dser_i2c->addr);
			if (bus == primary->dser_i2c->adapter->nr
				&& primary->dser_i2c->addr == i2c_info_des.addr) {
				dev_info(dev, "MAX96724 AGGREGATION found device on 0x%x\n", i2c_info_des.addr);
				state->dser_i2c = primary->dser_i2c;
				state->aggregated = 1;
			}
		}
		mutex_unlock(&d5x_inited[i].lock);
	}
	mutex_unlock(&serdes_lock__);
	if (state->aggregated)
		suffix += 4;
	dev_info(dev, "Init SerDes %c on %d@0x%x<->%d@0x%x\n",
		suffix,
		bus, pdata->subdev_info[0].board_info.addr, //48
		bus, pdata->subdev_info[0].ser_alias); //42

	if (!state->dser_i2c)
		state->dser_i2c = i2c_new_client_device(adapter, &i2c_info_des);

	if (state->ser_i2c == NULL) {
		err = -EPROBE_DEFER;
		dev_err(dev, "missing serializer client\n");
		goto error;
	}
	if (state->ser_i2c->dev.driver == NULL) {
		err = -EPROBE_DEFER;
		dev_err(dev, "missing serializer driver\n");
		goto error;
	}
	if (state->dser_i2c == NULL) {
		err = -EPROBE_DEFER;
		dev_err(dev, "missing deserializer client\n");
		goto error;
	}
	if (state->dser_i2c->dev.driver == NULL) {
		err = -EPROBE_DEFER;
		dev_err(dev, "missing deserializer driver\n");
		goto error;
	}

	state->g_ctx.sdev_reg = state->client->addr;
	state->g_ctx.sdev_def = 0x10;// def-addr TODO: configurable
	/* Address reassignment for d5xx-a 0x10->0x12 */
	dev_info(dev, "Address reassignment for %s-%c 0x%x->0x%x\n",
		pdata->subdev_info[0].board_info.type, suffix,
		state->g_ctx.sdev_def, state->g_ctx.sdev_reg);

	state->g_ctx.ser_reg = pdata->subdev_info[0].ser_alias;
	dev_info(dev,  "serializer: i2c-%d@0x%x\n",
		state->ser_i2c->adapter->nr, state->g_ctx.ser_reg);

	if (err < 0) {
		dev_err(dev, "serializer reg not found\n");
		goto error;
	}

	state->ser_dev = &state->ser_i2c->dev;

	dev_info(dev,  "deserializer: i2c-%d@0x%x\n",
		state->dser_i2c->adapter->nr, state->dser_i2c->addr);


	state->dser_dev = &state->dser_i2c->dev;
	/* Initialize deserializer interface */
	state->dser_ops = &max96724_interface;


	/* populate g_ctx from pdata */
	state->g_ctx.dst_csi_port = GMSL_CSI_PORT_A;
	state->g_ctx.src_csi_port = GMSL_CSI_PORT_B;
	state->g_ctx.csi_mode = GMSL_CSI_1X4_MODE;
	if (state->aggregated) {
		dev_info(dev,  "configure GMSL port B\n");
		state->g_ctx.serdes_csi_link = GMSL_SERDES_CSI_LINK_B;
	} else {
		dev_info(dev,  "configure GMSL port A\n");
		state->g_ctx.serdes_csi_link = GMSL_SERDES_CSI_LINK_A;
	}
	state->g_ctx.st_vc = 0;
	state->g_ctx.dst_vc = 0;

	state->g_ctx.num_csi_lanes = 2;
	state->g_ctx.s_dev = dev;

	err = d5x_setup_and_link(state);
error:
	return err;
}
#endif /* CONFIG_OF */

static int d5x_gmsl_serdes_setup(struct d5x *state)
{
	int err = 0;
	int des_err = 0;
	struct device *dev;

	if (!state || !state->ser_dev || !state->dser_dev || !state->client)
		return -EINVAL;

	dev = &state->client->dev;

	mutex_lock(&serdes_lock__);

	/*
	 * Skip power cycle: On Orin AGX the CAM0_RST_L GPIO is shared
	 * with the I2C bus pull-up domain. Asserting reset hangs the
	 * entire I2C-2 bus (TCA9548 + MAX96724 become unreachable).
	 * The MAX96724 is already responsive from probe; just proceed
	 * with link setup directly.
	 */
	msleep(100);

	dev_dbg(dev, "Setup SERDES addressing and control pipeline\n");
	/* setup serdes addressing and control pipeline */
	err = state->dser_ops->setup_link(state->dser_dev, &state->client->dev);
	if (err) {
		dev_err(dev, "gmsl deserializer link config failed\n");
		goto error;
	}
	/*
	 * After GMSL link locks, I2C pass-through to the
	 * remote serializer requires settling time before the first I2C
	 * write is ACKed.  Shorter delays result in -121 (EREMOTEIO).
	 */
	msleep(1000);
	err = max96717_setup_control(state->ser_dev);
	if (err) {
		dev_err(dev, "gmsl serializer setup failed\n");
		goto error;
	}
	state->ser_control_setup = true;

	/*
	 * Configure serializer registers (PIPE_EN,
	 * EXT11 tunnel mode, FRONTTOP) BEFORE the deserializer one-shot
	 * reset.  The one-shot in setup_control(dser) briefly disrupts
	 * the GMSL link; if init_settings runs after it, the first
	 * I2C write to the serializer NACKs with -121.
	 */
	msleep(200);
	err = max96717_init_settings(state->ser_dev);
	if (err) {
		dev_warn(dev, "max96717 init settings failed\n");
		/* non-fatal: tunnel mode + pipes will be set in setup_streaming */
		err = 0;
	}

	des_err = state->dser_ops->setup_control(state->dser_dev, &state->client->dev);
	if (des_err) {
		dev_err(dev, "gmsl deserializer setup failed\n");
		err = des_err;
	} else {
		state->dser_control_setup = true;
	}

error:
	mutex_unlock(&serdes_lock__);
	return err;
}

static void d5x_serdes_cleanup(struct d5x *state)
{
	int ret;
	bool do_cleanup = false;
	bool power_off = false;
	struct device *dev;

	if (!state || !state->serdes_primary)
		return;

	dev = &state->client->dev;

	mutex_lock(&serdes_lock__);
	if (state->d5x_dev) {
		mutex_lock(&state->d5x_dev->lock);
		if (state->d5x_dev->d5x_primary == state) {
			state->d5x_dev->d5x_primary = NULL;
			do_cleanup = true;
		}
		mutex_unlock(&state->d5x_dev->lock);
	}

	if (do_cleanup) {
		if (state->ser_control_setup && state->ser_dev) {
			power_off = true;
			ret = max96717_reset_control(state->ser_dev);
			if (ret)
				dev_warn(dev, "failed in 96717 reset control\n");
			state->ser_control_setup = false;
		}

		if (state->dser_control_setup &&
		    state->dser_ops && state->dser_dev) {
			power_off = true;
			ret = state->dser_ops->reset_control(state->dser_dev,
							    state->g_ctx.s_dev);
			if (ret)
				dev_warn(dev, "failed in %s reset control\n",
					 state->dser_ops->name);
			state->dser_control_setup = false;
		}

		if (state->ser_dev) {
			ret = max96717_sdev_unpair(state->ser_dev,
						   state->g_ctx.s_dev);
			if (ret)
				dev_warn(dev, "failed to unpair sdev\n");
		}

		if (state->dser_ops && state->dser_dev) {
			ret = state->dser_ops->sdev_unregister(state->dser_dev,
							      state->g_ctx.s_dev);
			if (ret)
				dev_warn(dev, "failed to %s unregister sdev\n",
					 state->dser_ops->name);
		}

		if (power_off && state->dser_ops && state->dser_dev)
			state->dser_ops->power_off(state->dser_dev);
	}
	mutex_unlock(&serdes_lock__);

	state->serdes_primary = false;
}

static int d5x_serdes_setup(struct d5x *state)
{
	int ret = 0;
	struct i2c_client *c = state->client;

	ret = d5x_board_setup(state);
	if (ret) {
		dev_err(&c->dev, "board setup failed\n");
		return ret;
	}

	/* 
	 * Peer instance of an already-initialized camera.
	 * d5x_setup_and_link() found that another instance already set up
	 * this serializer and marked us non-primary.  Skip SERDES setup
	 * (pair, register, gmsl init) — the primary already did it.
	 */
	if (!state->serdes_primary) {
		dev_info(&c->dev, "peer instance, skipping SERDES setup\n");
		return 0;
	}

	/* Pair sensor to serializer dev */
	ret = max96717_sdev_pair(state->ser_dev, &state->g_ctx);
	if (ret) {
		dev_err(&c->dev, "gmsl ser pairing failed\n");
		goto serdes_setup_end;
	}

	/* Register sensor to deserializer dev */
	ret = state->dser_ops->sdev_register(state->dser_dev, &state->g_ctx);
	if (ret) {
		dev_err(&c->dev, "gmsl deserializer register failed\n");
		goto serdes_setup_end;
	}

	ret = d5x_gmsl_serdes_setup(state);
	if (ret) {
		dev_err(&c->dev, "%s gmsl serdes setup failed\n", __func__);
		goto serdes_setup_end;
	}
	/*
	 * max96717_init_settings is now called inside d5x_gmsl_serdes_setup,
	 * after setup_control(ser) but before setup_control(dser) to avoid
	 * the deserializer one-shot reset disrupting I2C to the serializer.
	 */

	ret = state->dser_ops->init_settings(state->dser_dev);
	if (ret) {
		dev_warn(&c->dev, "%s, failed to init %s settings\n",
			__func__, state->dser_ops->name);
		goto serdes_setup_end;
	}

	/*
	 * Setup CSI output pipeline on deserializer
	 * and enable tunnel mode + CSI input on serializer.
	 * These calls configure PHY, DPLL, lane mapping, CSI output enable,
	 * and serializer tunnel mode — without them, no CSI output occurs.
	 */
	ret = max96724_setup_streaming(state->dser_dev, state->g_ctx.s_dev);
	if (ret) {
		dev_warn(&c->dev, "%s, failed to setup max96724 streaming\n",
			__func__);
		goto serdes_setup_end;
	}
	
	/*
	 * max96724_setup_streaming issues a one-shot reset
	 * (reg 0x0018=0x0F) which briefly disrupts the GMSL link.
	 * I2C pass-through to the remote serializer requires the link to
	 * re-lock first.  Without this delay, writes to MAX96717 fail
	 * with -121 (EREMOTEIO).
	 */
	msleep(1000);

	ret = max96717_setup_streaming(state->ser_dev);
	if (ret) {
		dev_warn(&c->dev, "%s, failed to setup max96717 streaming\n",
			__func__);
		goto serdes_setup_end;
	}

	/*
	 * SER setup includes a soft reset (0x0002=0x03) which
	 * disrupts the GMSL link. After SER final enable (0x0002=0x43),
	 * the link re-trains. DES needs ONESHOT to resync CSI TX controller
	 * to the now-active tunnel data stream.
	 * XML sequence: SER config → DES ONESHOT → SER enable (no link drop).
	 * Driver sequence: DES ONESHOT → SER reset+config+enable (link drop!).
	 * Fix: add delay for link re-lock, then fire ONESHOT.
	 */
	msleep(500);
	state->dser_ops->reset_oneshot(state->dser_dev);

serdes_setup_end:
	if (ret)
		d5x_serdes_cleanup(state);

	return ret;
}
#endif
enum state_sid {
	DEPTH_SID = 0,
	RGB_SID,
	IR_SID,
	IMU_SID,
	MUX_SID = -1
};

static int d5x_ctrl_init(struct d5x *state, int sid)
{
	const struct v4l2_ctrl_ops *ops = &d5x_ctrl_ops;
	struct d5x_ctrls *ctrls = &state->ctrls;
	struct v4l2_ctrl_handler *hdl = &ctrls->handler;
	struct v4l2_subdev *sd = &state->mux.sd.subdev;
	int ret = -1;
	struct d5x_sensor *sensor = NULL;

	switch (sid) {
	case DEPTH_SID:
		hdl = &ctrls->handler_depth;
		sensor = &state->depth.sensor;
		break;
	case RGB_SID:
		hdl = &ctrls->handler_rgb;
		sensor = &state->rgb.sensor;
		break;
	case IR_SID:
		hdl = &ctrls->handler_y8;
		sensor = &state->ir.sensor;
		break;
	case IMU_SID:
		hdl = &ctrls->handler_imu;
		sensor = &state->imu.sensor;
		break;
	default:
		/* control for MUX */
		hdl = &ctrls->handler;
		sensor = NULL;
		break;
	}

	dev_dbg(NULL, "%s():%d sid: %d\n", __func__, __LINE__, sid);
	ret = v4l2_ctrl_handler_init(hdl, D5X_N_CONTROLS);
	if (ret < 0) {
		v4l2_err(sd, "cannot init ctrl handler (%d)\n", ret);
		return ret;
	}

	if (sid == DEPTH_SID || sid == IR_SID) {
		ctrls->laser_power = v4l2_ctrl_new_custom(hdl,
						&d5x_ctrl_laser_power,
						sensor);
		ctrls->manual_laser_power = v4l2_ctrl_new_custom(hdl,
						&d5x_ctrl_manual_laser_power,
						sensor);
	}

	/* Total gain */
	if (sid == DEPTH_SID || sid == IR_SID) {
		ctrls->gain = v4l2_ctrl_new_std(hdl, ops,
						V4L2_CID_ANALOGUE_GAIN,
						16, 248, 1, 16);
	} else if (sid == RGB_SID) {
		ctrls->gain = v4l2_ctrl_new_std(hdl, ops,
						V4L2_CID_ANALOGUE_GAIN,
						0, 128, 1, 64);
	}

	if ((ctrls->gain) && (sid >= DEPTH_SID && sid < IMU_SID)) {
		ctrls->gain->priv = sensor;
		ctrls->gain->flags =
				V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
	}
	if (sid >= DEPTH_SID && sid < IMU_SID) {

		ctrls->auto_exp = v4l2_ctrl_new_std_menu(hdl, ops,
				V4L2_CID_EXPOSURE_AUTO,
				V4L2_EXPOSURE_APERTURE_PRIORITY,
				~((1 << V4L2_EXPOSURE_MANUAL) |
						(1 << V4L2_EXPOSURE_APERTURE_PRIORITY)),
						V4L2_EXPOSURE_APERTURE_PRIORITY);

		if (ctrls->auto_exp) {
			ctrls->auto_exp->flags |=
					V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
			ctrls->auto_exp->priv = sensor;
		}
	}

	/* Exposure time: V4L2_CID_EXPOSURE_ABSOLUTE default unit: 100 us. */
	if (sid == DEPTH_SID || sid == IR_SID) {
		ctrls->exposure = v4l2_ctrl_new_std(hdl, ops,
					V4L2_CID_EXPOSURE_ABSOLUTE,
					1, MAX_DEPTH_EXP, 1, DEF_DEPTH_EXP);
	} else if (sid == RGB_SID) {
		ctrls->exposure = v4l2_ctrl_new_std(hdl, ops,
					V4L2_CID_EXPOSURE_ABSOLUTE,
					1, MAX_RGB_EXP, 1, DEF_RGB_EXP);
	}

	if ((ctrls->exposure) && (sid >= DEPTH_SID && sid < IMU_SID)) {
		ctrls->exposure->priv = sensor;
		ctrls->exposure->flags |=
				V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
		/* override default int type to u32 to match SKU & UVC */
		ctrls->exposure->type = V4L2_CTRL_TYPE_U32;
	}
	if (hdl->error) {
		v4l2_err(sd, "error creating controls (%d)\n", hdl->error);
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	/* Add these after v4l2_ctrl_handler_setup so they won't be set up */
	if (sid >= DEPTH_SID && sid < IMU_SID) {
		ctrls->log = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_log, sensor);
		ctrls->fw_version = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_fw_version, sensor);
		ctrls->device_type =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_device_type, sensor);
		ctrls->gvd = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_gvd, sensor);
		ctrls->get_depth_calib =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_get_depth_calib, sensor);
		ctrls->set_depth_calib =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_set_depth_calib, sensor);
		ctrls->get_coeff_calib =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_get_coeff_calib, sensor);
		ctrls->set_coeff_calib =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_set_coeff_calib, sensor);
		ctrls->ae_roi_get = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_ae_roi_get, sensor);
		ctrls->ae_roi_set = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_ae_roi_set, sensor);
		ctrls->ae_setpoint_get =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_ae_setpoint_get, sensor);
		ctrls->ae_setpoint_set =
				v4l2_ctrl_new_custom(hdl, &d5x_ctrl_ae_setpoint_set, sensor);
		ctrls->erb = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_erb, sensor);
		ctrls->ewb = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_ewb, sensor);
		ctrls->hwmc = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_hwmc, sensor);
		ctrls->hwmc_rw = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_hwmc_rw, sensor);

#ifdef V4L2_CTRL_FLAG_DYNAMIC_ARRAY
		if (!hdl->error && ctrls->hwmc_rw) {
			ret = d5x_prime_hwmc_rw_ctrl(ctrls->hwmc_rw);
			if (ret) {
				v4l2_err(sd, "cannot prime HWMC_RW control (%d)\n", ret);
				v4l2_ctrl_handler_free(hdl);
				return ret;
			}
		}
#endif
		v4l2_ctrl_new_custom(hdl, &d5x_ctrl_hw_reset, sensor);
	}
	/* DEPTH custom */
	if (sid == DEPTH_SID) {
		ctrls->sync_mode = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_sync_mode, sensor);
	}
	/* IMU custom */
	if (sid == IMU_SID)
		ctrls->fw_version = v4l2_ctrl_new_custom(hdl, &d5x_ctrl_fw_version, sensor);

	switch (sid) {
	case DEPTH_SID:
		state->depth.sensor.sd.ctrl_handler = hdl;
		dev_dbg(state->depth.sensor.sd.dev,
			"%s():%d set ctrl_handler pad:%d\n",
			__func__, __LINE__, state->depth.sensor.mux_pad);
		break;
	case RGB_SID:
		state->rgb.sensor.sd.ctrl_handler = hdl;
		dev_dbg(state->rgb.sensor.sd.dev,
			"%s():%d set ctrl_handler pad:%d\n",
			__func__, __LINE__, state->rgb.sensor.mux_pad);
		break;
	case IR_SID:
		state->ir.sensor.sd.ctrl_handler = hdl;
		dev_dbg(state->ir.sensor.sd.dev,
			"%s():%d set ctrl_handler pad:%d\n",
			__func__, __LINE__, state->ir.sensor.mux_pad);
		break;
	case IMU_SID:
		state->imu.sensor.sd.ctrl_handler = hdl;
		dev_dbg(state->imu.sensor.sd.dev,
			"%s():%d set ctrl_handler pad:%d\n",
			__func__, __LINE__, state->imu.sensor.mux_pad);
		break;
	default:
		state->mux.sd.subdev.ctrl_handler = hdl;
		dev_dbg(state->mux.sd.subdev.dev,
			"%s():%d set ctrl_handler for MUX\n", __func__, __LINE__);
		break;
	}

	return 0;
}

static int d5x_sensor_init(struct i2c_client *c, struct d5x *state,
		struct d5x_sensor *sensor, const struct v4l2_subdev_ops *ops,
		const char *name)
{
	struct v4l2_subdev *sd = &sensor->sd;
	struct media_entity *entity = &sensor->sd.entity;
	struct media_pad *pad = &sensor->pad;
	dev_t *dev_num = &state->client->dev.devt;
#ifndef CONFIG_OF
	struct d4xx_pdata *dpdata = c->dev.platform_data;
	char suffix = dpdata->suffix;
#endif
	sensor->pipe_id = PIPE_NOT_CONFIGURED;
	sensor->pipe_reapply_gen = 0;
	v4l2_i2c_subdev_init(sd, c, ops);
	/* See tegracam_v4l2.c tegracam_v4l2subdev_register() */
	/* Set owner to NULL so we can unload the driver module */
	sd->owner = NULL;
	sd->internal_ops = &d5x_sensor_internal_ops;
	sd->grp_id = *dev_num;
	v4l2_set_subdevdata(sd, state);
#ifndef CONFIG_OF
	/*
	 * TODO: suffix for 2 D457 connected to 1 Deser
	 */
	if (state->aggregated & 1)
		suffix += 4;
	snprintf(sd->name, sizeof(sd->name), D5X_DRIVER_NAME_SENSOR " %s %c", name, suffix);
#else
	snprintf(sd->name, sizeof(sd->name), D5X_DRIVER_NAME_SENSOR " %s %d-%04x",
		 name, i2c_adapter_id(c->adapter), c->addr);
#endif

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	pad->flags = MEDIA_PAD_FL_SOURCE;
	entity->obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV;
	entity->function = MEDIA_ENT_F_CAM_SENSOR;
	return media_entity_pads_init(entity, 1, pad);
}

static int d5x_sensor_register(struct d5x *state, struct d5x_sensor *sensor)
{
	struct v4l2_subdev *sd = &sensor->sd;
	struct media_entity *entity = &sensor->sd.entity;
	int ret = -1;

	/* FIXME: is async needed? */
	ret = v4l2_device_register_subdev(state->mux.sd.subdev.v4l2_dev, sd);
	if (ret < 0) {
		dev_err(sd->dev, "%s(): %d: %d\n", __func__, __LINE__, ret);
		return ret;
	}

	ret = media_create_pad_link(entity, 0,
			&state->mux.sd.subdev.entity, sensor->mux_pad,
			MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED);
	if (ret < 0) {
		dev_err(sd->dev, "%s(): %d: %d\n", __func__, __LINE__, ret);
		goto e_sd;
	}

	dev_dbg(sd->dev, "%s(): 0 -> %d\n", __func__, sensor->mux_pad);

	return 0;

e_sd:
	v4l2_device_unregister_subdev(sd);

	return ret;
}

static void d5x_sensor_remove(struct d5x_sensor *sensor)
{
	v4l2_device_unregister_subdev(&sensor->sd);

	media_entity_cleanup(&sensor->sd.entity);
}

static int d5x_depth_init(struct i2c_client *c, struct d5x *state)
{
	/* Which mux pad we're connecting to */
	state->depth.sensor.mux_pad = D5X_MUX_PAD_DEPTH;
	return d5x_sensor_init(c, state, &state->depth.sensor,
		       &d5x_subdev_ops, "depth");
}

static int d5x_ir_init(struct i2c_client *c, struct d5x *state)
{
	state->ir.sensor.mux_pad = D5X_MUX_PAD_IR;
	return d5x_sensor_init(c, state, &state->ir.sensor,
		       &d5x_subdev_ops, "ir");
}

static int d5x_rgb_init(struct i2c_client *c, struct d5x *state)
{
	state->rgb.sensor.mux_pad = D5X_MUX_PAD_RGB;
	return d5x_sensor_init(c, state, &state->rgb.sensor,
		       &d5x_subdev_ops, "rgb");
}

static int d5x_imu_init(struct i2c_client *c, struct d5x *state)
{
	state->imu.sensor.mux_pad = D5X_MUX_PAD_IMU;
	return d5x_sensor_init(c, state, &state->imu.sensor,
		       &d5x_subdev_ops, "imu");
}

/* No locking needed */
static int d5x_mux_enum_mbus_code(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
				     struct v4l2_subdev_pad_config *cfg,
#else
				     struct v4l2_subdev_state *v4l2_state,
#endif
				  struct v4l2_subdev_mbus_code_enum *mce)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
	struct v4l2_subdev_mbus_code_enum tmp = *mce;
	struct v4l2_subdev *remote_sd;
	int ret = -1;

	dev_dbg(&state->client->dev, "%s(): %s \n", __func__, sd->name);
	switch (mce->pad) {
	case D5X_MUX_PAD_IR:
		remote_sd = &state->ir.sensor.sd;
		break;
	case D5X_MUX_PAD_DEPTH:
		remote_sd = &state->depth.sensor.sd;
		break;
	case D5X_MUX_PAD_RGB:
		remote_sd = &state->rgb.sensor.sd;
		break;
	case D5X_MUX_PAD_IMU:
		remote_sd = &state->imu.sensor.sd;
		break;
	case D5X_MUX_PAD_EXTERNAL:
		/*
		 * A stream-specific mux enumerates that stream's complete format
		 * table. Only the shared legacy mux combines IR and depth indices.
		 */
		if (state->is_rgb) {
			remote_sd = &state->rgb.sensor.sd;
			break;
		}
		if (state->is_depth) {
			remote_sd = &state->depth.sensor.sd;
			break;
		}
		if (state->is_y8) {
			remote_sd = &state->ir.sensor.sd;
			break;
		}
		if (state->is_imu) {
			remote_sd = &state->imu.sensor.sd;
			break;
		}

		if (mce->index >= state->ir.sensor.n_formats +
				state->depth.sensor.n_formats)
			return -EINVAL;

		/*
		 * First list Left node / Motion Tracker formats, then depth.
		 * This should also help because D16 doesn't have a direct
		 * analog in MIPI CSI-2.
		 */
		if (mce->index < state->ir.sensor.n_formats) {
			remote_sd = &state->ir.sensor.sd;
		} else {
			tmp.index = mce->index - state->ir.sensor.n_formats;
			remote_sd = &state->depth.sensor.sd;
		}

		break;
	default:
		return -EINVAL;
	}

	tmp.pad = 0;
	if (state->is_rgb)
		remote_sd = &state->rgb.sensor.sd;
	if (state->is_depth)
		remote_sd = &state->depth.sensor.sd;
	if (state->is_y8)
		remote_sd = &state->ir.sensor.sd;
	if (state->is_imu)
		remote_sd = &state->imu.sensor.sd;
	/* Locks internally */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
	ret = d5x_sensor_enum_mbus_code(remote_sd, cfg, &tmp);
#else
	ret = d5x_sensor_enum_mbus_code(remote_sd, v4l2_state, &tmp);
#endif
	if (!ret)
		mce->code = tmp.code;

	return ret;
}
static int d5x_state_to_pad(struct d5x *state) {
	int pad = -1;
	if (state->is_depth)
		pad = D5X_MUX_PAD_DEPTH;
	if (state->is_y8)
		pad = D5X_MUX_PAD_IR;
	if (state->is_rgb)
		pad = D5X_MUX_PAD_RGB;
	if (state->is_imu)
		pad = D5X_MUX_PAD_IMU;
	return pad;
}

/* No locking needed */
static int d5x_mux_enum_frame_size(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
				     struct v4l2_subdev_pad_config *cfg,
#else
				     struct v4l2_subdev_state *v4l2_state,
#endif
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
	struct v4l2_subdev_frame_size_enum tmp = *fse;
	struct v4l2_subdev *remote_sd;
	u32 pad = fse->pad;
	int ret = -1;

	tmp.pad = 0;
	pad = d5x_state_to_pad(state);

	switch (pad) {
	case D5X_MUX_PAD_IR:
		remote_sd = &state->ir.sensor.sd;
		break;
	case D5X_MUX_PAD_DEPTH:
		remote_sd = &state->depth.sensor.sd;
		break;
	case D5X_MUX_PAD_RGB:
		remote_sd = &state->rgb.sensor.sd;
		break;
	case D5X_MUX_PAD_IMU:
		remote_sd = &state->imu.sensor.sd;
		break;
	case D5X_MUX_PAD_EXTERNAL:
		/*
		 * Assume, that different sensors don't support the same formats
		 * Try the Depth sensor first, then the Motion Tracker
		 */
		remote_sd = &state->depth.sensor.sd;
		ret = d5x_sensor_enum_frame_size(remote_sd, NULL, &tmp);
		if (!ret) {
			*fse = tmp;
			fse->pad = pad;
			return 0;
		}

		remote_sd = &state->ir.sensor.sd;
		break;
	default:
		return -EINVAL;
	}

	/* Locks internally */
	ret = d5x_sensor_enum_frame_size(remote_sd, NULL, &tmp);
	if (!ret) {
		*fse = tmp;
		fse->pad = pad;
	}

	return ret;
}

/* No locking needed */
static int d5x_mux_enum_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
				     struct v4l2_subdev_pad_config *cfg,
#else
				     struct v4l2_subdev_state *v4l2_state,
#endif
				     struct v4l2_subdev_frame_interval_enum *fie)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
	struct v4l2_subdev_frame_interval_enum tmp = *fie;
	struct v4l2_subdev *remote_sd;
	u32 pad = fie->pad;
	int ret = -1;

	tmp.pad = 0;

	dev_dbg(state->depth.sensor.sd.dev,
			"%s(): pad %d code %x width %d height %d\n",
			__func__, pad, tmp.code, tmp.width, tmp.height);

	pad = d5x_state_to_pad(state);

	switch (pad) {
	case D5X_MUX_PAD_IR:
		remote_sd = &state->ir.sensor.sd;
		break;
	case D5X_MUX_PAD_DEPTH:
		remote_sd = &state->depth.sensor.sd;
		break;
	case D5X_MUX_PAD_RGB:
		remote_sd = &state->rgb.sensor.sd;
		break;
	case D5X_MUX_PAD_IMU:
		remote_sd = &state->imu.sensor.sd;
		break;
	case D5X_MUX_PAD_EXTERNAL:
		/* Similar to d5x_mux_enum_frame_size() above */
		if (state->is_rgb)
			remote_sd = &state->rgb.sensor.sd;
		else
			remote_sd = &state->ir.sensor.sd;
		ret = d5x_sensor_enum_frame_interval(remote_sd, NULL, &tmp);
		if (!ret) {
			*fie = tmp;
			fie->pad = pad;
			return 0;
		}

		remote_sd = &state->ir.sensor.sd;
		break;
	default:
		return -EINVAL;
	}

	/* Locks internally */
	ret = d5x_sensor_enum_frame_interval(remote_sd, NULL, &tmp);
	if (!ret) {
		*fie = tmp;
		fie->pad = pad;
	}

	return ret;
}

/* No locking needed */
static int d5x_mux_set_fmt(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_format *fmt)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
	struct v4l2_subdev_format tmp;
	struct v4l2_subdev *remote_sd;
	u32 pad;
	int ret = 0;

	if (!state) return -EINVAL;
	if (!fmt) return -EINVAL;

	pad = d5x_state_to_pad(state);
	tmp = *fmt;

	dev_dbg(sd->dev, "%s(): pad: %x %x: %ux%u\n",
			__func__, pad, fmt->format.code,
			fmt->format.width, fmt->format.height);

	switch (pad) {
	case D5X_MUX_PAD_IR:
		remote_sd = &state->ir.sensor.sd;
		break;
	case D5X_MUX_PAD_DEPTH:
		remote_sd = &state->depth.sensor.sd;
		break;
	case D5X_MUX_PAD_RGB:
		remote_sd = &state->rgb.sensor.sd;
		break;
	case D5X_MUX_PAD_IMU:
		remote_sd = &state->imu.sensor.sd;
		break;
	case D5X_MUX_PAD_EXTERNAL:
		if (state->is_rgb)
			remote_sd = &state->rgb.sensor.sd;
		else
			remote_sd = &state->mux.last_set->sd;
		break;
	default:
		return -EINVAL;
	}

	tmp.pad = 0;

	/* Locks internally */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
	ret = d5x_sensor_set_fmt(remote_sd, cfg, &tmp);
#else
	ret = d5x_sensor_set_fmt(remote_sd, v4l2_state, &tmp);
#endif
	if (!ret) {
		*fmt = tmp;
		fmt->pad = pad;
	}
	return ret;
}

/* No locking needed */
static int d5x_mux_get_fmt(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
		struct v4l2_subdev_pad_config *cfg,
#else
		struct v4l2_subdev_state *v4l2_state,
#endif
		struct v4l2_subdev_format *fmt)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
	struct v4l2_subdev_format tmp;
	struct v4l2_subdev *remote_sd;
	u32 pad;
	int ret = 0;
	struct d5x_sensor *sensor;

	if (!state) return -EINVAL;
	if (!fmt) return -EINVAL;

	sensor = state->mux.last_set;
	tmp = *fmt;
	pad = d5x_state_to_pad(state);

	dev_dbg(sd->dev, "%s(): %u %s %p\n", __func__, pad, d5x_get_sensor_name(state), state->mux.last_set);

	switch (pad) {
	case D5X_MUX_PAD_IR:
		remote_sd = &state->ir.sensor.sd;
		break;
	case D5X_MUX_PAD_DEPTH:
		remote_sd = &state->depth.sensor.sd;
		break;
	case D5X_MUX_PAD_EXTERNAL:
		remote_sd = &state->mux.last_set->sd;
		break;
	case D5X_MUX_PAD_RGB:
		remote_sd = &state->rgb.sensor.sd;
		break;
	case D5X_MUX_PAD_IMU:
		remote_sd = &state->imu.sensor.sd;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(sd->dev, "%s(): fmt->pad:%d, sensor->mux_pad:%u size:%d-%d, code:0x%x field:%d, color:%d\n",
		__func__, fmt->pad, pad,
		fmt->format.width, fmt->format.height, fmt->format.code,
		fmt->format.field, fmt->format.colorspace);
	/* Locks internally */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 10)
	ret = d5x_sensor_get_fmt(remote_sd, cfg, &tmp);
#else
	ret = d5x_sensor_get_fmt(remote_sd, v4l2_state, &tmp);
#endif
	if (!ret) {
		*fmt = tmp;
		fmt->pad = pad;
	}

	return ret;
}

/* Video ops */
static int d5x_mux_g_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
		struct v4l2_subdev_state *state,
#endif
		struct v4l2_subdev_frame_interval *fi)
{
	struct d5x *d5x_state = container_of(sd, struct d5x, mux.sd.subdev);
	struct d5x_sensor *sensor = NULL;

	if (NULL == sd || NULL == fi)
		return -EINVAL;

	sensor = d5x_state->mux.last_set;

	fi->interval.numerator = 1;
	fi->interval.denominator = sensor->config.framerate;

	dev_dbg(sd->dev, "%s(): %s %u\n", __func__, sd->name,
			fi->interval.denominator);

	return 0;
}

static u16 __d5x_probe_framerate(const struct d5x_resolution *res, u16 target)
{
	int i;
	u16 framerate;

	for (i = 0; i < res->n_framerates; i++) {
		framerate = res->framerates[i];
		if (target <= framerate)
			return framerate;
	}

	return res->framerates[res->n_framerates - 1];
}

static int d5x_mux_s_frame_interval(struct v4l2_subdev *sd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
		struct v4l2_subdev_state *state,
#endif
		struct v4l2_subdev_frame_interval *fi)
{
	struct d5x *d5x_state = container_of(sd, struct d5x, mux.sd.subdev);
	struct d5x_sensor *sensor = NULL;
	u16 framerate = 1;

	if (NULL == sd || NULL == fi || fi->interval.numerator == 0)
		return -EINVAL;

	sensor = d5x_state->mux.last_set;

	framerate = fi->interval.denominator / fi->interval.numerator;
	framerate = __d5x_probe_framerate(sensor->config.resolution, framerate);
	sensor->config.framerate = framerate;
	fi->interval.numerator = 1;
	fi->interval.denominator = framerate;

	dev_dbg(sd->dev, "%s(): %s %u\n", __func__, sd->name, framerate);

	return 0;
}

static int d5x_mux_s_stream(struct v4l2_subdev *sd, int on)
{
	struct d5x *state = container_of(sd, struct d5x, mux.sd.subdev);
#if D5X_BYPASS_CAMERA_I2C
	struct d5x_sensor *sensor = state->mux.last_set;
	bool *streaming_flag = NULL;
	int ret;

	dev_info(&state->client->dev,
		"d5x_mux_s_stream(): BYPASS mode - SERDES pipe setup only, on=%d\n", on);

	if (state->is_depth)
		streaming_flag = &state->d5x_dev->depth_streaming;
	else if (state->is_rgb)
		streaming_flag = &state->d5x_dev->rgb_streaming;
	else if (state->is_y8)
		streaming_flag = &state->d5x_dev->ir_streaming;
	else if (state->is_imu)
		streaming_flag = &state->d5x_dev->imu_streaming;
	else
		return -EINVAL;

	if (sensor->streaming == on)
		return 0;

	mutex_lock(&state->d5x_dev->lock);
	*streaming_flag = on;
	mutex_unlock(&state->d5x_dev->lock);
	sensor->streaming = on;

	if (on) {
		/* Configure SERDES pipes - this is essential for MIPI data flow */
		ret = d5x_configure(state);
		if (ret < 0) {
			dev_err(&state->client->dev,
				"BYPASS: d5x_configure (SERDES pipe setup) failed: %d\n", ret);
			sensor->streaming = false;
			mutex_lock(&state->d5x_dev->lock);
			*streaming_flag = false;
			mutex_unlock(&state->d5x_dev->lock);
			return ret;
		}
#ifdef CONFIG_VIDEO_D5XX_SERDES
		/*
		 * The control callback enables serializer GPIO tunneling
		 * immediately.  Re-assert it here after reset/recovery, then
		 * start the DES internal FSYNC source at the negotiated FPS.
		 */
		if (d5x_sync_mode_uses_esync(state)) {
			d5x_set_ser_esync_tunneling(state, true);
			d5x_set_des_fsync(state, true);
		}
#endif
	} else {
		/* On stream off, release SERDES pipe */
#ifdef CONFIG_VIDEO_D5XX_SERDES
		if (sensor->pipe_id >= 0) {
			mutex_lock(&serdes_lock__);
			if (state->dser_ops->release_pipe(state->dser_dev, sensor->pipe_id) < 0)
				dev_warn(&state->client->dev, "release pipe failed\n");
			else {
				d5x_disarm_dser_datapath_if_idle(state);
				sensor->pipe_id = PIPE_NOT_CONFIGURED;
				sensor->pipe_reapply_gen = 0;
			}
			mutex_unlock(&serdes_lock__);
		}
		/*
		 * Tear down the DES FSYNC generator when the last stream
		 * stops.  Serializer GPIO tunneling remains tied to
		 * camera_sync_mode and is disabled by the control callback.
		 */
		if (d5x_sync_mode_uses_esync(state) &&
		    !state->d5x_dev->depth_streaming &&
		    !state->d5x_dev->rgb_streaming &&
		    !state->d5x_dev->ir_streaming &&
		    !state->d5x_dev->imu_streaming)
			d5x_set_des_fsync(state, false);
#endif
	}
	return 0;
#else
	u16 streaming, status;
	int ret = 0;
	unsigned int i = 0, d5x_config_retries = MAX_D5X_CONFIG_RETRIES;
	unsigned long timeout, ts;
	int restore_val = 0;
	u16 stream_cmd;
	u16 config_status_base, stream_status_base, stream_id, vc_id;
	struct d5x_sensor *sensor = state->mux.last_set;
	u16 expected_streaming_state;
	bool d5x_config_done = !on; /* for stop, skip config */
	bool reset_invalidated = false;
	bool stream_ctrl_locked = false;
	bool command_read_status;
	bool *streaming_flag = NULL;
#ifdef CONFIG_VIDEO_D5XX_SERDES
	int cur_dser = atomic_read(dser_get_reset_gen(state));
#else
	int cur_dser = 0;
#endif
	int cur_d5x = atomic_read(d5x_get_reset_gen(state));

	if (state->is_depth) {
		config_status_base = D5X_DEPTH_CONFIG_STATUS;
		stream_status_base = D5X_DEPTH_STREAM_STATUS;
		stream_id = D5X_STREAM_DEPTH;
		vc_id = 0;
		streaming_flag = &state->d5x_dev->depth_streaming;
	} else if (state->is_rgb) {
		config_status_base = D5X_RGB_CONFIG_STATUS;
		stream_status_base = D5X_RGB_STREAM_STATUS;
		stream_id = D5X_STREAM_RGB;
		vc_id = 1;
		streaming_flag = &state->d5x_dev->rgb_streaming;
	} else if (state->is_y8) {
		config_status_base = D5X_IR_CONFIG_STATUS;
		stream_status_base = D5X_IR_STREAM_STATUS;
		stream_id = D5X_STREAM_IR;
		vc_id = 2;
		streaming_flag = &state->d5x_dev->ir_streaming;
	} else if (state->is_imu) {
		config_status_base = D5X_IMU_CONFIG_STATUS;
		stream_status_base = D5X_IMU_STREAM_STATUS;
		stream_id = D5X_STREAM_IMU;
		vc_id = 3;
		streaming_flag = &state->d5x_dev->imu_streaming;
	} else {
		return -EINVAL;
	}
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
#ifdef CONFIG_VIDEO_D5XX_SERDES
	vc_id = state->g_ctx.dst_vc;
#endif
#endif
	dev_dbg(&state->client->dev, "s_stream for stream %s, vc:%d, SENSOR=%s on = %d\n",
			sensor->sd.name, vc_id, d5x_get_sensor_name(state), on);

	/*
	 * Lazy invalidation after HW or deserializer reset.
	 * Detect gen-counter bumps, clear stale streaming/config/pipe
	 * state, then update refs.  Must run before the duplicate-call
	 * guard so a reset-killed stream is not mistaken for "already off".
	 */
	if (state->reset_ref_d5x != cur_d5x
			|| state->reset_ref_dser != cur_dser) {
		d5x_invalidate_sensor(state, sensor);
		sensor->streaming = false;
		reset_invalidated = true;
		state->reset_ref_d5x = cur_d5x;
		state->reset_ref_dser = cur_dser;
	}

	/* Ignore requests that already match the cached stream state. */
	if (sensor->streaming == on)
		return 0;

	if (on) {
		stream_cmd = (D5X_STREAM_START | stream_id);
		expected_streaming_state = D5X_STREAM_STREAMING;
		status = 0;
	} else {
		stream_cmd = (D5X_STREAM_STOP | stream_id);
		expected_streaming_state = D5X_STREAM_IDLE;
		status = D5X_STATUS_STREAMING;
	}

	/* START changes pipeline topology and remains fully serialized. STOP only
	 * serializes the shared command-register write; each stream then drains
	 * and polls independently, so control I2C remains usable while another
	 * HKR data path is stopping. */
	if (on) {
		mutex_lock(&state->d5x_dev->stream_ctrl_lock);
		stream_ctrl_locked = true;
	}

	if (on) {
		ret = d5x_configure(state);
		if (ret < 0) {
#ifdef CONFIG_VIDEO_D5XX_SERDES
			d5x_release_serdes_pipe(state, sensor, "stream configure failure");
#endif
			mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
			stream_ctrl_locked = false;
			return ret;
		}
		d5x_config_done = true;
	}

	/* Verify stream is in the expected state before issuing command */
	ts = jiffies;
	for (timeout = ts + msecs_to_jiffies(on ? D5X_START_MAX_TIME :
							 D5X_STOP_MAX_TIME), i = 0;
			time_before(jiffies, timeout);
			i++, msleep_range(d5x_stream_poll_delay(on, i)))
	{
		ret = d5x_read(state, config_status_base, &status);
		if (ret < 0 || status == D5X_STATUS_UNAVAILABLE ||
		    (status & ~D5X_STATUS_VALID_MASK))
			continue;
		/* A recovery STOP may race with an HKR-side teardown that already
		 * made the stream idle.  Let the common no-op cleanup run now rather
		 * than waiting the full STOP timeout for a state transition that has
		 * already happened. */
		if (!on && !(status & D5X_STATUS_STREAMING))
			break;
		if (on == !(status & D5X_STATUS_STREAMING)) {
			break;
		}
#ifdef CONFIG_VIDEO_D5XX_SERDES
		if (on && status != D5X_STATUS_UNAVAILABLE &&
		    (status & D5X_STATUS_STREAMING)) {
			dev_info(&state->client->dev,
				 "stream %d already reports streaming before START; treating as already started\n",
				 stream_id);
			break;
		}
#endif
	}
	if (on == !(status & D5X_STATUS_STREAMING))
	{
		dev_dbg(&state->client->dev,
			"stream %d in expected state, toggling to %d (status: 0x%04x) %dms\n",
			stream_id, on, status, jiffies_to_msecs(jiffies - ts));
	} else {
		if (on && reset_invalidated && (status & D5X_STATUS_STREAMING))
			dev_warn(&state->client->dev,
				"stream %d state mismatch after reset invalidation: host=off HKR=streaming (status: 0x%04x)\n",
				stream_id, status);

		/*
		 * The device status is authoritative. Do not inject a compensating
		 * STOP/START sequence here: it reorders the caller's stream commands
		 * and hides the lifecycle bug that produced a stale host cache.
		 *
		 * After HW reset the FW reboots and all streams return to idle. If VI
		 * error recovery asks to stop an already-idle stream, or start an
		 * already-streaming one, synchronize the local cache and let the upper
		 * layer continue instead of entering an EBUSY retry loop.
		 */
		dev_warn(&state->client->dev,
			"stream %d in %d state already (status: 0x%04x) %dms, treating as no-op\n",
			stream_id, on, status, jiffies_to_msecs(jiffies - ts));
		mutex_lock(&state->d5x_dev->lock);
		*streaming_flag = on;
		mutex_unlock(&state->d5x_dev->lock);
		sensor->streaming = on;
#ifdef CONFIG_VIDEO_D5XX_SERDES
		if (on)
			d5x_post_start_dser_datapath_kick(state, stream_id);
#endif
		if (stream_ctrl_locked) {
			mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
			stream_ctrl_locked = false;
		}
		return 0;
	}

	restore_val = sensor->streaming;
	mutex_lock(&state->d5x_dev->lock);
	*streaming_flag = on;
	mutex_unlock(&state->d5x_dev->lock);
	sensor->streaming = on;

	/*
	 * Execute command, poll state (retry if necessary) and poll completion.
	 * For start, also confirm config status is valid and not rejected by FW, otherwise retry.
	 */
	ts = jiffies;
	streaming = ~expected_streaming_state; /* force initial toggle */
		for (timeout = ts + msecs_to_jiffies(on ? D5X_START_MAX_TIME :
								 D5X_STOP_MAX_TIME), i = 0;
				time_before(jiffies, timeout);
				i++, msleep_range(d5x_stream_poll_delay(on, i)))
		{
			command_read_status = false;
			if (!d5x_config_done) {
			ret = d5x_configure(state);
			if (ret < 0) {
				if (ret == -ENOSR) {
					/* No recovery can help if no resources are available */
					if (stream_ctrl_locked) {
						mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
						stream_ctrl_locked = false;
					}
					return ret;
				}
				dev_warn(&state->client->dev, "stream %d config failed, retry %d, %dms\n",
					stream_id, i, jiffies_to_msecs(jiffies - ts));
				continue;
			}
			d5x_config_done = true;
		}

			if (streaming != expected_streaming_state) {
				if (!on)
					mutex_lock(&state->d5x_dev->stream_ctrl_lock);
				ret = d5x_write_then_read(state, D5X_START_STOP_STREAM,
							  stream_cmd, stream_status_base,
							  &streaming);
				if (!on)
					mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
				if (ret < 0) {
					dev_warn(&state->client->dev, "stream %d cmd 0x%x write failed, retry %d, %dms\n",
						stream_id, stream_cmd, i, jiffies_to_msecs(jiffies - ts));
				} else
					command_read_status = true;
			}

			if (!command_read_status)
				ret = d5x_read(state, stream_status_base, &streaming);
			if (ret < 0) {
			dev_warn(&state->client->dev,
				"stream %d status i2c read failed (%d), retry %u, %dms\n",
				stream_id, ret, i, jiffies_to_msecs(jiffies - ts));
		}

		if (streaming != expected_streaming_state) {
			dev_dbg(&state->client->dev, "stream %d status not as expected (%d != %d), retry %d, %dms\n",
				stream_id, streaming, expected_streaming_state, i, jiffies_to_msecs(jiffies - ts));
			continue;
		}

		ret = d5x_read(state, config_status_base, &status);
		if (ret < 0) {
			dev_warn(&state->client->dev,
				"stream %d config status i2c read failed (%d), retry %u, %dms\n",
				stream_id, ret, i, jiffies_to_msecs(jiffies - ts));
			continue;
		}

		if (status == D5X_STATUS_UNAVAILABLE ||
		    (status & ~D5X_STATUS_VALID_MASK)) {
			dev_warn(&state->client->dev,
				"stream %d config status invalid/unavailable (0x%04x), retry %u, %dms\n",
				stream_id, status, i, jiffies_to_msecs(jiffies - ts));
			continue;
		}

		if (on && (status & (D5X_STATUS_INVALID_DT |
								D5X_STATUS_INVALID_RES |
								D5X_STATUS_INVALID_FPS)))
		{
			dev_warn(&state->client->dev,
				"stream %d config rejected, status 0x%04x, retry %u, %dms\n",
				stream_id, status, i, jiffies_to_msecs(jiffies - ts));
			if (d5x_config_retries > 0) {
				d5x_config_retries--;
				d5x_config_done = false;
				d5x_config_cache_clear(sensor);
			} else {
				dev_warn(&state->client->dev,
					"stream %d config failed after %d retries, aborting, %dms\n",
					stream_id, i, jiffies_to_msecs(jiffies - ts));
				break;
			}
			continue;
		}

		if (!on == !(status & D5X_STATUS_STREAMING))
		{
			dev_info(&state->client->dev,
				"stream %d toggle ok to %d in %dms, retries %d\n",
				stream_id, on, jiffies_to_msecs(jiffies - ts), i);
			break;
		}
	}

	if (on == !(status & D5X_STATUS_STREAMING))
	{
		dev_warn(&state->client->dev,
			"stream %d toggle to %d timeout in %dms, retries %d\n",
			stream_id, on, jiffies_to_msecs(jiffies - ts), i);

		if (streaming == expected_streaming_state) { /* try to toggle stream back on timeout  */
			if (!on)
				mutex_lock(&state->d5x_dev->stream_ctrl_lock);
			d5x_write(state, D5X_START_STOP_STREAM,
				(on ? D5X_STREAM_STOP : D5X_STREAM_START) | stream_id);
			if (!on)
				mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
		}
#ifdef CONFIG_VIDEO_D5XX_SERDES
		if (on && sensor->pipe_id >= 0) {
			mutex_lock(&serdes_lock__);
			ret = state->dser_ops->release_pipe(state->dser_dev, sensor->pipe_id);
			if (ret >= 0)
				d5x_disarm_dser_datapath_if_idle(state);
			mutex_unlock(&serdes_lock__);
			if (ret < 0) {
				dev_warn(&state->client->dev, "release pipe failed\n");
			} else {
				sensor->pipe_id = PIPE_NOT_CONFIGURED;
				sensor->pipe_reapply_gen = 0;
			}
		}
#endif
		mutex_lock(&state->d5x_dev->lock);
		*streaming_flag = restore_val;
		mutex_unlock(&state->d5x_dev->lock);
		sensor->streaming = restore_val;
		ret = -EAGAIN;
	}
	else if (!on)
	{
#ifdef CONFIG_VIDEO_D5XX_SERDES
		mutex_lock(&serdes_lock__);
		if (state->dser_ops->release_pipe(state->dser_dev, sensor->pipe_id) < 0)
			dev_warn(&state->client->dev, "release pipe failed\n");
		else {
			d5x_disarm_dser_datapath_if_idle(state);
			sensor->pipe_id = PIPE_NOT_CONFIGURED;
			sensor->pipe_reapply_gen = 0;
		}
		if (state->is_y8
			&& (state->ir.sensor.config.format->data_type == GMSL_CSI_DT_RGB_888))
		{
			state->dser_ops->reset_oneshot(state->dser_dev);
		}
		mutex_unlock(&serdes_lock__);
		msleep_range(100);

		/*
		 * Tear down the DES FSYNC generator when the last stream stops.
		 * Check the d5x_dev-level streaming flags (already
		 * cleared for *this* sensor before we entered the polling
		 * loop) to decide whether any stream is still active.
		 */
		if (d5x_sync_mode_uses_esync(state) &&
		    !state->d5x_dev->depth_streaming &&
		    !state->d5x_dev->rgb_streaming &&
		    !state->d5x_dev->ir_streaming &&
		    !state->d5x_dev->imu_streaming)
			d5x_set_des_fsync(state, false);
#endif
	}
#ifdef CONFIG_VIDEO_D5XX_SERDES
	/*
	 * Stream-on success path (neither timeout nor stream-off).
	 * Re-assert serializer GPIO tunneling after reset/recovery and
	 * start the DES internal FSYNC source at the negotiated FPS.
	 */
	if (on && ret >= 0)
		d5x_post_start_dser_datapath_kick(state, stream_id);

	if (on && d5x_sync_mode_uses_esync(state)) {
		d5x_set_ser_esync_tunneling(state, true);
		d5x_set_des_fsync(state, true);
	}
#endif
	if (stream_ctrl_locked)
		mutex_unlock(&state->d5x_dev->stream_ctrl_lock);
	return ret;
#endif /* !D5X_BYPASS_CAMERA_I2C */
}

static int d5x_mux_get_frame_desc(struct v4l2_subdev *sd,
	unsigned int pad, struct v4l2_mbus_frame_desc *desc)
{
	unsigned int i;

	desc->num_entries = V4L2_FRAME_DESC_ENTRY_MAX;

	for (i = 0; i < desc->num_entries; i++) {
		desc->entry[i].flags = 0;
		desc->entry[i].pixelcode = MEDIA_BUS_FMT_FIXED;
		desc->entry[i].length = 0;
		if (i == desc->num_entries - 1) {
			desc->entry[i].flags = V4L2_MBUS_FRAME_DESC_FL_LEN_MAX;
			desc->entry[i].pixelcode = MEDIA_BUS_FMT_FIXED;
			desc->entry[i].length = D5X_CSI_METADATA_MAX_WC;
		}
	}
	return 0;
}

static const struct v4l2_subdev_pad_ops d5x_mux_pad_ops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	.get_frame_interval	= d5x_mux_g_frame_interval,
	.set_frame_interval	= d5x_mux_s_frame_interval,
#endif
	.enum_mbus_code		= d5x_mux_enum_mbus_code,
	.enum_frame_size	= d5x_mux_enum_frame_size,
	.enum_frame_interval	= d5x_mux_enum_frame_interval,
	.get_fmt		= d5x_mux_get_fmt,
	.set_fmt		= d5x_mux_set_fmt,
	.get_frame_desc		= d5x_mux_get_frame_desc,
};

static const struct v4l2_subdev_core_ops d5x_mux_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
};

static const struct v4l2_subdev_video_ops d5x_mux_video_ops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
	.g_frame_interval	= d5x_mux_g_frame_interval,
	.s_frame_interval	= d5x_mux_s_frame_interval,
#endif
	.s_stream		= d5x_mux_s_stream,
};

static const struct v4l2_subdev_ops d5x_mux_subdev_ops = {
	.core = &d5x_mux_core_ops,
	.pad = &d5x_mux_pad_ops,
	.video = &d5x_mux_video_ops,
};

static int d5x_mux_registered(struct v4l2_subdev *sd)
{
	struct d5x *state = v4l2_get_subdevdata(sd);
	int ret = d5x_sensor_register(state, &state->depth.sensor);
	if (ret < 0)
		return ret;

	ret = d5x_sensor_register(state, &state->ir.sensor);
	if (ret < 0)
		goto e_depth;

	ret = d5x_sensor_register(state, &state->rgb.sensor);
	if (ret < 0)
		goto e_rgb;

	ret = d5x_sensor_register(state, &state->imu.sensor);
	if (ret < 0)
		goto e_imu;

	return 0;

e_imu:
	v4l2_device_unregister_subdev(&state->rgb.sensor.sd);

e_rgb:
	v4l2_device_unregister_subdev(&state->ir.sensor.sd);

e_depth:
	v4l2_device_unregister_subdev(&state->depth.sensor.sd);

	return ret;
}

static void d5x_mux_unregistered(struct v4l2_subdev *sd)
{
	struct d5x *state = v4l2_get_subdevdata(sd);
	d5x_sensor_remove(&state->imu.sensor);
	d5x_sensor_remove(&state->rgb.sensor);
	d5x_sensor_remove(&state->ir.sensor);
	d5x_sensor_remove(&state->depth.sensor);
}

static const struct v4l2_subdev_internal_ops d5x_mux_internal_ops = {
	.open = d5x_mux_open,
	.close = d5x_mux_close,
	.registered = d5x_mux_registered,
	.unregistered = d5x_mux_unregistered,
};

static int d5x_mux_register(struct i2c_client *c, struct d5x *state)
{
	return v4l2_async_register_subdev(&state->mux.sd.subdev);
}

static int d5x_hw_init(struct i2c_client *c, struct d5x *state)
{
	struct v4l2_subdev *sd = &state->mux.sd.subdev;
#if D5X_BYPASS_CAMERA_I2C
	dev_info(sd->dev, "%s(): BYPASS - skip camera MIPI config I2C\n", __func__);
	return 0;
#else
	u16 mipi_status, n_lanes, phy, drate_min, drate_max;
	int ret = d5x_read(state, D5X_MIPI_SUPPORT_LINES, &n_lanes);
	if (!ret)
		ret = d5x_read(state, D5X_MIPI_SUPPORT_PHY, &phy);

	if (!ret)
		ret = d5x_read(state, D5X_MIPI_DATARATE_MIN, &drate_min);

	if (!ret)
		ret = d5x_read(state, D5X_MIPI_DATARATE_MAX, &drate_max);

	if (!ret)
		dev_dbg(sd->dev, "%s(): %d: %u lanes, phy %x, data rate %u-%u\n",
			 __func__, __LINE__, n_lanes, phy, drate_min, drate_max);

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	n_lanes = state->mux.sd.numlanes;
#else
	n_lanes = 2;
#endif

	ret = d5x_write(state, D5X_MIPI_LANE_NUMS, n_lanes - 1);
	if (!ret)
		ret = d5x_write(state, D5X_MIPI_LANE_DATARATE, MIPI_LANE_RATE);

	if (!ret)
		ret = d5x_read(state, D5X_MIPI_CONF_STATUS, &mipi_status);

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	dev_dbg(sd->dev, "%s(): %d phandle %x node %s status %x\n", __func__, __LINE__,
		 c->dev.of_node->phandle, c->dev.of_node->full_name, mipi_status);
#endif

	return ret;
#endif /* !D5X_BYPASS_CAMERA_I2C */
}

static int d5x_mux_init(struct i2c_client *c, struct d5x *state)
{
	struct v4l2_subdev *sd = &state->mux.sd.subdev;
	struct media_entity *entity = &state->mux.sd.subdev.entity;
	struct media_pad *pads = state->mux.pads, *pad;
	unsigned int i;
	int ret;
#ifndef CONFIG_OF
	struct d4xx_pdata *dpdata = c->dev.platform_data;
	char suffix = dpdata->suffix;
#endif
	v4l2_i2c_subdev_init(sd, c, &d5x_mux_subdev_ops);
	/* See tegracam_v4l2.c tegracam_v4l2subdev_register() */
	/* Set owner to NULL so we can unload the driver module */
	sd->owner = NULL;
	sd->internal_ops = &d5x_mux_internal_ops;
	v4l2_set_subdevdata(sd, state);
#ifdef CONFIG_OF
	snprintf(sd->name, sizeof(sd->name), D5X_DRIVER_NAME_MUX " %d-%04x",
		 i2c_adapter_id(c->adapter), c->addr);
#else
	if (state->aggregated)
		suffix += 4;
	snprintf(sd->name, sizeof(sd->name), D5X_DRIVER_NAME_MUX " %c", suffix);
#endif
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	entity->obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV;
	entity->function = MEDIA_ENT_F_CAM_SENSOR;

	pads[0].flags = MEDIA_PAD_FL_SOURCE;
	for (i = 1, pad = pads + 1; i < ARRAY_SIZE(state->mux.pads); i++, pad++)
		pad->flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(entity, ARRAY_SIZE(state->mux.pads), pads);
	if (ret < 0)
		return ret;

	/*set for mux*/
	ret = d5x_ctrl_init(state, MUX_SID);
	if (ret < 0)
		goto e_entity;

	/*set for depth*/
	ret = d5x_ctrl_init(state, DEPTH_SID);
	if (ret < 0)
		return ret;
	/*set for rgb*/
	ret = d5x_ctrl_init(state, RGB_SID);
	if (ret < 0)
		return ret;
	/*set for y8*/
	ret = d5x_ctrl_init(state, IR_SID);
	if (ret < 0)
		return ret;
	/*set for imu*/
	ret = d5x_ctrl_init(state, IMU_SID);
	if (ret < 0)
		return ret;

	d5x_set_state_last_set(state);

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	if (state->is_depth) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_depth, NULL);
#else
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_depth, NULL, true);
#endif
		state->mux.last_set = &state->depth.sensor;
	}
	else if (state->is_rgb) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_rgb, NULL);
#else
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_rgb, NULL, true);
#endif
		state->mux.last_set = &state->rgb.sensor;
	}
	else if (state->is_y8) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_y8, NULL);
#else
		v4l2_ctrl_add_handler(&state->ctrls.handler,
					&state->ctrls.handler_y8, NULL, true);
#endif
		state->mux.last_set = &state->ir.sensor;
	}
	else
		state->mux.last_set = &state->imu.sensor;

	state->mux.sd.dev = &c->dev;
#if defined(CONFIG_TEGRA_CAMERA_PLATFORM) && defined(CONFIG_TEGRA_EMBEDDED_METADATA_OPS)
	state->mux.sd.embedded_metadata_ops = &d5x_csi_metadata_ops;
#endif
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	state->mux.sd.frame_postprocess_ops = &d5x_frame_postprocess_ops;
#endif
	ret = camera_common_initialize(&state->mux.sd, "d5xx");
	if (ret) {
		dev_err(&c->dev, "Failed to initialize d5xx.\n");
		goto e_ctrl;
	}
#endif

	return 0;

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
e_ctrl:
	v4l2_ctrl_handler_free(sd->ctrl_handler);
#endif
e_entity:
	media_entity_cleanup(entity);

	return ret;
}

#define USE_Y

static int d5x_fixed_configuration(struct i2c_client *client, struct d5x *state)
{
	struct d5x_sensor *sensor;
	u16 cfg0 = 0, cfg0_md = 0, cfg1 = 0, cfg1_md = 0;
	u16 dw = 0, dh = 0, yw = 0, yh = 0, dev_type = 0;
	int ret;

#if D5X_BYPASS_CAMERA_I2C
	/* Hardcode D58X configuration - no camera I2C available */
	dev_info(&client->dev, "%s(): BYPASS - using hardcoded D58X config\n", __func__);
	cfg0 = 0x1e;   /* depth DT: GMSL_CSI_DT_YUV422_8 */
	dw = 1280;
	dh = 720;
	cfg1 = 0x2a;   /* IR DT: RAW8 */
	yw = 1280;
	yh = 720;
	dev_type = D5X_DEVICE_TYPE_D58X;
	ret = 0;
#else
	ret = d5x_read(state, D5X_DEPTH_STREAM_DT, &cfg0);
	if (!ret)
		ret = d5x_read(state, D5X_DEPTH_STREAM_MD, &cfg0_md);
	if (!ret)
		ret = d5x_read(state, D5X_DEPTH_RES_WIDTH, &dw);
	if (!ret)
		ret = d5x_read(state, D5X_DEPTH_RES_HEIGHT, &dh);
	if (!ret)
		ret = d5x_read(state, D5X_IR_STREAM_DT, &cfg1);
	if (!ret)
		ret = d5x_read(state, D5X_IR_STREAM_MD, &cfg1_md);
	if (!ret)
		ret = d5x_read(state, D5X_IR_RES_WIDTH, &yw);
	if (!ret)
		ret = d5x_read(state, D5X_IR_RES_HEIGHT, &yh);
	if (!ret)
		ret = d5x_read(state, D5X_DEVICE_TYPE, &dev_type);
	if (ret < 0)
		return ret;
#endif

	dev_dbg(&client->dev, "%s(): cfg0 %x %ux%u cfg0_md %x %ux%u\n", __func__,
		 cfg0, dw, dh, cfg0_md, yw, yh);

	dev_dbg(&client->dev, "%s(): cfg1 %x %ux%u cfg1_md %x %ux%u\n", __func__,
		 cfg1, dw, dh, cfg1_md, yw, yh);

	sensor = &state->depth.sensor;
	dev_type = d5x_dev_type(state, dev_type);
	switch (dev_type) {
	case D5X_DEVICE_TYPE_D58X:
	default:
		if (dev_type != D5X_DEVICE_TYPE_D58X)
			dev_warn(&client->dev,
				"%s(): unknown device type 0x%x, using D58X format tables\n",
				__func__, dev_type);
		sensor->formats = d5x_depth_formats_d58x;
	}
	sensor->n_formats = 1;
	sensor->mux_pad = D5X_MUX_PAD_DEPTH;

	sensor = &state->ir.sensor;
	switch (dev_type) {
	case D5X_DEVICE_TYPE_D58X:
		sensor->formats = d5x_y_formats_d58x;
		sensor->n_formats = ARRAY_SIZE(d5x_y_formats_d58x);
		break;
	default:
		sensor->formats = state->variant->formats;
		sensor->n_formats = state->variant->n_formats;
	}
	sensor->mux_pad = D5X_MUX_PAD_IR;

	sensor = &state->rgb.sensor;
	switch (dev_type) {
	case D5X_DEVICE_TYPE_D58X:
	default:
		sensor->formats = d5x_rgb_formats_d58x;
		sensor->n_formats = ARRAY_SIZE(d5x_rgb_formats_d58x);
	}
	sensor->mux_pad = D5X_MUX_PAD_RGB;

	sensor = &state->imu.sensor;

	/* 
	 * For fimware version starting from: 5.16,
	 * IMU will have 32bit axis values.
 	 * 5.16.x.y = firmware version: 0x0510
	 */
	if (state->fw_version >= 0x510)
		sensor->formats = d5x_imu_formats_extended;
	else
		sensor->formats = d5x_imu_formats;
	
	sensor->n_formats = 1;
	sensor->mux_pad = D5X_MUX_PAD_IMU;

	/* Development: set a configuration during probing */
	if ((cfg0 & 0xff00) == 0x1800) {
		/* MIPI CSI-2 YUV420 isn't supported by V4L, reconfigure to Y8 */
		struct v4l2_subdev_format fmt = {
			.which = V4L2_SUBDEV_FORMAT_ACTIVE,
			.pad = 0,
			/* Use template to fill in .field, .colorspace etc. */
			.format = d5x_mbus_framefmt_template,
		};

//#undef USE_Y
#ifdef USE_Y
		/* Override .width, .height, .code */
		fmt.format.width = yw;
		fmt.format.height = yh;
		fmt.format.code = MEDIA_BUS_FMT_UYVY8_2X8;
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
		state->mux.sd.mode_prop_idx = 0;
#endif
		state->ir.sensor.streaming = true;
		state->depth.sensor.streaming = true;
		ret = __d5x_sensor_set_fmt(state, &state->ir.sensor, NULL, &fmt);
#else
		fmt.format.width = dw;
		fmt.format.height = dh;
		fmt.format.code = MEDIA_BUS_FMT_UYVY8_1X16;
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
		state->mux.sd.mode_prop_idx = 1;
#endif
		state->ir.sensor.streaming = false;
		state->depth.sensor.streaming = true;
		ret = __d5x_sensor_set_fmt(state, &state->depth.sensor, NULL, &fmt);
#endif
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int d5x_parse_cam(struct i2c_client *client, struct d5x *state)
{
	int ret;

	ret = d5x_fixed_configuration(client, state);
	if (ret < 0)
		return ret;

	d5x_sensor_format_init(&state->depth.sensor);
	d5x_sensor_format_init(&state->ir.sensor);
	d5x_sensor_format_init(&state->rgb.sensor);
	d5x_sensor_format_init(&state->imu.sensor);

	return 0;
}

static void d5x_mux_remove(struct d5x *state)
{
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	camera_common_cleanup(&state->mux.sd);
#endif
	v4l2_async_unregister_subdev(&state->mux.sd.subdev);
	v4l2_ctrl_handler_free(state->mux.sd.subdev.ctrl_handler);
	media_entity_cleanup(&state->mux.sd.subdev.entity);
}

static const struct regmap_config d5x_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_NATIVE,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
};

static int d5x_dfu_wait_for_status(struct d5x *state)
{
	int i, ret = 0;
	u16 status;

	for (i = 0; i < D5X_START_MAX_COUNT; i++) {
		d5x_read(state, 0x5000, &status);
		if (status == 0x0001 || status == 0x0002) {
			dev_err(&state->client->dev,
					"%s(): dfu failed status(0x%4x)\n",
					__func__, status);
			ret = -EREMOTEIO;
			break;
		}
		if (!status)
			break;
		msleep_range(D5X_START_POLL_TIME);
	}

	return ret;
};

static int d5x_dfu_switch_to_dfu(struct d5x *state)
{
	int ret;
	int i = D5X_START_MAX_COUNT;
	u16 status;

	ret = d5x_hwmc_send(state, sizeof(cmd_switch_to_dfu),
			    (struct hwm_cmd *)&cmd_switch_to_dfu);
	if (ret)
		return ret;
	/*Wait for DFU fw to boot*/
	do {
		msleep_range(D5X_START_POLL_TIME*10);
		ret = d5x_read(state, 0x5000, &status);
	} while (ret && i--);
	return ret;
};

static int d5x_dfu_wait_for_get_dfu_status(struct d5x *state,
		enum dfu_fw_state exp_state)
{
	int ret = 0;
	u16 status, dfu_state_len = 0x0000;
	unsigned char dfu_asw_buf[DFU_WAIT_RET_LEN];
	unsigned int dfu_wr_wait_msec = 0;

	do {
		/* Get Write state */
		d5x_write_with_check(state, 0x5008, 0x0003);
		do {
			d5x_read_with_check(state, 0x5000, &status);
			if (status == 0x0001) {
				dev_err(&state->client->dev,
						"%s(): Write status error I2C_STATUS_ERROR(1)\n",
						__func__);
				return -EINVAL;
			} else
				if (status == 0x0002 && dfu_wr_wait_msec)
					msleep_range(dfu_wr_wait_msec);

		} while (status);

		d5x_read_with_check(state, 0x5004, &dfu_state_len);
		if (dfu_state_len != DFU_WAIT_RET_LEN) {
			dev_err(&state->client->dev,
					"%s(): Wrong answer len (%d)\n", __func__, dfu_state_len);
			return -EINVAL;
		}
		d5x_raw_read_with_check(state, 0x4e00, &dfu_asw_buf, DFU_WAIT_RET_LEN);
		if (dfu_asw_buf[0]) {
			dev_err(&state->client->dev,
					"%s(): Wrong dfu_status (%d)\n", __func__, dfu_asw_buf[0]);
			return -EINVAL;
		}
		dfu_wr_wait_msec = (((unsigned int)dfu_asw_buf[3]) << 16)
						| (((unsigned int)dfu_asw_buf[2]) << 8)
						| dfu_asw_buf[1];
	} while ((dfu_asw_buf[4] == dfuDNBUSY &&
		  exp_state == dfuDNLOAD_IDLE) ||
		 ((dfu_asw_buf[4] == dfuMANIFEST_SYNC ||
		   dfu_asw_buf[4] == dfuMANIFEST) &&
		  exp_state == dfuMANIFEST_WAIT_RESET));

	if (dfu_asw_buf[4] != exp_state) {
		dev_notice(&state->client->dev,
				"%s(): Wrong dfu_state (%d) while expected(%d)\n",
				__func__, dfu_asw_buf[4], exp_state);
		ret = -EINVAL;
	}
	return ret;
};

static int d5x_dfu_get_dev_info(struct d5x *state, struct __fw_status *buf)
{
	int ret = 0;
	u16 len = 0;

	ret = d5x_write(state, 0x5008, 0x0002); //Upload DFU cmd
	if (!ret)
		ret = d5x_dfu_wait_for_status(state);
	if (!ret)
		d5x_read_with_check(state, 0x5004, &len);
	/*Sanity check*/
	if (len == sizeof(struct __fw_status)) {
		d5x_raw_read_with_check(state, 0x4e00, buf, len);
	} else {
		dev_err(&state->client->dev,
				"%s(): Wrong state size (%d)\n",
				__func__, len);
		ret = -EINVAL;
	}
	return ret;
};

static int d5x_dfu_detach(struct d5x *state)
{
	int ret;
	struct __fw_status buf = {0};

	d5x_write_with_check(state, 0x500c, 0x00);
	ret = d5x_dfu_wait_for_get_dfu_status(state, dfuIDLE);
	if (!ret)
		ret = d5x_dfu_get_dev_info(state, &buf);
	dev_notice(&state->client->dev, "%s():DFU ver (0x%x) received\n",
			__func__, buf.DFU_version);
	dev_notice(&state->client->dev, "%s():FW last version (0x%x) received\n",
			__func__, buf.FW_lastVersion);
	dev_notice(&state->client->dev, "%s():FW status (%s)\n",
			__func__, buf.DFU_isLocked ? "locked" : "unlocked");
	return ret;
};

/* When a process reads from our device, this gets called. */
static ssize_t d5x_dfu_device_read(struct file *flip,
		char __user *buffer, size_t len, loff_t *offset)
{
	struct d5x *state = flip->private_data;
	u16 fw_ver, fw_build;
	char msg[64];
	int ret = 0;
	struct __fw_status f = {0};

	if (mutex_lock_interruptible(&state->lock))
		return -ERESTARTSYS;
	if (state->dfu_dev.dfu_state_flag == D5X_DFU_RECOVERY) {
		/* Read device info in recovery mode */
		ret = d5x_dfu_detach(state);
		if (ret < 0)
			goto e_dfu_read_failed;
		ret = d5x_dfu_get_dev_info(state, &f);
		if (ret < 0)
			goto e_dfu_read_failed;
		snprintf(msg, sizeof(msg) ,
			 "DFU info: \trecovery:  %02x%02x%02x%02x%02x%02x\n",
			 f.ivcamSerialNum[0], f.ivcamSerialNum[1], f.ivcamSerialNum[2],
			 f.ivcamSerialNum[3], f.ivcamSerialNum[4], f.ivcamSerialNum[5] );
	} else {
		ret |= d5x_read(state, D5X_FW_VERSION, &fw_ver);
		ret |= d5x_read(state, D5X_FW_BUILD, &fw_build);
		if (ret < 0)
			goto e_dfu_read_failed;
		snprintf(msg, sizeof(msg) ,"DFU info: \tver:  %d.%d.%d.%d\n",
			(fw_ver >> 8) & 0xff, fw_ver & 0xff,
			(fw_build >> 8) & 0xff, fw_build & 0xff);
	}

	if (copy_to_user(buffer, msg, strlen(msg)))
		ret = -EFAULT;
	else {
		state->dfu_dev.msg_write_once = ~state->dfu_dev.msg_write_once;
		ret = strlen(msg) & state->dfu_dev.msg_write_once;
	}

e_dfu_read_failed:
	mutex_unlock(&state->lock);
	return ret;
};

static ssize_t d5x_dfu_device_write(struct file *flip,
		const char __user *buffer, size_t len, loff_t *offset)
{
	struct d5x *state = flip->private_data;
	int ret = 0;
	(void)offset;

	if (mutex_lock_interruptible(&state->lock))
		return -ERESTARTSYS;
	switch (state->dfu_dev.dfu_state_flag) {

	case D5X_DFU_OPEN:
		ret = d5x_dfu_switch_to_dfu(state);
		if (ret < 0) {
			dev_err(&state->client->dev, "%s(): Switch to dfu failed (%d)\n",
					__func__, ret);
			goto dfu_write_error;
		}
	/* fallthrough - procceed to recovery */
	__attribute__((__fallthrough__));
	case D5X_DFU_RECOVERY:
		ret = d5x_dfu_detach(state);
		if (ret < 0) {
			dev_err(&state->client->dev, "%s(): Detach failed (%d)\n",
					__func__, ret);
			goto dfu_write_error;
		}
		state->dfu_dev.dfu_state_flag = D5X_DFU_IN_PROGRESS;
	__attribute__((__fallthrough__));
	case D5X_DFU_IN_PROGRESS: {
		unsigned int dfu_full_blocks = len / DFU_BLOCK_SIZE;
		unsigned int dfu_part_blocks = len % DFU_BLOCK_SIZE;

		while (dfu_full_blocks--) {
			if (copy_from_user(state->dfu_dev.dfu_msg, buffer, DFU_BLOCK_SIZE)) {
				ret = -EFAULT;
				goto dfu_write_error;
			}
			ret = d5x_raw_write(state, 0x4a00,
					state->dfu_dev.dfu_msg, DFU_BLOCK_SIZE);
			if (ret < 0)
				goto dfu_write_error;
			ret = d5x_dfu_wait_for_get_dfu_status(state, dfuDNLOAD_IDLE);
			if (ret < 0)
				goto dfu_write_error;
			buffer += DFU_BLOCK_SIZE;
		}
		if (copy_from_user(state->dfu_dev.dfu_msg, buffer, dfu_part_blocks)) {
				ret = -EFAULT;
				goto dfu_write_error;
		}
		if (dfu_part_blocks) {
			ret = d5x_raw_write(state, 0x4a00,
					state->dfu_dev.dfu_msg, dfu_part_blocks);
			if (!ret)
				ret = d5x_dfu_wait_for_get_dfu_status(state, dfuDNLOAD_IDLE);
			if (!ret)
				ret = d5x_write(state, 0x4a04, 0x00); /*Download complete */
			if (!ret)
				ret = d5x_dfu_wait_for_get_dfu_status(state,
								      dfuMANIFEST_WAIT_RESET);
			if (ret < 0)
				goto dfu_write_error;
			state->dfu_dev.dfu_state_flag = D5X_DFU_DONE;
		}
		if (len)
			dev_notice(&state->client->dev, "%s(): DFU block (%d) bytes written\n",
				__func__, (int)len);
		break;
	}
	default:
		dev_err(&state->client->dev, "%s(): Wrong state (%d)\n",
				__func__, state->dfu_dev.dfu_state_flag);
		ret = -EINVAL;
		goto dfu_write_error;

	};
	mutex_unlock(&state->lock);
	return len;

dfu_write_error:
	state->dfu_dev.dfu_state_flag = D5X_DFU_ERROR;
	/* Reset DFU device to IDLE states */
	if (!d5x_write(state, 0x5010, 0x0))
		state->dfu_dev.dfu_state_flag = D5X_DFU_IDLE;
	mutex_unlock(&state->lock);
	return ret;
};

static int d5x_dfu_device_open(struct inode *inode, struct file *file)
{
	struct d5x *state = container_of(inode->i_cdev, struct d5x,
			dfu_dev.d5x_cdev);
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(
			state->client->adapter);
#endif
	mutex_lock(&state->lock);
	if (state->dfu_dev.device_open_count) {
		mutex_unlock(&state->lock);
		return -EBUSY;
	}
	state->dfu_dev.device_open_count++;
	if (state->dfu_dev.dfu_state_flag != D5X_DFU_RECOVERY)
		state->dfu_dev.dfu_state_flag = D5X_DFU_OPEN;
	state->dfu_dev.dfu_msg = devm_kzalloc(&state->client->dev,
			DFU_BLOCK_SIZE, GFP_KERNEL);
	if (!state->dfu_dev.dfu_msg) {
		mutex_unlock(&state->lock);
		return -ENOMEM;
	}
	file->private_data = state;
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	/* Get i2c controller and set dfu bus clock rate */
	while (parent && i2c_parent_is_i2c_adapter(parent))
		parent = i2c_parent_is_i2c_adapter(state->client->adapter);

	if (!parent) {
		mutex_unlock(&state->lock);
		return 0;
	}
	dev_dbg(&state->client->dev, "%s(): i2c-%d bus_clk = %d, set %d\n",
			__func__,
			i2c_adapter_id(parent),
			i2c_get_adapter_bus_clk_rate(parent),
			DFU_I2C_BUS_CLK_RATE);

	state->dfu_dev.bus_clk_rate = i2c_get_adapter_bus_clk_rate(parent);
	i2c_set_adapter_bus_clk_rate(parent, DFU_I2C_BUS_CLK_RATE);
#endif
	mutex_unlock(&state->lock);
	return 0;
};

/* 
 * Adjust sync_mode control range based on device type.
 * Must be called after d5x_mux_init() which creates the control.
 */
static void d5x_adjust_sync_mode_control(struct i2c_client *client, struct d5x *state)
{
	u16 dev_type = 0;
#if !D5X_BYPASS_CAMERA_I2C
	int ret;
#endif

	if (!state->ctrls.sync_mode)
		return;

	#if D5X_BYPASS_CAMERA_I2C
	dev_type = D5X_DEVICE_TYPE_D58X;
#else
	ret = d5x_read(state, D5X_DEVICE_TYPE, &dev_type);
	if (ret < 0) {
		dev_warn(&client->dev, "%s(): Failed to read device type\n", __func__);
		return;
	}
	dev_type = d5x_dev_type(state, dev_type);
#endif
	switch (dev_type) {
	case D5X_DEVICE_TYPE_D58X:
		/* HWM supports 0=Disabled, 2=PWM Master and 3=External. */
		__v4l2_ctrl_modify_range(state->ctrls.sync_mode,
					 D5X_SYNC_MODE_DISABLED,
					 D5X_SYNC_MODE_EXTERNAL, 0,
					 D5X_SYNC_MODE_DISABLED);
		state->ctrls.sync_mode->qmenu = sync_mode_menu;
		dev_dbg(&client->dev,
			"%s(): D58X HWM sync modes: 0=Disabled, 2=PWM Master, 3=External\n",
			__func__);
		break;
	default:
		/* Unknown device - disable sync mode */
		dev_warn(&client->dev, "%s(): Unknown device type %d, disabling sync mode\n",
			__func__, dev_type);
		__v4l2_ctrl_modify_range(state->ctrls.sync_mode, 0, 0, 0, 0);
		break;
	}
}

static int d5x_v4l_init(struct i2c_client *c, struct d5x *state)
{
	int ret;

	ret = d5x_parse_cam(c, state);
	if (ret < 0)
		return ret;

	ret = d5x_depth_init(c, state);
	if (ret < 0)
		return ret;

	ret = d5x_ir_init(c, state);
	if (ret < 0)
		goto e_depth;

	ret = d5x_rgb_init(c, state);
	if (ret < 0)
		goto e_ir;

	ret = d5x_imu_init(c, state);
	if (ret < 0)
		goto e_rgb;

	ret = d5x_mux_init(c, state);
	if (ret < 0)
		goto e_imu;

	/* 
	 * Adjust sync_mode control range based on device type - must be done
	 * after d5x_mux_init() creates the control
	 */
	d5x_adjust_sync_mode_control(c, state);

	ret = d5x_hw_init(c, state);
	if (ret < 0)
		goto e_mux;

	ret = d5x_mux_register(c, state);
	if (ret < 0)
		goto e_mux;

	return 0;
e_mux:
	d5x_mux_remove(state);
e_imu:
	media_entity_cleanup(&state->imu.sensor.sd.entity);
e_rgb:
	media_entity_cleanup(&state->rgb.sensor.sd.entity);
e_ir:
	media_entity_cleanup(&state->ir.sensor.sd.entity);
e_depth:
	media_entity_cleanup(&state->depth.sensor.sd.entity);
	return ret;
}

static int d5x_dfu_device_release(struct inode *inode, struct file *file)
{
	struct d5x *state = container_of(inode->i_cdev, struct d5x, dfu_dev.d5x_cdev);
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(
			state->client->adapter);
#endif
	int ret = 0, retry = 10;
	mutex_lock(&state->lock);
	state->dfu_dev.device_open_count--;
	if (state->dfu_dev.dfu_state_flag != D5X_DFU_RECOVERY)
		state->dfu_dev.dfu_state_flag = D5X_DFU_IDLE;
	/* We disable this section as it has no effect when device in operational
	   mode and has not enough effect when device in recovery mode */
	// if (state->dfu_dev.dfu_state_flag == D5X_DFU_DONE
	// 		&& state->dfu_dev.init_v4l_f)
	// 	d5x_v4l_init(state->client, state);
	// state->dfu_dev.init_v4l_f = 0;
	if (state->dfu_dev.dfu_msg)
		devm_kfree(&state->client->dev, state->dfu_dev.dfu_msg);
	state->dfu_dev.dfu_msg = NULL;
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
	/* get i2c controller and restore bus clock rate */
	while (parent && i2c_parent_is_i2c_adapter(parent))
		parent = i2c_parent_is_i2c_adapter(state->client->adapter);
	if (!parent) {
		mutex_unlock(&state->lock);
		return 0;
	}
	dev_dbg(&state->client->dev, "%s(): i2c-%d bus_clk %d, restore to %d\n",
			__func__, i2c_adapter_id(parent),
			i2c_get_adapter_bus_clk_rate(parent),
			state->dfu_dev.bus_clk_rate);

	i2c_set_adapter_bus_clk_rate(parent, state->dfu_dev.bus_clk_rate);
#endif
	/* Verify communication */
	do {
		ret = d5x_read(state, D5X_FW_VERSION, &state->fw_version);
		if (ret)
			msleep_range(10);
	} while (retry-- && ret != 0 );
	if (ret) {
		dev_warn(&state->client->dev,
			"%s(): no communication with d4xx\n", __func__);
		mutex_unlock(&state->lock);
		return ret;
	}
	ret = d5x_read(state, D5X_FW_BUILD, &state->fw_build);
	mutex_unlock(&state->lock);
	return ret;
};

static const struct file_operations d5x_device_file_ops = {
	.owner = THIS_MODULE,
	.read = &d5x_dfu_device_read,
	.write = &d5x_dfu_device_write,
	.open = &d5x_dfu_device_open,
	.release = &d5x_dfu_device_release
};

struct class *d5x_global_class;
atomic_t d5x_primary_chardev = ATOMIC_INIT(0);

static int d5x_chrdev_init(struct i2c_client *c, struct d5x *state)
{
	struct cdev *d5x_cdev = &state->dfu_dev.d5x_cdev;
	struct class **d5x_class = &state->dfu_dev.d5x_class;
#ifndef CONFIG_OF
	struct d4xx_pdata *pdata = c->dev.platform_data;
	char suffix = pdata->suffix;
#endif
	struct device *chr_dev;
	char dev_name[sizeof(D5X_DRIVER_NAME_DFU) + 8];
	dev_t *dev_num = &c->dev.devt;
	int ret;

	dev_dbg(&c->dev, "%s()\n", __func__);
	/* Request the kernel for N_MINOR devices */
	ret = alloc_chrdev_region(dev_num, 0, 1, D5X_DRIVER_NAME_DFU);
	if (ret < 0)
		return ret;

	if (!atomic_read(&d5x_primary_chardev)) {
		dev_dbg(&c->dev, "%s(): <Major, Minor>: <%d, %d>\n",
				__func__, MAJOR(*dev_num), MINOR(*dev_num));
		/* Create a class : appears at /sys/class */
#if defined(NV_CLASS_CREATE_HAS_NO_OWNER_ARG) || LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
		*d5x_class = class_create(THIS_MODULE, D5X_DRIVER_NAME_CLASS);
#else
		*d5x_class = class_create(D5X_DRIVER_NAME_CLASS);
#endif
		dev_warn(&state->client->dev, "%s() class_create\n", __func__);
		if (IS_ERR(*d5x_class)) {
			dev_err(&c->dev, "Could not create class device\n");
			unregister_chrdev_region(0, 1);
			ret = PTR_ERR(*d5x_class);
			return ret;
		}
		d5x_global_class = *d5x_class;
	} else
		*d5x_class = d5x_global_class;
	/* Associate the cdev with a set of file_operations */
	cdev_init(d5x_cdev, &d5x_device_file_ops);
	/* Build up the current device number. To be used further */
	*dev_num = MKDEV(MAJOR(*dev_num), MINOR(*dev_num));
	/* Create a device node for this device. */
#ifndef CONFIG_OF
	if (state->aggregated)
		suffix += 4;
	snprintf(dev_name, sizeof(dev_name), "%s-%c",
		D5X_DRIVER_NAME_DFU, suffix);
#else
	snprintf (dev_name, sizeof(dev_name), "%s-%d-%04x",
			D5X_DRIVER_NAME_DFU, i2c_adapter_id(c->adapter), c->addr);
#endif
	chr_dev = device_create(*d5x_class, NULL, *dev_num, NULL, dev_name);
	if (IS_ERR(chr_dev)) {
		ret = PTR_ERR(chr_dev);
		dev_err(&c->dev, "Could not create device\n");
		class_destroy(*d5x_class);
		unregister_chrdev_region(0, 1);
		return ret;
	}
	cdev_add(d5x_cdev, *dev_num, 1);
	atomic_inc(&d5x_primary_chardev);
	return 0;
};

static int d5x_chrdev_remove(struct d5x *state)
{
	struct class **d5x_class = &state->dfu_dev.d5x_class;
	dev_t *dev_num = &state->client->dev.devt;
	if (!d5x_class) {
		return 0;
	}
	dev_dbg(&state->client->dev, "%s()\n", __func__);
	unregister_chrdev_region(*dev_num, 1);
	device_destroy(*d5x_class, *dev_num);
	if (atomic_dec_and_test(&d5x_primary_chardev)) {
		dev_warn(&state->client->dev, "%s() class_destroy\n", __func__);
		class_destroy(*d5x_class);
	}
	return 0;
}

/* SYSFS attributes */
#ifdef CONFIG_SYSFS
static ssize_t d5x_fw_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *c = to_i2c_client(dev);
	struct d5x *state = container_of(i2c_get_clientdata(c),
			struct d5x, mux.sd.subdev);

	d5x_read(state, D5X_FW_VERSION, &state->fw_version);
	d5x_read(state, D5X_FW_BUILD, &state->fw_build);

	return snprintf(buf, PAGE_SIZE, "D4XX Sensor: %s, Version: %d.%d.%d.%d\n",
			d5x_get_sensor_name(state),
			(state->fw_version >> 8) & 0xff, state->fw_version & 0xff,
			(state->fw_build >> 8) & 0xff, state->fw_build & 0xff);
}

static DEVICE_ATTR_RO(d5x_fw_ver);

/* Derive 'device_attribute' structure for a read register's attribute */
struct dev_d5x_reg_attribute {
	struct device_attribute attr;
	u16 reg;
	u8 valid;
};

/*
 * Read a D5xx register.
 * d5x_read_reg_show will actually read register from d5x while
 * d5x_read_reg_store will store register to read
 * Example:
 * echo -n "0xc03c" >d5x_read_reg
 * Read register result:
 * cat d5x_read_reg
 * Expected:
 * reg:0xc93c, result:0x11
 */
static ssize_t d5x_read_reg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u16 rbuf;
	int n;
	struct i2c_client *c = to_i2c_client(dev);
	struct d5x *state = container_of(i2c_get_clientdata(c),
			struct d5x, mux.sd.subdev);
	struct dev_d5x_reg_attribute *d5x_rw_attr = container_of(attr,
			struct dev_d5x_reg_attribute, attr);
	if (d5x_rw_attr->valid != 1)
		return -EINVAL;
	d5x_read(state, d5x_rw_attr->reg, &rbuf);

	n = snprintf(buf, PAGE_SIZE, "register:0x%4x, value:0x%02x\n",
			d5x_rw_attr->reg, rbuf);

	return n;
}

/* 
 * Select the D5xx register exposed through the read attribute.
 * d5x_read_reg_show will actually read register from d5x while
 * d5x_read_reg_store will store module, offset and length
 */
static ssize_t d5x_read_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct dev_d5x_reg_attribute *d5x_rw_attr = container_of(attr,
			struct dev_d5x_reg_attribute, attr);
	int rc = -1;
	u32 reg;
	d5x_rw_attr->valid = 0;
	/* Decode input */
	rc = sscanf(buf, "0x%04x", &reg);
	if (rc != 1)
		return -EINVAL;
	d5x_rw_attr->reg = reg;
	d5x_rw_attr->valid = 1;
	return count;
}

#define D5X_RW_REG_ATTR(_name) \
		struct dev_d5x_reg_attribute dev_attr_##_name = { \
			__ATTR(_name, S_IRUGO | S_IWUSR, \
			d5x_read_reg_show, d5x_read_reg_store), \
			0, 0 }

static D5X_RW_REG_ATTR(d5x_read_reg);

static ssize_t d5x_write_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *c = to_i2c_client(dev);
	struct d5x *state = container_of(i2c_get_clientdata(c),
			struct d5x, mux.sd.subdev);

	int rc = -1;
	u32 reg, w_val = 0;
	u16 val = -1;
	/* Decode input */
	rc = sscanf(buf, "0x%04x 0x%04x", &reg, &w_val);
	if (rc != 2)
		return -EINVAL;
	val = w_val & 0xffff;
	mutex_lock(&state->lock);
	d5x_write(state, reg, val);
	mutex_unlock(&state->lock);
	return count;
}

static DEVICE_ATTR_WO(d5x_write_reg);

static struct attribute *d5x_attributes[] = {
		&dev_attr_d5x_fw_ver.attr,
		&dev_attr_d5x_read_reg.attr.attr,
		&dev_attr_d5x_write_reg.attr,
		NULL
};

static const struct attribute_group d5x_attr_group = {
	.attrs = d5x_attributes,
};
#endif /* CONFIG_SYSFS */

static int d5x_probe(struct i2c_client *c
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
		, const struct i2c_device_id *id
#endif
		)
{
	struct d5x *state = devm_kzalloc(&c->dev, sizeof(*state), GFP_KERNEL);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	const struct i2c_device_id *id = i2c_client_get_device_id(c);
#endif
	u16 rec_state;
	int ret, err = 0;
#ifdef CONFIG_OF
	const char *str;
	uint32_t override_addr = 0;
	struct device_node *mode0_node;
#endif
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);
	state->client = c;

	dev_warn(&c->dev, "Probing driver for D5xx\n");
#ifdef CONFIG_OF
	ret = of_property_read_u32(c->dev.of_node, "override_reg", &override_addr);
	if (!ret) {
		/* Override probed address */
		dev_dbg(&c->dev, "Using override addr 0x%x\n", override_addr);
		c->addr = override_addr;
	}
#endif
	state->variant = d5x_variants + id->driver_data;
#ifdef CONFIG_OF
	state->vcc = devm_regulator_get(&c->dev, "vcc");
	if (IS_ERR(state->vcc)) {
		ret = PTR_ERR(state->vcc);
		dev_warn(&c->dev, "failed %d to get vcc regulator\n", ret);
		return ret;
	}

	if (state->vcc) {
		ret = regulator_enable(state->vcc);
		if (ret < 0) {
			dev_warn(&c->dev, "failed %d to enable the vcc regulator\n", ret);
			return ret;
		}
	}
#endif
	state->regmap = devm_regmap_init_i2c(c, &d5x_regmap_config);
	if (IS_ERR(state->regmap)) {
		ret = PTR_ERR(state->regmap);
		dev_err(&c->dev, "regmap init failed: %d\n", ret);
		goto e_regulator;
	}

#ifdef CONFIG_VIDEO_D5XX_SERDES
	ret = d5x_serdes_setup(state);
	if (ret < 0)
		goto e_regulator;
	state->reset_ref_dser = atomic_read(dser_get_reset_gen(state));
#else
	d5x_init_global_slots_once();
	mutex_lock(&d5x_inited[0].lock);
	if (NULL == d5x_inited[0].d5x_primary) {
		d5x_init_d5x_dev(state, &d5x_inited[0]);
		dev_dbg(&c->dev, "%s(): set primary d5x instance\n", __func__);
	}
	state->d5x_dev = &d5x_inited[0];
	mutex_unlock(&d5x_inited[0].lock);
#endif
	state->reset_ref_d5x = atomic_read(d5x_get_reset_gen(state));

#if D5X_BYPASS_CAMERA_I2C
	/* Skip camera I2C verification - use hardcoded FW version */
	state->fw_version = 0x0516;
	dev_info(&c->dev, "%s(): BYPASS - skip camera I2C verify, fake FW 5.22\n", __func__);
#else
	/* Verify communication */
	ret = d5x_read(state, D5X_FW_VERSION, &state->fw_version);
	if (ret < 0) {
		dev_err(&c->dev,
			"%s(): cannot communicate with D4XX: %d on addr: 0x%x\n",
			__func__, ret, c->addr);
		goto e_regulator;
	}
#endif

	state->is_depth = 0;
	state->is_y8 = 0;
	state->is_rgb = 0;
	state->is_imu = 0;
#ifdef CONFIG_OF
	ret = of_property_read_string(c->dev.of_node, "cam-type", &str);
	if (!ret && !strncmp(str, "Depth", strlen("Depth"))) {
		state->is_depth = 1;
		state->control_base = D5X_DEPTH_CONTROL_BASE;
		state->control_status_reg = D5X_DEPTH_CONTROL_STATUS;
	}
	if (!ret && !strncmp(str, "Y8", strlen("Y8"))) {
		state->is_y8 = 1;
		state->control_base = D5X_DEPTH_CONTROL_BASE;
		state->control_status_reg = D5X_DEPTH_CONTROL_STATUS;
	}
	if (!ret && !strncmp(str, "RGB", strlen("RGB"))) {
		state->is_rgb = 1;
		state->control_base = D5X_RGB_CONTROL_BASE;
		state->control_status_reg = D5X_RGB_CONTROL_STATUS;
	}
	if (!ret && !strncmp(str, "IMU", strlen("IMU"))) {
		state->is_imu = 1;
		state->control_base = D5X_DEPTH_CONTROL_BASE;
		state->control_status_reg = D5X_DEPTH_CONTROL_STATUS;
	}

	mode0_node = of_get_child_by_name(state->client->dev.of_node, "mode0");
	if (mode0_node) {
		ret = of_property_read_string(mode0_node, "embedded_metadata_height", &str);
		if (!ret && !strncmp(str, "1", 1)) {
				state->metadata_enabled = 1;
		} else {
				state->metadata_enabled = 0;
		}
		of_node_put(mode0_node);
	} else {
		dev_err(&state->client->dev, "No mode0 provided\n");
		goto e_regulator;
	}

	if (ret < 0) {
		dev_err(&state->client->dev, "No embedded_metadata_height provided\n");
		goto e_regulator;
	}
	dev_dbg(&state->client->dev, "metadata_enabled = %d\n", state->metadata_enabled);
#else
	state->is_depth = 1;
	state->control_base = D5X_DEPTH_CONTROL_BASE;
	state->control_status_reg = D5X_DEPTH_CONTROL_STATUS;
#endif

	if (!state->control_base) {
		state->control_base = D5X_DEPTH_CONTROL_BASE;
		state->control_status_reg = D5X_DEPTH_CONTROL_STATUS;
	}
	/* create DFU chardev once */
	if (state->is_depth) {
		ret = d5x_chrdev_init(c, state);
		if (ret < 0)
			goto e_regulator;
	}

	/* 
	 * Verify format-discovery readiness.
	 * FW_VERSION becomes readable earlier than D5X_DEVICE_TYPE, while later
	 * probe code depends on DEVICE_TYPE to pick the correct format tables.
	 */
#if D5X_BYPASS_CAMERA_I2C
	rec_state = D5X_DEVICE_TYPE_D58X;
	WRITE_ONCE(state->d5x_dev->cached_device_type, rec_state);
	dev_info(&c->dev, "%s(): BYPASS - hardcode device type D58X\n", __func__);
#else
	ret = d5x_wait_device_type(state, &rec_state);
	if (ret < 0) {
		dev_err(&c->dev,
			"%s(): device type is not valid: %d (last val 0x%x)\n",
			__func__, ret, rec_state);
		goto e_chardev;
	}

	ret = d5x_read(state, D5X_DFU_MAGIC_REG, &rec_state);
	if (ret < 0)
		rec_state = 0;

	if (rec_state == D5X_DFU_MAGIC_LSW) {
		dev_info(&c->dev, "%s(): D4XX recovery state\n", __func__);
		state->dfu_dev.dfu_state_flag = D5X_DFU_RECOVERY;
		/* Override I2C drvdata with state for use in remove function */
		i2c_set_clientdata(c, state);
		return 0;
	}
#endif

#if D5X_BYPASS_CAMERA_I2C
	state->fw_build = 0x0100;
#else
	d5x_read_with_check(state, D5X_FW_VERSION, &state->fw_version);
	d5x_read_with_check(state, D5X_FW_BUILD, &state->fw_build);
#endif

	dev_info(&c->dev, "D4XX Sensor: %s, firmware build: %d.%d.%d.%d\n",
			d5x_get_sensor_name(state),
			(state->fw_version >> 8) & 0xff, state->fw_version & 0xff,
			(state->fw_build >> 8) & 0xff, state->fw_build & 0xff);

	ret = d5x_v4l_init(c, state);
	if (ret < 0)
		goto e_chardev;

	dev_info(&c->dev, "%s: driver version: %s\n", __func__,
		THIS_MODULE->version ? THIS_MODULE->version : "N/A");

#ifdef CONFIG_SYSFS
	/* Custom sysfs attributes */
	/* create the sysfs file group */
	err = sysfs_create_group(&state->client->dev.kobj, &d5x_attr_group);
#endif
	return 0;

e_chardev:
	if (state->dfu_dev.d5x_class)
		d5x_chrdev_remove(state);
e_regulator:
#ifdef CONFIG_VIDEO_D5XX_SERDES
	d5x_serdes_cleanup(state);
#endif
	if (state->vcc)
		regulator_disable(state->vcc);
#ifdef CONFIG_VIDEO_D5XX_SERDES
#ifndef CONFIG_OF
	if (state->ser_i2c)
		i2c_unregister_device(state->ser_i2c);
	if (state->dser_i2c && !state->aggregated)
		i2c_unregister_device(state->dser_i2c);
#endif
#endif
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
static int d5x_remove(struct i2c_client *c)
#else
static void d5x_remove(struct i2c_client *c)
#endif
{
	struct d5x *state = container_of(i2c_get_clientdata(c), struct d5x, mux.sd.subdev);
	if (state && !state->mux.sd.subdev.v4l2_dev) {
		state = i2c_get_clientdata(c);
	}

#ifdef CONFIG_VIDEO_D5XX_SERDES
	d5x_serdes_cleanup(state);
#ifndef CONFIG_OF
	if (state->ser_i2c)
		i2c_unregister_device(state->ser_i2c);
	if (state->dser_i2c && !state->aggregated)
		i2c_unregister_device(state->dser_i2c);
#endif
#endif /* CONFIG_VIDEO_D5XX_SERDES */
#ifndef CONFIG_TEGRA_CAMERA_PLATFORM
	state->is_depth = 1;
#endif
	dev_info(&c->dev, "D4XX remove %s\n",
			d5x_get_sensor_name(state));
	if (state->vcc)
		regulator_disable(state->vcc);

	if (state->dfu_dev.dfu_state_flag != D5X_DFU_RECOVERY && \
		 state->mux.sd.subdev.v4l2_dev) {
#ifdef CONFIG_SYSFS
		sysfs_remove_group(&c->dev.kobj, &d5x_attr_group);
#endif
		d5x_mux_remove(state);
	}

	if (state->is_depth && state->dfu_dev.d5x_class) {
		d5x_chrdev_remove(state);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)
	return 0;
#endif
}

static const struct i2c_device_id d5x_id[] = {
	{ D5X_DRIVER_NAME, D5X_DS5U },
	{ D5X_DRIVER_NAME_ASR, D5X_ASR },
	{ D5X_DRIVER_NAME_AWG, D5X_AWG },
	{ },
};
MODULE_DEVICE_TABLE(i2c, d5x_id);

static const struct of_device_id d5xx_of_match[] = {
	{ .compatible = "realsense,d5xx", },
	{ },
};
MODULE_DEVICE_TABLE(of, d5xx_of_match);

static struct i2c_driver d5x_i2c_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(d5xx_of_match),
		.name = D5X_DRIVER_NAME
	},
	.probe		= d5x_probe,
	.remove		= d5x_remove,
	.id_table	= d5x_id,
};

module_i2c_driver(d5x_i2c_driver);

MODULE_DESCRIPTION("RealSense D5XX Camera Driver");
MODULE_AUTHOR("Guennadi Liakhovetski <guennadi.liakhovetski@intel.com>,\n\
				Nael Masalha <nael.masalha@intel.com>,\n\
				Alexander Gantman <alexander.gantman@intel.com>,\n\
				Emil Jahshan <emil.jahshan@intel.com>,\n\
				Xin Zhang <xin.x.zhang@intel.com>,\n\
				Qingwu Zhang <qingwu.zhang@intel.com>,\n\
				Evgeni Raikhel <evgeni.raikhel@intel.com>,\n\
				Shikun Ding <shikun.ding@intel.com>,\n\
				Dmitry Perchanov <dmitry.perchanov@intel.com>,\n\
				Pinquan Wang <pinquan.wang@realsenseai.com>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.3.9");

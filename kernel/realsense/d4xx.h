/* SPDX-License-Identifier: GPL-2.0 */
/*
 * d4xx.h - RealSense D4XX camera driver, internal definitions
 *
 * Shared by d4xx_main.c and d4xx_desc.c; not a UAPI or cross-module header.
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

#ifndef __D4XX_H__
#define __D4XX_H__

/* Only what the declarations below need; each .c includes what its code uses. */
#include <linux/atomic.h>	/* atomic_t in struct ds5_dev */
#include <linux/cdev.h>		/* struct cdev in struct ds5_dfu_dev */
#include <linux/mutex.h>
#include <linux/types.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

/* Held only as pointers below; the driver's own ones live in d4xx_main.c. */
struct class;
struct i2c_client;
struct regmap;
struct regulator;
struct dser_interface;
struct ser_interface;
struct ds5_variant;

#ifdef CONFIG_VIDEO_D4XX_SERDES
#include <media/max9295.h>
#include <media/max9296.h>
#include <media/max96712.h>
#include <media/max96717.h>
#include <media/max96724.h>
#else
#include <media/gmsl-link.h>
#define GMSL_CSI_DT_YUV422_8 0x1E
#define GMSL_CSI_DT_RGB_888 0x24
#define GMSL_CSI_DT_RAW_8 0x2A
#define GMSL_CSI_DT_EMBED 0x12
#endif

#include "d4xx_desc.h"

/* FW stream ids, and the format descriptor's stream_id wire values: this is
 * how d4xx_desc.c maps a parsed table onto a node.
 */
#define DS5_STREAM_DEPTH		0x0
#define DS5_STREAM_RGB			0x1
#define DS5_STREAM_IMU			0x2
#define DS5_STREAM_IR			0x4

enum ds5_mux_pad {
	DS5_MUX_PAD_EXTERNAL,
	DS5_MUX_PAD_DEPTH,
	DS5_MUX_PAD_RGB,
	DS5_MUX_PAD_IR,
	DS5_MUX_PAD_IMU,
	DS5_MUX_PAD_COUNT,
};

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
	DS5_DFU_IDLE = 0,
	DS5_DFU_RECOVERY,
	DS5_DFU_OPEN,
	DS5_DFU_IN_PROGRESS,
	DS5_DFU_DONE,
	DS5_DFU_ERROR
};

struct ds5_ctrls {
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl_handler handler_depth;
	struct v4l2_ctrl_handler handler_rgb;
	struct v4l2_ctrl_handler handler_y8;
	struct v4l2_ctrl_handler handler_imu;
	struct {
		struct v4l2_ctrl *log;
		struct v4l2_ctrl *fw_version;
		struct v4l2_ctrl *gvd;
		struct v4l2_ctrl *get_depth_calib;
		struct v4l2_ctrl *set_depth_calib;
		struct v4l2_ctrl *get_coeff_calib;
		struct v4l2_ctrl *set_coeff_calib;
		struct v4l2_ctrl *ae_roi_get;
		struct v4l2_ctrl *ae_roi_set;
		struct v4l2_ctrl *ae_setpoint_get;
		struct v4l2_ctrl *ae_setpoint_set;
		struct v4l2_ctrl *ae_mode;
		struct v4l2_ctrl *erb;
		struct v4l2_ctrl *ewb;
		struct v4l2_ctrl *hwmc;
		struct v4l2_ctrl *laser_power;
		struct v4l2_ctrl *manual_laser_power;
		struct v4l2_ctrl *auto_exp;
		struct v4l2_ctrl *exposure;
		/* in DS5 manual gain only works with manual exposure */
		struct v4l2_ctrl *gain;
		struct v4l2_ctrl *link_freq;
		struct v4l2_ctrl *query_sub_stream;
		struct v4l2_ctrl *set_sub_stream;
		struct v4l2_ctrl *sync_mode;
		struct v4l2_ctrl *device_mode;
		struct v4l2_ctrl *dual_rgb_ae_policy;
		struct v4l2_ctrl *passive_depth;
		struct v4l2_ctrl *minz;
		struct v4l2_ctrl *aligned_depth;
		/* RGB-only ISP controls. Only ae_priority needs a stored
		 * pointer because it must be disabled per-SKU after probe
		 * (D40X/D401 does not support it). The remaining controls
		 * are owned by the V4L2 handler and looked up by CID at dispatch.
		 */
		struct v4l2_ctrl *ae_priority;
	};
};

struct ds5_resolution {
	u16 width;
	u16 height;
	u8 n_framerates;
	const u16 *framerates;
};

struct ds5_format {
	unsigned int n_resolutions;
	const struct ds5_resolution *resolutions;
	u32 mbus_code;
	u8 data_type;		/* Source format DT programmed into HKR. */
	u8 override_data_type;	/* Non-zero CSI packet DT override on the wire. */
};

struct ds5_sensor {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt format;
	u16 mux_pad;
	struct {
		const struct ds5_format *format;
		const struct ds5_resolution *resolution;
		u16 framerate;
	} config;
	bool streaming;
	const struct ds5_format *formats;
	unsigned int n_formats;
	int pipe_id;
	u16 pipe_data_type1;
	u16 pipe_data_type2;
	u32 pipe_vc_id;
	u16 cached_dt_value;
	u16 cached_md_value;
	u16 cached_override_value;
	u16 cached_fps_value;
	u16 cached_width_value;
	u16 cached_height_value;
};

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
#include <media/camera_common.h>
#define ds5_mux_subdev camera_common_data
#else
struct ds5_mux_subdev {
	struct v4l2_subdev subdev;
};
#endif

struct ds5_dfu_dev {
	struct cdev ds5_cdev;
	struct class *ds5_class;
	int device_open_count;
	enum dfu_state dfu_state_flag;
	enum dfu_fw_state manifest_state;
	bool manifest_complete;
	unsigned char *dfu_msg;
	u16 msg_write_once;
	// unsigned char init_v4l_f; // need refactoring
	u32 bus_clk_rate;
};

struct ds5 {
	struct { struct ds5_sensor sensor; } depth;
	struct { struct ds5_sensor sensor; } ir;
	struct { struct ds5_sensor sensor; } rgb;
	struct { struct ds5_sensor sensor; } imu;
	struct {
		struct ds5_mux_subdev sd;
		struct media_pad pads[DS5_MUX_PAD_COUNT];
		struct ds5_sensor *last_set;
	} mux;
	struct ds5_ctrls ctrls;
	struct ds5_dfu_dev dfu_dev;
	bool power;
	struct i2c_client *client;
	/* All below pointers are used for writing, cannot be const */
	struct mutex lock;
	struct regmap *regmap;
	struct regulator *vcc;
	const struct ds5_variant *variant;
	int is_depth, is_y8, is_rgb, is_imu;
	bool metadata_enabled;
	int aggregated;
	int reset_ref_ds5;
	u16 fw_version;
	u16 fw_build;
	/* D58x camera behind a PIXEL-mode deserializer - requires strict
	 * Scheduling and data sizes in HKR side */
	bool d58x_pixel_mode;
	u16 control_base;
	u16 control_status_reg;
#ifdef CONFIG_VIDEO_D4XX_SERDES
	struct gmsl_link_ctx g_ctx;
	/* GMSL link this camera is wired to. From the serializer node's
	 * "maxim,gmsl-link-id"; no longer derivable from dst_vc once a
	 * deserializer remaps virtual channels. */
	u32 gmsl_link;
	struct device *ser_dev;
	struct device *dser_dev;
	struct i2c_client *ser_i2c;
	struct i2c_client *dser_i2c;
	const struct dser_interface *dser_ops;
	const struct ser_interface *ser_ops;
	bool ser_primary; /* true for the first instance per serializer (first stream of a specific camera) */
	bool dser_primary; /* true for the first instance per deserializer (first camera of a specific dser) */
#endif
	struct ds5_dev *ds5_dev; /* pointer to DS5 device struct */
};

struct ds5_dev {
	struct mutex lock;

	/*
	* Per-camera reset generation counter.
	* Incremented whenever the camera undergoes a HW reset,
	*  either from its own instance or a sibling's instance.
	* Each instance stores the last seen generation count
	*  and compares it on each access to detect if a reset has occurred
	*  and invalidates its own state if so.
	*/
	atomic_t reset_gen;

	/* Cached device type from post-reset device-type polling.
	* During probe the first instance resets the camera, causing DS5_DEVICE_TYPE
	* to temporarily return 0. The reset path polls until the register is valid
	* and stores the result here so that all four probe instances (and any
	* post-reset code path) can use the confirmed value even if the register
	* read still returns 0.
	*/
	u16 cached_device_type;
	u16 d585_product_id;

	/* Camera-resident format descriptor, parsed once per camera. Written
	 * under lock before any subdev registers; the tables it points at live
	 * until module exit, so stream-time readers need no lock.
	 */
	struct ds5_desc *desc;
	enum ds5_desc_state desc_state;

	/* Timestamp (jiffies) of last completed HW reset.
	* Used to enforce DS5_HW_RESET_COOLDOWN_MS between consecutive resets
	* and prevent GMSL link degradation from rapid reset cycles.
	*/
	unsigned long last_reset_jiffies;

#ifdef CONFIG_VIDEO_D4XX_SERDES
 	/* Pointer to the deserializer dev */
	struct dser_control *dser_control;

	/* Cold-bring-up flush request for this camera's GMSL link: the first
	 * stream on a link the deser already served for another camera wedges
	 * (VI discards every frame) until a one-shot. Guarded by lock. */
	bool link_flush_pending;
#endif

	/* Pointer to the primary DS5 struct */
	struct ds5 *ds5_primary;
	bool serdes_setup_complete;
	int configured_device_mode;
	int active_device_mode;
	bool device_mode_valid;

	bool depth_streaming;
	bool ir_streaming;
	bool rgb_streaming;
	bool imu_streaming;

	/* Per-role probe instances (indexed by DS5_MUX_PAD_*); the post-reset
	 * reload uses them to reach every node's channel. Guarded by lock.
	 */
	struct ds5 *role_inst[DS5_MUX_PAD_COUNT];
};

/* d4xx_main.c, used by d4xx_desc.c */
int ds5_raw_read(struct ds5 *state, u16 reg, void *val, size_t val_len);
/* Per-SKU built-in format tables: the fallback when the camera serves no
 * descriptor. Defined after the static tables it selects from.
 */
void ds5_apply_builtin_tables(struct ds5 *state, u16 dev_type);

#endif /* __D4XX_H__ */

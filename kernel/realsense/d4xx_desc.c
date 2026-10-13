// SPDX-License-Identifier: GPL-2.0
/*
 * d4xx_desc.c - RealSense D4XX camera-resident format descriptor
 *
 * Fetch, validate and parse the versioned format blob the camera serves over
 * I2C, and hand the resulting tables to the driver's sensor nodes.
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

#include <linux/crc32.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
#include <media/mc_common.h>	/* tegra_channel_mark_fmts_dirty() */
#endif

#include "d4xx.h"

/* Camera-resident format descriptor: a little-endian blob in a read-only
 * register aperture, read host-native like every other FW register here.
 * Every TOC entry carries its record stride, so a driver built against
 * older records strides past fields newer FW appends.
 */
#define RS_DESC_BASE            0x6000  /* FW: REG_BASE_MIPI_FORMAT_DESC (I2cSlaveHostIf.h) */
#define RS_DESC_MAX_SIZE        2048
#define RS_DESC_CHUNK           256
#define RS_DESC_MAGIC           0x53445352      /* "RSDS" */
#define RS_DESC_VER_MAJOR       1

struct rs_desc_header {
	u32 magic;
	u16 header_size;       /* offset of the TOC */
	u16 total_size;
	u8 ver_major;          /* bump = incompatible layout */
	u8 ver_minor;          /* bump = fields appended */
	u8 n_tables;
	u8 rsvd;
	u32 crc32;             /* CRC-32 (zlib) over [header_size, total_size) */
} __packed;

struct rs_desc_toc {
	u16 type;
	u16 entry_size;
	u16 n_entries;
	u16 offset;
} __packed;

enum rs_desc_table {
	RS_DESC_T_STREAM = 1,
	RS_DESC_T_FORMAT,
	RS_DESC_T_RESOLUTION,
	RS_DESC_T_FRAMERATE,    /* flat u16 pool */
	RS_DESC_T_MAX = RS_DESC_T_FRAMERATE,
};

struct rs_desc_stream {
	u16 stream_id;         /* DS5_STREAM_*: the FW's own ids */
	u16 fmt_first;         /* range into the FORMAT table */
	u16 fmt_count;
} __packed;

struct rs_desc_format {
	u16 pixfmt_id;         /* enum rs_pixfmt, never a Linux mbus code */
	u8 src_data_type;      /* CSI DT for this format - (wire DT unless using override) */
	u8 rsvd;               /* keeps every field naturally aligned */
	u16 res_first;         /* range into the RESOLUTION table */
	u16 res_count;
} __packed;

struct rs_desc_resolution {
	u16 width;
	u16 height;
	u16 fps_first;         /* range into the FRAMERATE pool */
	u16 fps_count;
} __packed;

/* Pixel-format registry shared with firmware. */
enum rs_pixfmt {
	RS_PIXFMT_Z16 = 1,
	RS_PIXFMT_Y8,
	RS_PIXFMT_Y8I,
	RS_PIXFMT_Y12I,
	RS_PIXFMT_Y16I,
	RS_PIXFMT_UYVY,
	RS_PIXFMT_NV12_FLAT,
	RS_PIXFMT_GRBG16,
	RS_PIXFMT_SBGGR10P,
	RS_PIXFMT_IMU,
	RS_PIXFMT_RSVL,
	RS_PIXFMT_H264,
	RS_PIXFMT_JPEG,
};

struct ds5_desc_stream {
	struct ds5_format *formats;
	unsigned int n_formats;
};

/* Wire stream ids are the FW's (depth 0, RGB 1, IMU 2, IR 4); slot 3 is unused. */
static const u8 ds5_desc_stream_ids[] = {
	DS5_STREAM_DEPTH, DS5_STREAM_RGB, DS5_STREAM_IMU, DS5_STREAM_IR,
};

#define DS5_DESC_STREAM_SLOTS	(DS5_STREAM_IR + 1)

struct ds5_desc {
	struct list_head node;
	struct ds5_desc_stream streams[DS5_DESC_STREAM_SLOTS];
	u16 *framerates;
	u32 crc;
	u8 ver_major;
	u8 ver_minor;
};

/* A sibling subdev can outlive the primary that owns the slot (sysfs unbind),
 * so parsed tables are never freed while the module is loaded.
 */
static LIST_HEAD(ds5_desc_all);
static DEFINE_MUTEX(ds5_desc_all_lock);

/* Static table mapping the RS pixel formats to the host side MBUS codes,
 * and if the FW DT cannot be serviced by SERDES/NVCSI, the wire DT for override
 * If wire_dt == 0, that means no override is needed, and the wire_dt will be
 * the DT exposed by FW.
 */
static const struct {
	u16 pixfmt;
	u32 mbus_code;
	u8 wire_dt;
} ds5_desc_pixfmt_map[] = {
	/* Z16 and Y8I are asked for with a user-defined FW token and sent as
	 * YUV422-8, which is all Tegra VI will carry at full rate.
	 */
	{ RS_PIXFMT_Z16,        MEDIA_BUS_FMT_UYVY8_1X16, GMSL_CSI_DT_YUV422_8 },
	{ RS_PIXFMT_Y8,         MEDIA_BUS_FMT_Y8_1X8 },
	{ RS_PIXFMT_Y8I,        MEDIA_BUS_FMT_VYUY8_1X16, GMSL_CSI_DT_YUV422_8 },
	{ RS_PIXFMT_Y12I,       MEDIA_BUS_FMT_RGB888_1X24 },
	{ RS_PIXFMT_Y16I,       MEDIA_BUS_FMT_ARGB8888_1X32 },
	{ RS_PIXFMT_UYVY,       MEDIA_BUS_FMT_YUYV8_1X16 },
	/* Flat NV12 bytes ride RAW8 as an opaque 8-bit carrier */
	{ RS_PIXFMT_NV12_FLAT,  MEDIA_BUS_FMT_RS_NV12_FLAT_1X8, GMSL_CSI_DT_RAW_8 },
	{ RS_PIXFMT_GRBG16,     MEDIA_BUS_FMT_SGRBG16_1X16 },
	/* D401 CSI passthrough: 10bit data riding an opeque 8-bit carrier. */
	{ RS_PIXFMT_SBGGR10P,   MEDIA_BUS_FMT_RS_SBGGR10P_1X8, GMSL_CSI_DT_RAW_8 },
	{ RS_PIXFMT_IMU,        MEDIA_BUS_FMT_Y8_1X8 },
	/* RSVL is the shared VC2 queue geometry, not a PD/OCC selector. */
#ifdef MEDIA_BUS_FMT_RS_VARLEN_1X8
	{ RS_PIXFMT_RSVL,       MEDIA_BUS_FMT_RS_VARLEN_1X8, GMSL_CSI_DT_RAW_8 },
#endif
	/* Compressed colour on the RAW8 carrier. */
	{ .pixfmt = RS_PIXFMT_H264, .mbus_code = MEDIA_BUS_FMT_RS_H264_1X8,
	  .wire_dt = GMSL_CSI_DT_RAW_8 },
	{ .pixfmt = RS_PIXFMT_JPEG, .mbus_code = MEDIA_BUS_FMT_RS_JPEG_1X8,
	  .wire_dt = GMSL_CSI_DT_RAW_8 },
};

/* Probed a word at a time: legacy FW loads one 16-bit word for an unmapped
 * register and wedges if the master clocks past it. Bare reads, because a
 * camera without a descriptor returns junk and retrying that is only log noise.
 */
static int ds5_desc_check_magic(struct ds5 *state)
{
	u16 word;
	unsigned int i;
	int ret;

	for (i = 0; i < sizeof(u32) / sizeof(word); i++) {
		ret = regmap_raw_read(state->regmap,
				      RS_DESC_BASE + i * sizeof(word),
				      &word, sizeof(word));
		if (ret < 0)
			return ret;
		if (word != ((RS_DESC_MAGIC >> (16 * i)) & 0xffff))
			return -ENODEV;
	}
	return 0;
}

static bool ds5_desc_stream_valid(u16 id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ds5_desc_stream_ids); i++)
		if (ds5_desc_stream_ids[i] == id)
			return true;
	return false;
}

static int ds5_desc_pixfmt_slot(u16 pixfmt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ds5_desc_pixfmt_map); i++)
		if (ds5_desc_pixfmt_map[i].pixfmt == pixfmt)
			return i;
	return -1;
}

static void ds5_desc_free(struct ds5_desc *desc)
{
	unsigned int s, f;

	if (!desc)
		return;
	for (s = 0; s < DS5_DESC_STREAM_SLOTS; s++) {
		for (f = 0; f < desc->streams[s].n_formats; f++)
			kfree(desc->streams[s].formats[f].resolutions);
		kfree(desc->streams[s].formats);
	}
	kfree(desc->framerates);
	kfree(desc);
}

/* The blob's stride may be shorter (older FW: missing fields read as 0)
 * or longer (newer FW: extra fields skipped) than the driver's record.
 */
static void ds5_desc_rec(const u8 *blob, const struct rs_desc_toc *toc,
			 unsigned int i, void *dst, size_t dst_size)
{
	size_t stride = toc->entry_size;

	memset(dst, 0, dst_size);
	memcpy(dst, blob + toc->offset + i * stride,
	       min(dst_size, stride));
}

static int ds5_desc_read_blob(struct ds5 *state, u8 **blob_out)
{
	struct rs_desc_header header;
	size_t header_size, total, off;
	u8 *blob;
	u32 crc;
	int ret;

	ret = ds5_desc_check_magic(state);
	if (ret < 0)
		return ret;
	ret = ds5_raw_read(state, RS_DESC_BASE, &header, sizeof(header));
	if (ret < 0)
		return ret;
	if (header.magic != RS_DESC_MAGIC)
		return -ENODEV;
	if (header.ver_major != RS_DESC_VER_MAJOR) {
		dev_warn(&state->client->dev,
			 "format descriptor v%u.%u unsupported (driver v%u)\n",
			 header.ver_major, header.ver_minor, RS_DESC_VER_MAJOR);
		return -EPROTONOSUPPORT;
	}
	header_size = header.header_size;
	total = header.total_size;
	if (header_size < sizeof(header) || total > RS_DESC_MAX_SIZE ||
	    total < header_size + header.n_tables * sizeof(struct rs_desc_toc)) {
		dev_warn(&state->client->dev,
			 "format descriptor: bad sizes (header %zu, total %zu, %u tables)\n",
			 header_size, total, header.n_tables);
		return -EINVAL;
	}

	blob = kzalloc(total, GFP_KERNEL);
	if (!blob)
		return -ENOMEM;
	memcpy(blob, &header, sizeof(header));
	for (off = sizeof(header); off < total; off += RS_DESC_CHUNK) {
		ret = ds5_raw_read(state, (u16)(RS_DESC_BASE + off), blob + off,
				   min_t(size_t, RS_DESC_CHUNK, total - off));
		if (ret < 0)
			goto err;
	}
	crc = crc32_le(~0, blob + header_size, total - header_size) ^ ~0;
	if (crc != header.crc32) {
		dev_warn(&state->client->dev,
			 "format descriptor CRC 0x%08x, expected 0x%08x\n",
			 crc, header.crc32);
		ret = -EBADMSG;
		goto err;
	}
	*blob_out = blob;
	return 0;
err:
	kfree(blob);
	return ret;
}

static int ds5_desc_find_tables(struct ds5 *state, const u8 *blob,
				const struct rs_desc_toc **tabs)
{
	const struct rs_desc_header *header = (const void *)blob;
	const struct rs_desc_toc *toc =
		(const void *)(blob + header->header_size);
	u32 total = header->total_size;
	unsigned int i;

	memset(tabs, 0, sizeof(*tabs) * (RS_DESC_T_MAX + 1));
	for (i = 0; i < header->n_tables; i++, toc++) {
		u32 type = toc->type;
		u32 stride = toc->entry_size;
		u32 n = toc->n_entries;
		u32 off = toc->offset;

		if (!stride || !n || off + n * stride > total) {
			dev_warn(&state->client->dev,
				 "format descriptor: toc[%u] type %u: %u x %u @ %u exceeds %u\n",
				 i, type, n, stride, off, total);
			return -EINVAL;
		}
		/* Table types this driver predates are skipped by contract. */
		if (type == 0 || type > RS_DESC_T_MAX)
			continue;
		if (tabs[type]) {
			dev_warn(&state->client->dev,
				 "format descriptor: duplicate table type %u\n", type);
			return -EINVAL;
		}
		tabs[type] = toc;
	}
	for (i = RS_DESC_T_STREAM; i <= RS_DESC_T_MAX; i++) {
		if (!tabs[i]) {
			dev_warn(&state->client->dev,
				 "format descriptor: table type %u missing\n", i);
			return -EINVAL;
		}
	}
	if (tabs[RS_DESC_T_FRAMERATE]->entry_size != sizeof(u16)) {
		dev_warn(&state->client->dev,
			 "format descriptor: framerate stride %u != 2\n",
			 tabs[RS_DESC_T_FRAMERATE]->entry_size);
		return -EINVAL;
	}
	return 0;
}

static int ds5_desc_parse_framerates(struct ds5 *state, struct ds5_desc *desc,
				     const u8 *blob,
				     const struct rs_desc_toc *toc,
				     unsigned int *n_out)
{
	unsigned int n = toc->n_entries, i;
	const u8 *src = blob + toc->offset;

	desc->framerates = kcalloc(n, sizeof(u16), GFP_KERNEL);
	if (!desc->framerates)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		u16 v;

		memcpy(&v, src + i * sizeof(v), sizeof(v));
		desc->framerates[i] = v;
		if (!desc->framerates[i]) {
			dev_warn(&state->client->dev, "format descriptor: framerate[%u] is 0\n", i);
			return -EINVAL;
		}
	}
	*n_out = n;
	return 0;
}

static struct ds5_resolution *
ds5_desc_build_resolutions(struct ds5 *state, const struct ds5_desc *desc,
			   unsigned int n_fps, const u8 *blob,
			   const struct rs_desc_toc *toc,
			   unsigned int first, unsigned int count)
{
	struct ds5_resolution *res;
	unsigned int i;

	if (!count || first + count > toc->n_entries) {
		dev_warn(&state->client->dev,
			 "format descriptor: resolution range %u+%u exceeds %u\n",
			 first, count, toc->n_entries);
		return ERR_PTR(-EINVAL);
	}
	res = kcalloc(count, sizeof(*res), GFP_KERNEL);
	if (!res)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < count; i++) {
		struct rs_desc_resolution rec;
		unsigned int fps_first;
		u16 width, height;

		ds5_desc_rec(blob, toc, first + i, &rec, sizeof(rec));
		width = rec.width;
		height = rec.height;
		fps_first = rec.fps_first;
		if (!width || !height || !rec.fps_count ||
		    rec.fps_count > U8_MAX ||
		    fps_first + rec.fps_count > n_fps) {
			dev_warn(&state->client->dev,
				 "format descriptor: resolution[%u] %ux%u fps %u+%u invalid (pool %u)\n",
				 first + i, width, height, fps_first,
				 rec.fps_count, n_fps);
			kfree(res);
			return ERR_PTR(-EINVAL);
		}
		res[i].width = width;
		res[i].height = height;
		res[i].n_framerates = rec.fps_count;
		res[i].framerates = &desc->framerates[fps_first];
		dev_dbg(&state->client->dev,
			"format descriptor:   res[%u] %ux%u, %u fps from %u (first %u)\n",
			first + i, width, height, rec.fps_count, fps_first,
			desc->framerates[fps_first]);
	}
	return res;
}

static int ds5_desc_build_stream(struct ds5 *state, struct ds5_desc *desc,
				 unsigned int n_fps, const u8 *blob,
				 const struct rs_desc_toc **tabs,
				 const struct rs_desc_stream *st)
{
	const struct rs_desc_toc *ftoc = tabs[RS_DESC_T_FORMAT];
	struct ds5_desc_stream *out;
	unsigned int first = st->fmt_first;
	unsigned int count = st->fmt_count;
	unsigned int i;

	if (!ds5_desc_stream_valid(st->stream_id)) {
		dev_warn(&state->client->dev,
			 "format descriptor: stream id %u unknown\n", st->stream_id);
		return -EINVAL;
	}
	out = &desc->streams[st->stream_id];
	if (out->formats || !count ||
	    first + count > ftoc->n_entries) {
		dev_warn(&state->client->dev,
			 "format descriptor: stream %u: format range %u+%u exceeds %u%s\n",
			 st->stream_id, first, count, ftoc->n_entries,
			 out->formats ? " (duplicate stream)" : "");
		return -EINVAL;
	}
	out->formats = kcalloc(count, sizeof(*out->formats), GFP_KERNEL);
	if (!out->formats)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct rs_desc_format rec;
		struct ds5_resolution *res;
		struct ds5_format *fmt;
		int slot;

		ds5_desc_rec(blob, ftoc, first + i, &rec, sizeof(rec));
		/* DT 0 is illegal; rsvd must-be-zero.
		 * Newer fields, if any, arrive via a longer entry_size.
		 */
		if (!rec.src_data_type || rec.rsvd) {
			dev_warn(&state->client->dev,
				 "format descriptor: format[%u] dt 0x%02x rsvd 0x%02x invalid\n",
				 first + i, rec.src_data_type, rec.rsvd);
			return -EINVAL;
		}
		slot = ds5_desc_pixfmt_slot(rec.pixfmt_id);
		if (slot < 0) {
			dev_warn(&state->client->dev,
				 "format descriptor: pixfmt %u unknown, skipped\n",
				 rec.pixfmt_id);
			continue;
		}
		dev_dbg(&state->client->dev,
			"format descriptor: stream %u fmt[%u] pixfmt %u dt 0x%02x override 0x%02x, %u res\n",
			st->stream_id, i, rec.pixfmt_id, rec.src_data_type,
			ds5_desc_pixfmt_map[slot].wire_dt, rec.res_count);
		res = ds5_desc_build_resolutions(state, desc, n_fps, blob,
						 tabs[RS_DESC_T_RESOLUTION],
						 rec.res_first,
						 rec.res_count);
		if (IS_ERR(res))
			return PTR_ERR(res);

		fmt = &out->formats[out->n_formats];
		fmt->mbus_code = ds5_desc_pixfmt_map[slot].mbus_code;
		fmt->data_type = rec.src_data_type;
		fmt->override_data_type = ds5_desc_pixfmt_map[slot].wire_dt;
		fmt->resolutions = res;
		fmt->n_resolutions = rec.res_count;
		out->n_formats++;
	}
	return out->n_formats ? 0 : -EINVAL;
}

/* Anything transient - a CRC miss, a glitched transfer - is worth one retry.
 * Latching on the first failure would drop the camera to the static tables
 * for the life of the module.
 */
static int ds5_desc_fetch(struct ds5 *state, u8 **blob)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < 2; attempt++) {
		ret = ds5_desc_read_blob(state, blob);
		/* A verdict about the descriptor itself: a reread cannot change it. */
		if (!ret || ret == -ENODEV || ret == -EPROTONOSUPPORT ||
		    ret == -EINVAL || ret == -ENOMEM)
			break;
	}
	return ret;
}

/* Parse a fetched, CRC-checked blob. Returns ERR_PTR on any rejection,
 * having freed whatever it had built.
 */
static struct ds5_desc *ds5_desc_build(struct ds5 *state, const u8 *blob)
{
	const struct rs_desc_header *header = (const void *)blob;
	const struct rs_desc_toc *tabs[RS_DESC_T_MAX + 1];
	unsigned int n_fps = 0, i;
	struct ds5_desc *desc;
	int ret;

	ret = ds5_desc_find_tables(state, blob, tabs);
	if (ret)
		return ERR_PTR(ret);
	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);
	desc->crc = header->crc32;
	desc->ver_major = header->ver_major;
	desc->ver_minor = header->ver_minor;

	ret = ds5_desc_parse_framerates(state, desc, blob,
					tabs[RS_DESC_T_FRAMERATE],
					&n_fps);
	if (ret)
		goto err;
	for (i = 0; i < tabs[RS_DESC_T_STREAM]->n_entries; i++) {
		struct rs_desc_stream st;

		ds5_desc_rec(blob, tabs[RS_DESC_T_STREAM], i, &st, sizeof(st));
		ret = ds5_desc_build_stream(state, desc, n_fps, blob, tabs, &st);
		if (ret)
			goto err;
	}
	/* Every subdev registers unconditionally, so each needs a format list. */
	for (i = 0; i < ARRAY_SIZE(ds5_desc_stream_ids); i++) {
		if (!desc->streams[ds5_desc_stream_ids[i]].n_formats) {
			dev_warn(&state->client->dev, "format descriptor: stream %u missing\n",
				 ds5_desc_stream_ids[i]);
			ret = -EINVAL;
			goto err;
		}
	}
	return desc;
err:
	ds5_desc_free(desc);
	return ERR_PTR(ret);
}

/* Tables are never freed once published, so publishing is the last step.
 * A rebind reparses the same blob, so an identical set is reused rather than
 * stranding the previous copy on the list for the life of the module.
 */
static void ds5_desc_publish(struct ds5 *state, struct ds5_desc *desc)
{
	struct ds5_dev *dd = state->ds5_dev;
	struct ds5_desc *pub = NULL, *d;

	mutex_lock(&ds5_desc_all_lock);
	list_for_each_entry(d, &ds5_desc_all, node) {
		if (d->crc == desc->crc && d->ver_major == desc->ver_major &&
		    d->ver_minor == desc->ver_minor) {
			pub = d;
			break;
		}
	}
	if (!pub) {
		list_add(&desc->node, &ds5_desc_all);
		pub = desc;
		desc = NULL;
	}
	mutex_unlock(&ds5_desc_all_lock);
	ds5_desc_free(desc);
	dd->desc = pub;
	dd->desc_state = DS5_DESC_READY;
	dev_info(&state->client->dev,
		 "format descriptor v%u.%u crc 0x%08x: %u/%u/%u/%u formats (depth/ir/rgb/imu)\n",
		 pub->ver_major, pub->ver_minor, pub->crc,
		 pub->streams[DS5_STREAM_DEPTH].n_formats,
		 pub->streams[DS5_STREAM_IR].n_formats,
		 pub->streams[DS5_STREAM_RGB].n_formats,
		 pub->streams[DS5_STREAM_IMU].n_formats);
}

/* Caller holds ds5_dev->lock. */
static int ds5_desc_load_locked(struct ds5 *state)
{
	struct ds5_desc *desc = NULL;
	u8 *blob = NULL;
	int ret;

	ret = ds5_desc_fetch(state, &blob);
	if (!ret) {
		desc = ds5_desc_build(state, blob);
		if (IS_ERR(desc)) {
			ret = PTR_ERR(desc);
			desc = NULL;
		}
	}
	kfree(blob);
	if (ret) {
		state->ds5_dev->desc_state = DS5_DESC_ABSENT;
		if (ret == -ENODEV)
			dev_warn(&state->client->dev,
				 "format descriptors not provided by camera, using built-in tables\n");
		else
			dev_warn(&state->client->dev,
				 "format descriptor rejected (%d), using built-in tables\n",
				 ret);
		return ret;
	}
	ds5_desc_publish(state, desc);
	return 0;
}

static void ds5_desc_apply_sensor(struct ds5_sensor *sensor,
				  const struct ds5_desc_stream *st, u16 mux_pad)
{
	if (!sensor || !st)
		return;

	sensor->formats = st->formats;
	sensor->n_formats = st->n_formats;
	sensor->mux_pad = mux_pad;
}

/* True when the camera's descriptor supplied every sensor's format table.
 * Caller holds ds5_dev->lock.
 */
static bool __ds5_desc_apply(struct ds5 *state)
{
	struct ds5_dev *dd = state->ds5_dev;
	const struct ds5_desc *desc = NULL;

	if (dd->desc_state == DS5_DESC_UNKNOWN)
		ds5_desc_load_locked(state);
	if (dd->desc_state == DS5_DESC_READY)
		desc = dd->desc;
	if (!desc)
		return false;

	ds5_desc_apply_sensor(&state->depth.sensor,
			      &desc->streams[DS5_STREAM_DEPTH],
			      DS5_MUX_PAD_DEPTH);
	ds5_desc_apply_sensor(&state->ir.sensor,
			      &desc->streams[DS5_STREAM_IR], DS5_MUX_PAD_IR);
	ds5_desc_apply_sensor(&state->rgb.sensor,
			      &desc->streams[DS5_STREAM_RGB], DS5_MUX_PAD_RGB);
	ds5_desc_apply_sensor(&state->imu.sensor,
			      &desc->streams[DS5_STREAM_IMU], DS5_MUX_PAD_IMU);
	return true;
}

bool ds5_desc_apply(struct ds5 *state)
{
	struct ds5_dev *dd = state->ds5_dev;
	bool ret;

	mutex_lock(&dd->lock);
	ret = __ds5_desc_apply(state);
	mutex_unlock(&dd->lock);
	return ret;
}

#ifdef CONFIG_TEGRA_CAMERA_PLATFORM
/* Mark this node's VI format list stale (lock-free) so the VI rebuilds it on the
 * next format ioctl. A direct refresh would deadlock - the reload runs in the
 * reset node's s_ctrl under the video_lock the rebuild needs.
 */
static void ds5_mark_channel_fmts_dirty(struct ds5 *inst)
{
	struct v4l2_subdev *sd = &inst->mux.sd.subdev;
	struct tegra_channel *chan = v4l2_get_subdev_hostdata(sd);

	if (chan)
		tegra_channel_mark_fmts_dirty(chan);
	else
		dev_warn(&inst->client->dev, "%s(): %s has no VI channel\n",
			 __func__, sd->name);
}
#else
static inline void ds5_mark_channel_fmts_dirty(struct ds5 *inst) { }
#endif

/* After a HW reset re-fetch the descriptor and re-apply every node's table (the
 * new one, or built-in per-SKU tables if it is gone), then mark each channel
 * stale. No-op when unchanged. @dev_type is what recovery just validated.
 */
void ds5_desc_reload_after_reset(struct ds5 *state, u16 dev_type)
{
	struct ds5_dev *dd = state->ds5_dev;
	struct ds5_desc *prev_desc;
	enum ds5_desc_state prev_state;
	struct rs_desc_header header;
	bool present, changed, fw_changed, have_desc;
	unsigned int pad;
	u32 crc = 0;
	int ret;

	ret = ds5_desc_check_magic(state);
	if (ret < 0 && ret != -ENODEV)
		return;			/* transient I2C error: leave tables as-is */
	present = !ret;
	if (present) {
		if (ds5_raw_read(state, RS_DESC_BASE, &header, sizeof(header)))
			return;
		crc = header.crc32;
	}

	mutex_lock(&dd->lock);
	if (dd->desc_state == DS5_DESC_READY)
		changed = !present || crc != dd->desc->crc;
	else
		changed = present;	/* on static tables; a descriptor appeared */
	/* A DFU can also change fw_version-gated built-in tables (IMU across
	 * 5.16) with no descriptor involved; siblings hold the pre-reset value.
	 */
	fw_changed = false;
	for (pad = DS5_MUX_PAD_DEPTH; pad < DS5_MUX_PAD_COUNT; pad++)
		if (dd->role_inst[pad] &&
		    (dd->role_inst[pad]->fw_version != state->fw_version ||
		     dd->role_inst[pad]->fw_build != state->fw_build))
			fw_changed = true;
	if (!changed && !fw_changed) {
		mutex_unlock(&dd->lock);
		return;			/* common case: FW unchanged across reset */
	}
	if (changed) {
		/* Force a fresh fetch+parse. Publish CRC-dedups on the
		 * module-lifetime list, so an unchanged blob costs nothing and
		 * a changed one adds one table (the old is never freed - a
		 * sibling subdev may still use it).
		 */
		prev_desc = dd->desc;
		prev_state = dd->desc_state;
		dd->desc_state = DS5_DESC_UNKNOWN;
		ret = ds5_desc_load_locked(state);
		if (ret && ret != -ENODEV) {
			/* Transient fetch/parse failure, not "no descriptor"
			 * (the magic matched above): keep the previous tables.
			 */
			dd->desc = prev_desc;
			dd->desc_state = prev_state;
			mutex_unlock(&dd->lock);
			return;
		}
	}
	have_desc = dd->desc_state == DS5_DESC_READY;

	/* Applies are pointer swaps and the mark is a lock-free flag, so the
	 * loop stays under dd->lock: ds5_remove() clears role_inst[] under the
	 * same lock before its instance is freed, so no slot can go stale here.
	 */
	for (pad = DS5_MUX_PAD_DEPTH; pad < DS5_MUX_PAD_COUNT; pad++) {
		struct ds5 *inst = dd->role_inst[pad];

		if (!inst)
			continue;
		/* Share the post-reset FW version/build; only @state re-read it. */
		inst->fw_version = state->fw_version;
		inst->fw_build = state->fw_build;
		if (!have_desc || !__ds5_desc_apply(inst))
			ds5_apply_builtin_tables(inst, dev_type);
		ds5_mark_channel_fmts_dirty(inst);
	}
	mutex_unlock(&dd->lock);

	if (have_desc)
		dev_info(&state->client->dev,
			 "descriptor tables re-applied after HW reset (crc 0x%08x); nodes refreshed\n",
			 crc);
	else
		dev_info(&state->client->dev,
			 "built-in tables re-applied after HW reset (fw %x); nodes refreshed\n",
			 state->fw_version);
}

/* Module exit: the parsed tables outlive every instance, so they are freed
 * only once nothing can reference them.
 */
void ds5_desc_free_all(void)
{
	struct ds5_desc *desc, *tmp;

	list_for_each_entry_safe(desc, tmp, &ds5_desc_all, node)
		ds5_desc_free(desc);
}

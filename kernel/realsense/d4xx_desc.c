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

#include <linux/bitops.h>	/* hweight8(): constant-param runs */
#include <linux/crc32.h>
#include <linux/ctype.h>	/* isprint(): published names and labels */
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
#define RS_DESC_MAX_SIZE        8192    /* control tables: ~3.2 KB on a D585 */
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
	RS_DESC_T_LAST_REQUIRED = RS_DESC_T_FRAMERATE,

	/* Control tables: optional as a group, numbered in reference order -
	 * a stream names controls, a control names its bounds, labels and
	 * wire values.
	 */
	RS_DESC_T_CTRL_STREAM_MAP,      /* flat u16 pool: control indices, one slice per stream */
	RS_DESC_T_CONTROL,
	RS_DESC_T_CTRL_BOUNDS,
	RS_DESC_T_CTRL_STRING,          /* flat NUL-separated pool: names and menu labels */
	RS_DESC_T_CTRL_MENU_WIRE_MAP,   /* flat u16 pool: menu index -> firmware value */
	RS_DESC_T_CTRL_HWMC_CONST,      /* flat u16 pool: constant command params */
	RS_DESC_T_MAX = RS_DESC_T_CTRL_HWMC_CONST,
};

/* 6 B in v1.0, 14 B with controls; ds5_desc_rec() zero-fills what an older
 * blob lacks, so a v1.0 stream reads ctrl_count 0.
 */
struct rs_desc_stream {
	u16 stream_id;         /* DS5_STREAM_*: the FW's own ids */
	u16 fmt_first;         /* range into the FORMAT table */
	u16 fmt_count;
	u16 ctrl_first;        /* slice of CTRL_STREAM_MAP */
	u16 ctrl_count;        /* 0 = this stream advertises no controls */
	u16 ctrl_base;         /* base of this stream's control block */
	u16 status_addr;       /* result register for REG writes into that block; 0 = none */
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

/* Control records. mech is a union discriminated by transport and fixed at
 * 8 B so the record stays 26 B; a parser reads only the arm transport names.
 */
#define RS_DESC_NONE            0xFFFF  /* absent, for every optional index or offset */
#define RS_CTRL_NAME_MAX        32      /* bytes incl. NUL: V4L2's name field */
#define RS_CTRL_MENU_MAX        16      /* menu_mask is a u16 indexed by menu index */

struct rs_desc_mech_reg {
	u16 addr;              /* absolute, or an offset into the stream's ctrl_base */
	u16 extent;            /* bytes transferred; a scalar's width */
	u32 rsvd;              /* holds the union at 8 B */
} __packed;

struct rs_desc_mech_hwmc {
	u8 opcode_set;         /* 0xFF = not settable */
	u8 opcode_get;         /* 0xFF = not gettable */
	u8 value_param;        /* 0 = value rides in Data; else first of param1..4 */
	u8 const_mask;         /* bit i = param(i+1) is a constant from the pool */
	u16 const_offset;      /* into CTRL_HWMC_CONST, hweight(const_mask) values in slot order */
	u16 resp_bytes;        /* 0 = from DS5_HWMC_RESP_LEN at read time */
} __packed;

struct rs_desc_control {
	u16 control_id;        /* private CID offset, or standard-map index with F_STANDARD */
	u8 v4l2_type;          /* enum rs_ctrl_type */
	u8 transport;          /* enum rs_ctrl_transport */
	u16 flags;             /* RS_CTRL_F_* */
	u16 elems;             /* array length; 1 = scalar */
	u16 menu_mask;         /* bit i set = menu index i absent */
	u16 bounds;            /* index into CTRL_BOUNDS; RS_DESC_NONE = the type's natural range */
	u16 name_offset;       /* into CTRL_STRING; RS_DESC_NONE with F_STANDARD */
	u16 menu_offset;       /* into CTRL_STRING, max + 1 labels; RS_DESC_NONE with F_STANDARD */
	/* into CTRL_MENU_WIRE_MAP, max + 1 values; RS_DESC_NONE = the index
	 * is the value
	 */
	u16 menu_wire_map_offset;
	union {
		struct rs_desc_mech_reg reg;
		struct rs_desc_mech_hwmc hwmc;
	} mech;
} __packed;

struct rs_desc_ctrl_bounds {
	s32 min, max, step, def;
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
	struct ds5_desc_ctrl *ctrls;    /* resolved for this stream; NULL = none published */
	unsigned int n_ctrls;
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
	/* Control pools the records point into, copied out of the blob: the
	 * V4L2 core stores the name and qmenu pointers, so they must outlive
	 * the fetch buffer.
	 */
	char *strings;
	u16 *wire_map;
	u16 *hwmc_const;
	const char **qmenus;    /* every private menu's label pointers, back to back */
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
		kfree(desc->streams[s].ctrls);
	}
	kfree(desc->framerates);
	kfree(desc->strings);
	kfree(desc->wire_map);
	kfree(desc->hwmc_const);
	kfree(desc->qmenus);
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
	/* Only the format tables are required; a v1.0 blob has nothing above. */
	for (i = RS_DESC_T_STREAM; i <= RS_DESC_T_LAST_REQUIRED; i++) {
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

/* The control set is served whole or not at all; only the wire map, which
 * none of today's private menus needs, may be absent within it. The pools
 * are byte- or u16-addressed, so their strides are not negotiable.
 * Returns -ENODATA for no control set, -EINVAL for a broken one.
 */
static int ds5_desc_check_ctrl_tables(struct ds5 *state,
				      const struct rs_desc_toc **tabs)
{
	static const struct {
		u8 type;
		u8 stride;
		const char *name;
	} pools[] = {
		{ RS_DESC_T_CTRL_STREAM_MAP,    2, "stream map" },
		{ RS_DESC_T_CTRL_STRING,        1, "string" },
		{ RS_DESC_T_CTRL_MENU_WIRE_MAP, 2, "menu wire map" },
		{ RS_DESC_T_CTRL_HWMC_CONST,    2, "hwmc constant" },
	};
	static const u8 required[] = {
		RS_DESC_T_CTRL_STREAM_MAP, RS_DESC_T_CONTROL,
		RS_DESC_T_CTRL_BOUNDS, RS_DESC_T_CTRL_STRING,
	};
	unsigned int i, present = 0;

	for (i = 0; i < ARRAY_SIZE(required); i++)
		present += !!tabs[required[i]];
	if (!present) {
		if (tabs[RS_DESC_T_CTRL_MENU_WIRE_MAP] ||
		    tabs[RS_DESC_T_CTRL_HWMC_CONST]) {
			dev_warn(&state->client->dev,
				 "control descriptor: a control pool without a control set\n");
			return -EINVAL;
		}
		return -ENODATA;
	}
	if (present != ARRAY_SIZE(required)) {
		dev_warn(&state->client->dev,
			 "control descriptor: %u of %zu control tables present\n",
			 present, ARRAY_SIZE(required));
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		const struct rs_desc_toc *toc = tabs[pools[i].type];

		if (toc && toc->entry_size != pools[i].stride) {
			dev_warn(&state->client->dev,
				 "control descriptor: %s stride %u != %u\n",
				 pools[i].name, toc->entry_size, pools[i].stride);
			return -EINVAL;
		}
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

/* --- Control tables --------------------------------------------------- */

#define RS_HWMC_HDR_SIZE	20	/* struct hwm_cmd ahead of Data */

/* Standard-CID registry: a camera names a V4L2-defined control by index here
 * and inherits Linux's name, labels and index numbering. Append-only. The
 * type is what the record must declare: with .name always supplied,
 * v4l2_ctrl_fill() never runs and nothing downstream corrects a mismatch.
 */
static const struct {
	u32 cid;
	u8 type;		/* enum rs_ctrl_type */
} ds5_desc_std_cids[] = {
	[0]  = { V4L2_CID_EXPOSURE_ABSOLUTE,         RS_CTRL_T_INTEGER },
	[1]  = { V4L2_CID_EXPOSURE_AUTO,             RS_CTRL_T_MENU },
	[2]  = { V4L2_CID_ANALOGUE_GAIN,             RS_CTRL_T_INTEGER },
	[3]  = { V4L2_CID_SATURATION,                RS_CTRL_T_INTEGER },
	[4]  = { V4L2_CID_SHARPNESS,                 RS_CTRL_T_INTEGER },
	[5]  = { V4L2_CID_WHITE_BALANCE_TEMPERATURE, RS_CTRL_T_INTEGER },
	[6]  = { V4L2_CID_AUTO_WHITE_BALANCE,        RS_CTRL_T_BOOLEAN },
	[7]  = { V4L2_CID_POWER_LINE_FREQUENCY,      RS_CTRL_T_MENU },
	[8]  = { V4L2_CID_EXPOSURE_AUTO_PRIORITY,    RS_CTRL_T_BOOLEAN },
	[9]  = { V4L2_CID_BRIGHTNESS,                RS_CTRL_T_INTEGER },
	[10] = { V4L2_CID_CONTRAST,                  RS_CTRL_T_INTEGER },
	[11] = { V4L2_CID_GAMMA,                     RS_CTRL_T_INTEGER },
	[12] = { V4L2_CID_HUE,                       RS_CTRL_T_INTEGER },
};

/* rs_ctrl_type -> V4L2 type and element size. */
static const struct {
	u32 type;
	u32 elem_size;
} ds5_desc_ctrl_types[] = {
	[RS_CTRL_T_INTEGER]   = { V4L2_CTRL_TYPE_INTEGER, sizeof(s32) },
	[RS_CTRL_T_BOOLEAN]   = { V4L2_CTRL_TYPE_BOOLEAN, sizeof(s32) },
	[RS_CTRL_T_MENU]      = { V4L2_CTRL_TYPE_MENU,    sizeof(s32) },
	[RS_CTRL_T_BUTTON]    = { V4L2_CTRL_TYPE_BUTTON,  sizeof(s32) },
	[RS_CTRL_T_U8_ARRAY]  = { V4L2_CTRL_TYPE_U8,      sizeof(u8) },
	[RS_CTRL_T_U32_ARRAY] = { V4L2_CTRL_TYPE_U32,     sizeof(u32) },
};

/* An HWMC opcode whose host sequence the driver owns is accepted only under
 * the control_id its hook is bound to. Nothing else is filtered: an opcode
 * the driver has never seen registers and works.
 */
static const struct {
	u8 opcode;
	u16 control_id;
} ds5_desc_wrapped_opcodes[] = {
	{ 0x20, DS5_CAMERA_CID_HW_RESET - DS5_CAMERA_CID_BASE },
	{ 0x17, DS5_CAMERA_CID_ERB - DS5_CAMERA_CID_BASE },
	{ 0x18, DS5_CAMERA_CID_EWB - DS5_CAMERA_CID_BASE },
};

struct ds5_desc_ctrl_ctx {
	struct ds5 *state;
	struct ds5_desc *desc;
	const u8 *blob;
	const struct rs_desc_toc *map, *ctrl, *bounds, *strings, *wire, *hconst;
	unsigned int n_strings;
	unsigned int n_wire;
	unsigned int n_hconst;
	unsigned int qmenu_used;	/* pointers handed out from desc->qmenus */
	unsigned int qmenu_cap;
};

#define ctrl_warn(ctx, idx, fmt, ...)					\
	dev_warn(&(ctx)->state->client->dev,				\
		 "control descriptor: control[%u]: " fmt, idx, ##__VA_ARGS__)

static void ds5_desc_drop_controls(struct ds5_desc *desc)
{
	unsigned int s;

	for (s = 0; s < DS5_DESC_STREAM_SLOTS; s++) {
		kfree(desc->streams[s].ctrls);
		desc->streams[s].ctrls = NULL;
		desc->streams[s].n_ctrls = 0;
	}
	kfree(desc->strings);
	kfree(desc->wire_map);
	kfree(desc->hwmc_const);
	kfree(desc->qmenus);
	desc->strings = NULL;
	desc->wire_map = NULL;
	desc->hwmc_const = NULL;
	desc->qmenus = NULL;
}

/* A terminated, printable string of at most @max_len characters at @off in
 * the copied pool, or NULL.
 */
static const char *ds5_desc_string(const struct ds5_desc_ctrl_ctx *ctx,
				   unsigned int off, size_t max_len)
{
	const char *s = ctx->desc->strings + off;
	size_t n;

	if (off >= ctx->n_strings)
		return NULL;
	n = strnlen(s, min_t(size_t, max_len, ctx->n_strings - off));
	if (!n || off + n >= ctx->n_strings || s[n] != '\0')
		return NULL;
	while (n--)
		if (!isprint(s[n]))
			return NULL;
	return s;
}

/* The element type's own range, for a record that publishes no bounds row.
 * check_range() rejects step 0 for every type it tests, so a zeroed config
 * registers nothing; an integer or a menu has no natural range to substitute.
 */
static bool ds5_desc_natural_range(u8 type, struct ds5_desc_ctrl *c)
{
	switch (type) {
	case RS_CTRL_T_BOOLEAN:
		c->min = 0; c->max = 1; c->step = 1; c->def = 0;
		return true;
	case RS_CTRL_T_U8_ARRAY:
		c->min = 0; c->max = U8_MAX; c->step = 1; c->def = 0;
		return true;
	case RS_CTRL_T_U32_ARRAY:
		c->min = 0; c->max = U32_MAX; c->step = 1; c->def = 0;
		return true;
	case RS_CTRL_T_BUTTON:
		c->min = 0; c->max = 0; c->step = 0; c->def = 0;
		return true;
	default:
		return false;
	}
}

/* Validate one published control for the stream that advertises it and
 * resolve it into @out. -EINVAL skips the record. The checks are the parser
 * rules of docs/camera-control-descriptor.md §12.2, in their numbering.
 */
static int ds5_desc_build_ctrl(struct ds5_desc_ctrl_ctx *ctx,
			       const struct rs_desc_stream *st,
			       const struct rs_desc_control *rec,
			       unsigned int idx,
			       const struct ds5_desc_ctrl *done,
			       unsigned int n_done,
			       struct ds5_desc_ctrl *out)
{
	struct ds5_desc_ctrl c = { 0 };
	bool is_std = rec->flags & RS_CTRL_F_STANDARD;
	bool is_menu = rec->v4l2_type == RS_CTRL_T_MENU;
	unsigned int i;

	/* rule 6: enumerated fields the driver does not know */
	if (rec->transport != RS_CTRL_XPORT_REG &&
	    rec->transport != RS_CTRL_XPORT_HWMC) {
		ctrl_warn(ctx, idx, "transport %u unknown\n", rec->transport);
		return -EINVAL;
	}
	if (rec->v4l2_type < RS_CTRL_T_INTEGER ||
	    rec->v4l2_type > RS_CTRL_T_U32_ARRAY) {
		ctrl_warn(ctx, idx, "type %u unknown\n", rec->v4l2_type);
		return -EINVAL;
	}
	if (!rec->elems) {
		ctrl_warn(ctx, idx, "elems 0\n");
		return -EINVAL;
	}
	c.type = ds5_desc_ctrl_types[rec->v4l2_type].type;
	c.elem_size = ds5_desc_ctrl_types[rec->v4l2_type].elem_size;
	c.elems = rec->elems;
	c.flags = rec->flags;
	c.transport = rec->transport;

	/* Identity: a standard CID is Linux's, a private one is ours. */
	if (is_std) {
		if (rec->control_id >= ARRAY_SIZE(ds5_desc_std_cids)) {
			ctrl_warn(ctx, idx, "standard index %u unknown\n",
				  rec->control_id);
			return -EINVAL;
		}
		if (rec->v4l2_type != ds5_desc_std_cids[rec->control_id].type) {
			ctrl_warn(ctx, idx, "standard index %u declares type %u, Linux's is %u\n",
				  rec->control_id, rec->v4l2_type,
				  ds5_desc_std_cids[rec->control_id].type);
			return -EINVAL;
		}
		/* rule 13: the name and labels are Linux's */
		if (rec->name_offset != RS_DESC_NONE ||
		    rec->menu_offset != RS_DESC_NONE) {
			ctrl_warn(ctx, idx, "standard control publishes a name or labels\n");
			return -EINVAL;
		}
		if (rec->transport == RS_CTRL_XPORT_HWMC) {
			ctrl_warn(ctx, idx, "standard control over HWMC\n");
			return -EINVAL;
		}
		c.cid = ds5_desc_std_cids[rec->control_id].cid;
		c.name = v4l2_ctrl_get_name(c.cid);
		if (!c.name)
			return -EINVAL;
	} else {
		c.cid = DS5_CAMERA_CID_BASE + rec->control_id;
		/* rule 4 */
		if (V4L2_CTRL_ID2WHICH(c.cid) != V4L2_CTRL_CLASS_CAMERA) {
			ctrl_warn(ctx, idx, "id %u leaves the camera class\n",
				  rec->control_id);
			return -EINVAL;
		}
		/* rule 10: the core has no name or labels to fall back on */
		if (rec->name_offset == RS_DESC_NONE ||
		    (is_menu && rec->menu_offset == RS_DESC_NONE)) {
			ctrl_warn(ctx, idx, "private control without a name or labels\n");
			return -EINVAL;
		}
		c.name = ds5_desc_string(ctx, rec->name_offset,
					 RS_CTRL_NAME_MAX - 1);
		if (!c.name) {
			ctrl_warn(ctx, idx, "name at %u invalid\n",
				  rec->name_offset);
			return -EINVAL;
		}
	}

	/* Bounds: published, or the type's natural range (rule 11). */
	if (rec->bounds != RS_DESC_NONE) {
		struct rs_desc_ctrl_bounds b;

		if (rec->bounds >= ctx->bounds->n_entries) {	/* rule 5 */
			ctrl_warn(ctx, idx, "bounds %u exceeds %u\n",
				  rec->bounds, ctx->bounds->n_entries);
			return -EINVAL;
		}
		ds5_desc_rec(ctx->blob, ctx->bounds, rec->bounds, &b, sizeof(b));
		c.min = b.min;
		c.max = b.max;
		c.step = b.step;
		c.def = b.def;
	} else if (!ds5_desc_natural_range(rec->v4l2_type, &c)) {
		ctrl_warn(ctx, idx, "type %u without bounds\n", rec->v4l2_type);
		return -EINVAL;
	}

	if (is_menu) {
		unsigned int n_entries;

		/* menu_mask is a u16 indexed by menu index (rule 6) */
		if (c.min != 0 || c.max < 0 || c.max >= RS_CTRL_MENU_MAX) {
			ctrl_warn(ctx, idx, "menu range %lld..%lld\n",
				  c.min, c.max);
			return -EINVAL;
		}
		n_entries = c.max + 1;
		if (c.step) {					/* rule 9 */
			ctrl_warn(ctx, idx, "menu with step %lld\n", c.step);
			return -EINVAL;
		}
		c.menu_mask = rec->menu_mask & GENMASK((unsigned long)c.max, 0);
		if (c.def < 0 || c.def > c.max ||		/* rule 8 */
		    (c.menu_mask & BIT((unsigned long)c.def))) {
			ctrl_warn(ctx, idx, "menu default %lld masked or out of range\n",
				  c.def);
			return -EINVAL;
		}
		if (!is_std) {
			const char **q = ctx->desc->qmenus + ctx->qmenu_used;
			unsigned int off = rec->menu_offset;

			/* Stream slices may overlap on the map; each resolves
			 * its own labels, so the pool can run out (rule 5).
			 */
			if (ctx->qmenu_used + n_entries > ctx->qmenu_cap) {
				ctrl_warn(ctx, idx, "label pool exhausted\n");
				return -EINVAL;
			}
			for (i = 0; i < n_entries; i++) {	/* rule 5 */
				const char *label = ds5_desc_string(ctx, off,
							RS_CTRL_NAME_MAX - 1);

				if (!label) {
					ctrl_warn(ctx, idx, "label %u at %u invalid\n",
						  i, off);
					return -EINVAL;
				}
				q[i] = label;
				off += strlen(label) + 1;
			}
			c.qmenu = q;
		} else {
			const char * const *m = v4l2_ctrl_get_menu(c.cid);
			unsigned int n = 0;

			/* validate_new() indexes qmenu[val] unchecked */
			while (m && m[n])
				n++;
			if (c.max >= n) {
				ctrl_warn(ctx, idx, "menu max %lld, Linux has %u entries\n",
					  c.max, n);
				return -EINVAL;
			}
		}
		if (rec->menu_wire_map_offset != RS_DESC_NONE) {	/* rule 5 */
			if (!ctx->wire ||
			    rec->menu_wire_map_offset + n_entries > ctx->n_wire) {
				ctrl_warn(ctx, idx, "wire map %u+%u exceeds %u\n",
					  rec->menu_wire_map_offset, n_entries,
					  ctx->n_wire);
				return -EINVAL;
			}
			c.wire_map = ctx->desc->wire_map +
				     rec->menu_wire_map_offset;
		}
	} else if (rec->menu_mask) {				/* rule 9 */
		ctrl_warn(ctx, idx, "menu_mask on a non-menu\n");
		return -EINVAL;
	}

	/* rule 7: the transfer fits, checked on the arm transport names */
	if (rec->transport == RS_CTRL_XPORT_REG) {
		const struct rs_desc_mech_reg *m = &rec->mech.reg;
		u32 addr = m->addr, extent = m->extent;
		bool ok;

		if (rec->elems == 1)
			ok = extent == 1 || extent == 2 || extent == 4;
		else
			ok = extent == (u32)rec->elems * c.elem_size;
		if (!extent || extent > RS_CTRL_MAX_EXTENT || !ok) {
			ctrl_warn(ctx, idx, "extent %u for %u x %u\n",
				  extent, rec->elems, c.elem_size);
			return -EINVAL;
		}
		/* the latch is the two half-words of one 4-byte register */
		if ((rec->flags & RS_CTRL_F_LSW_LATCH) &&
		    (rec->elems != 1 || extent != 4)) {
			ctrl_warn(ctx, idx, "latch on a %u-byte window\n", extent);
			return -EINVAL;
		}
		if (rec->flags & RS_CTRL_F_ADDR_RELATIVE)
			addr += st->ctrl_base;
		if (addr + extent > U16_MAX + 1) {
			ctrl_warn(ctx, idx, "window 0x%x+%u leaves the register space\n",
				  addr, extent);
			return -EINVAL;
		}
		/* rule 12: a result the driver reads must be this write's */
		if (!(rec->flags & RS_CTRL_F_NO_STATUS)) {
			if (!st->status_addr || addr < st->ctrl_base ||
			    addr + extent > (u32)st->ctrl_base + 0x100) {
				ctrl_warn(ctx, idx, "0x%x reports no result into 0x%x\n",
					  addr, st->status_addr);
				return -EINVAL;
			}
			c.status_addr = st->status_addr;
		}
		/* rule 2: no overlap with another REG window on this node */
		for (i = 0; i < n_done; i++) {
			if (done[i].transport != RS_CTRL_XPORT_REG)
				continue;
			if (addr < done[i].reg.addr + done[i].reg.extent &&
			    done[i].reg.addr < addr + extent) {
				ctrl_warn(ctx, idx, "0x%x+%u overlaps 0x%x+%u\n",
					  addr, extent, done[i].reg.addr,
					  done[i].reg.extent);
				return -EINVAL;
			}
		}
		c.reg.addr = addr;
		c.reg.extent = extent;
	} else {
		const struct rs_desc_mech_hwmc *m = &rec->mech.hwmc;
		u32 payload = (u32)rec->elems * c.elem_size;
		u32 resp_max = payload +
			       ((rec->flags & RS_CTRL_F_RESP_SKIP_HDR) ? 4 : 0);
		bool set_in_data = m->opcode_set != 0xFF && !m->value_param;
		bool wrapped = false;
		u8 value_mask = 0;
		unsigned int n_const;

		/* rule 1: an opcode the driver sequences itself is accepted
		 * only under its own control; that arm also sizes the transfer.
		 */
		for (i = 0; i < ARRAY_SIZE(ds5_desc_wrapped_opcodes); i++) {
			u8 op = ds5_desc_wrapped_opcodes[i].opcode;

			if (m->opcode_set != op && m->opcode_get != op)
				continue;
			if (rec->control_id !=
			    ds5_desc_wrapped_opcodes[i].control_id) {
				ctrl_warn(ctx, idx, "opcode 0x%02x needs the driver's hook, id %u has none\n",
					  op, rec->control_id);
				return -EINVAL;
			}
			wrapped = true;
		}
		/* rule 7: a value sent in Data shares the command buffer with
		 * the header; a response is bounded by the data window.
		 */
		if ((!wrapped && set_in_data &&
		     payload > DS5_HWMC_BUFFER_SIZE - RS_HWMC_HDR_SIZE) ||
		    (!wrapped && resp_max > DS5_HWMC_BUFFER_SIZE) ||
		    (m->value_param &&
		     m->value_param + rec->elems - 1 > RS_CTRL_HWMC_PARAMS) ||
		    m->resp_bytes > resp_max) {
			ctrl_warn(ctx, idx, "hwmc %u x %u, value param %u, resp %u do not fit\n",
				  rec->elems, c.elem_size, m->value_param,
				  m->resp_bytes);
			return -EINVAL;
		}
		/* rule 7, constants: a slot is a value or a constant, and the
		 * run of constants lies inside the pool.
		 */
		if (m->value_param)
			value_mask = GENMASK(m->value_param + rec->elems - 2,
					     m->value_param - 1);
		n_const = hweight8(m->const_mask);
		if ((m->const_mask & ~GENMASK(RS_CTRL_HWMC_PARAMS - 1, 0)) ||
		    (m->const_mask & value_mask) ||
		    (n_const && (!ctx->hconst ||
				 m->const_offset + n_const > ctx->n_hconst))) {
			ctrl_warn(ctx, idx, "hwmc constants 0x%x at %u clash or exceed %u\n",
				  m->const_mask, m->const_offset, ctx->n_hconst);
			return -EINVAL;
		}
		c.hwmc.opcode_set = m->opcode_set;
		c.hwmc.opcode_get = m->opcode_get;
		c.hwmc.value_param = m->value_param;
		c.hwmc.const_mask = m->const_mask;
		c.hwmc.consts = n_const ? ctx->desc->hwmc_const + m->const_offset
					: NULL;
		c.hwmc.resp_bytes = m->resp_bytes;
	}

	/* rule 3 */
	for (i = 0; i < n_done; i++) {
		if (done[i].cid == c.cid) {
			ctrl_warn(ctx, idx, "CID 0x%x already registered on this node\n",
				  c.cid);
			return -EINVAL;
		}
	}

	if (rec->flags & RS_CTRL_F_READ_ONLY)
		c.v4l2_flags |= V4L2_CTRL_FLAG_READ_ONLY;
	if (rec->flags & RS_CTRL_F_VOLATILE)
		c.v4l2_flags |= V4L2_CTRL_FLAG_VOLATILE;
	if (rec->flags & RS_CTRL_F_EXEC_ON_WRITE)
		c.v4l2_flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;

	*out = c;
	if (c.qmenu)
		ctx->qmenu_used += c.max + 1;
	return 0;
}

static int ds5_desc_build_stream_ctrls(struct ds5_desc_ctrl_ctx *ctx,
				       const struct rs_desc_stream *st)
{
	struct ds5_desc_stream *out = &ctx->desc->streams[st->stream_id];
	unsigned int first = st->ctrl_first, count = st->ctrl_count, i;

	if (!count)
		return 0;
	/* rule 5: a slice past the map means the blob's own cross-references
	 * do not hold, so no record can be trusted and the set goes whole.
	 */
	if (first + count > ctx->map->n_entries) {
		dev_warn(&ctx->state->client->dev,
			 "control descriptor: stream %u: map slice %u+%u exceeds %u\n",
			 st->stream_id, first, count, ctx->map->n_entries);
		return -EINVAL;
	}
	out->ctrls = kcalloc(count, sizeof(*out->ctrls), GFP_KERNEL);
	if (!out->ctrls)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		struct rs_desc_control rec;
		u16 idx;

		memcpy(&idx, ctx->blob + ctx->map->offset +
			     (first + i) * sizeof(idx), sizeof(idx));
		if (idx >= ctx->ctrl->n_entries) {		/* rule 5 */
			dev_warn(&ctx->state->client->dev,
				 "control descriptor: stream %u: map[%u] = %u exceeds %u controls\n",
				 st->stream_id, first + i, idx,
				 ctx->ctrl->n_entries);
			return -EINVAL;
		}
		ds5_desc_rec(ctx->blob, ctx->ctrl, idx, &rec, sizeof(rec));
		/* A bad record costs itself and nothing else. */
		if (!ds5_desc_build_ctrl(ctx, st, &rec, idx, out->ctrls,
					 out->n_ctrls, &out->ctrls[out->n_ctrls]))
			out->n_ctrls++;
	}
	return 0;
}

/* Parse the control tables into per-stream resolved records. -ENODATA when
 * the blob carries none, -EINVAL when the set is rejected whole; a failing
 * record only skips itself.
 */
static int ds5_desc_build_controls(struct ds5 *state, struct ds5_desc *desc,
				   const u8 *blob,
				   const struct rs_desc_toc **tabs)
{
	struct ds5_desc_ctrl_ctx ctx = {
		.state = state,
		.desc = desc,
		.blob = blob,
		.map = tabs[RS_DESC_T_CTRL_STREAM_MAP],
		.ctrl = tabs[RS_DESC_T_CONTROL],
		.bounds = tabs[RS_DESC_T_CTRL_BOUNDS],
		.strings = tabs[RS_DESC_T_CTRL_STRING],
		.wire = tabs[RS_DESC_T_CTRL_MENU_WIRE_MAP],
		.hconst = tabs[RS_DESC_T_CTRL_HWMC_CONST],
	};
	unsigned int i;
	int ret;

	ret = ds5_desc_check_ctrl_tables(state, tabs);
	if (ret)
		return ret;

	/* The pools are copied out: the V4L2 core keeps the name and qmenu
	 * pointers it is handed, so they must outlive the fetch buffer.
	 */
	ctx.n_strings = ctx.strings->n_entries;
	desc->strings = kmemdup(blob + ctx.strings->offset, ctx.n_strings,
				GFP_KERNEL);
	if (!desc->strings)
		return -ENOMEM;
	if (desc->strings[ctx.n_strings - 1] != '\0') {
		dev_warn(&state->client->dev,
			 "control descriptor: string pool unterminated\n");
		return -EINVAL;
	}
	if (ctx.wire) {
		ctx.n_wire = ctx.wire->n_entries;
		desc->wire_map = kmalloc_array(ctx.n_wire, sizeof(u16),
					       GFP_KERNEL);
		if (!desc->wire_map)
			return -ENOMEM;
		memcpy(desc->wire_map, blob + ctx.wire->offset,
		       ctx.n_wire * sizeof(u16));
	}
	if (ctx.hconst) {
		ctx.n_hconst = ctx.hconst->n_entries;
		desc->hwmc_const = kmalloc_array(ctx.n_hconst, sizeof(u16),
						 GFP_KERNEL);
		if (!desc->hwmc_const)
			return -ENOMEM;
		memcpy(desc->hwmc_const, blob + ctx.hconst->offset,
		       ctx.n_hconst * sizeof(u16));
	}
	/* Every map slot a private menu of RS_CTRL_MENU_MAX labels; the map,
	 * not the control table, since a control two streams share resolves
	 * twice. Overlapping slices are bounded by qmenu_cap at build time.
	 */
	ctx.qmenu_cap = (unsigned int)ctx.map->n_entries * RS_CTRL_MENU_MAX;
	desc->qmenus = kcalloc(ctx.qmenu_cap, sizeof(*desc->qmenus),
			       GFP_KERNEL);
	if (!desc->qmenus)
		return -ENOMEM;

	for (i = 0; i < tabs[RS_DESC_T_STREAM]->n_entries; i++) {
		struct rs_desc_stream st;

		ds5_desc_rec(blob, tabs[RS_DESC_T_STREAM], i, &st, sizeof(st));
		ret = ds5_desc_build_stream_ctrls(&ctx, &st);
		if (ret)
			return ret;
	}
	return 0;
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
	/* The control set is optional and stands apart: the format tables have
	 * their own TOC entries and bounds, so a rejected set costs only itself.
	 */
	ret = ds5_desc_build_controls(state, desc, blob, tabs);
	if (ret && ret != -ENODATA) {
		dev_warn(&state->client->dev,
			 "control descriptor rejected (%d), using built-in controls\n",
			 ret);
		ds5_desc_drop_controls(desc);
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
		 "format descriptor v%u.%u crc 0x%08x: %u/%u/%u/%u formats, %u/%u/%u/%u controls (depth/ir/rgb/imu)\n",
		 pub->ver_major, pub->ver_minor, pub->crc,
		 pub->streams[DS5_STREAM_DEPTH].n_formats,
		 pub->streams[DS5_STREAM_IR].n_formats,
		 pub->streams[DS5_STREAM_RGB].n_formats,
		 pub->streams[DS5_STREAM_IMU].n_formats,
		 pub->streams[DS5_STREAM_DEPTH].n_ctrls,
		 pub->streams[DS5_STREAM_IR].n_ctrls,
		 pub->streams[DS5_STREAM_RGB].n_ctrls,
		 pub->streams[DS5_STREAM_IMU].n_ctrls);
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
	sensor->desc_ctrls = st->ctrls;
	sensor->n_desc_ctrls = st->n_ctrls;
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

/* Same mark for the control set: the VI re-syncs each subdev's handler on
 * the next control ioctl, outside video_lock and every handler lock.
 */
static void ds5_mark_channel_ctrls_dirty(struct ds5 *inst)
{
	struct tegra_channel *chan =
		v4l2_get_subdev_hostdata(&inst->mux.sd.subdev);

	if (chan)
		tegra_channel_mark_ctrls_dirty(chan);
}
#else
static inline void ds5_mark_channel_fmts_dirty(struct ds5 *inst) { }
static inline void ds5_mark_channel_ctrls_dirty(struct ds5 *inst) { }
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
	 *
	 * Control records swap with the formats. Re-ranging the handlers here
	 * would deadlock (this runs under the reset node's handler lock, and a
	 * sibling's s_ctrl takes its handler lock before dd->lock), so the VI
	 * is marked and re-syncs each node on its next control ioctl.
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
		if (changed && have_desc)
			ds5_mark_channel_ctrls_dirty(inst);
	}
	mutex_unlock(&dd->lock);

	if (have_desc)
		dev_info(&state->client->dev,
			 "descriptor tables re-applied after HW reset (crc 0x%08x); nodes refreshed%s\n",
			 crc, changed ? ", controls re-sync at next control ioctl" : "");
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

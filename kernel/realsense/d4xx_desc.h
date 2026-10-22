/* SPDX-License-Identifier: GPL-2.0 */
/*
 * d4xx_desc.h - camera-resident format descriptor, driver-facing interface
 *
 * Copyright (c) 2026, RealSense, Inc.  All rights reserved.
 */

#ifndef __D4XX_DESC_H__
#define __D4XX_DESC_H__

#include <linux/bits.h>		/* BIT() for RS_CTRL_F_* */
#include <linux/types.h>

struct ds5;

enum ds5_desc_state {
	DS5_DESC_UNKNOWN,
	DS5_DESC_READY,
	DS5_DESC_ABSENT,        /* none served, or rejected: static tables in use */
};

/* Parsed descriptor. Opaque outside d4xx_desc.c: the tables live until
 * module exit, so only the pointer is ever handed around.
 */
struct ds5_desc;

/* Control registries shared with firmware: append-only, a value never changes
 * meaning. These are the wire values of rs_desc_control's type and transport.
 */
enum rs_ctrl_type {
	RS_CTRL_T_INTEGER = 1,
	RS_CTRL_T_BOOLEAN,
	RS_CTRL_T_MENU,
	RS_CTRL_T_BUTTON,
	RS_CTRL_T_U8_ARRAY,
	RS_CTRL_T_U32_ARRAY,
};

enum rs_ctrl_transport {
	RS_CTRL_XPORT_REG = 1,	/* a packed struct at a register address */
	RS_CTRL_XPORT_HWMC,	/* a mailbox command */
};

#define RS_CTRL_F_READ_ONLY	BIT(0)
#define RS_CTRL_F_VOLATILE	BIT(1)	/* read: ask the camera, never the core's copy */
#define RS_CTRL_F_EXEC_ON_WRITE	BIT(2)	/* write: reach the camera even if unchanged */
#define RS_CTRL_F_READ_TO_CLEAR	BIT(3)	/* a read consumes the value */
#define RS_CTRL_F_ADDR_RELATIVE	BIT(4)	/* REG: addr is an offset into the stream's ctrl_base */
#define RS_CTRL_F_CAMERA_SCOPE	BIT(5)	/* applies to the camera, not one sensor */
#define RS_CTRL_F_STANDARD	BIT(6)	/* control_id indexes the standard-CID map */
#define RS_CTRL_F_RESP_SKIP_HDR	BIT(7)	/* HWMC: drop the 4-byte opcode header from the response */
#define RS_CTRL_F_NO_STATUS	BIT(8)	/* firmware reports no result for this write */
#define RS_CTRL_F_LSW_LATCH	BIT(9)	/* REG: 16-bit halves, firmware acts on the low one */

/* Largest REG window: one regmap transfer, as the descriptor fetch chunk
 * proves good. The parser refuses more; the executors size their buffer by it.
 */
#define RS_CTRL_MAX_EXTENT	256
#define RS_CTRL_HWMC_PARAMS	4	/* param1..param4 of struct hwm_cmd */

/* One control as the driver operates it, resolved for the stream that
 * advertises it: addr against that stream's ctrl_base, bounds substituted,
 * CID assigned. A control two streams share is therefore two of these.
 * name and qmenu are module-lifetime - the V4L2 core stores the pointers.
 */
struct ds5_desc_ctrl {
	u32 cid;
	const char *name;		/* never NULL */
	const char * const *qmenu;	/* private menu: max + 1 labels; else NULL */
	const u16 *wire_map;		/* menu index -> firmware value; NULL = identity */
	u32 type;			/* V4L2_CTRL_TYPE_* */
	u32 elems;
	u32 elem_size;
	s64 min, max, step, def;
	u64 menu_mask;
	u32 v4l2_flags;			/* V4L2_CTRL_FLAG_*, translated */
	u16 flags;			/* RS_CTRL_F_*, for the executors */
	u8 transport;			/* enum rs_ctrl_transport */
	union {
		struct {
			u16 addr;		/* absolute */
			u16 extent;		/* bytes transferred */
		} reg;
		struct {
			u8 opcode_set;		/* 0xFF = not settable */
			u8 opcode_get;		/* 0xFF = not gettable */
			u8 value_param;		/* 0 = value in Data; else first of param1..4 */
			u8 const_mask;		/* bit i = param(i+1) is a constant */
			const u16 *consts;	/* hweight(const_mask) values, in slot order */
			u16 resp_bytes;		/* 0 = take it from DS5_HWMC_RESP_LEN */
		} hwmc;
	};
	u16 status_addr;		/* 0 = no result to read after a write */
};

/* Fetch (once) and apply the camera's tables to this instance's sensors.
 * True when the descriptor supplied them; false leaves the built-in tables.
 * Takes ds5_dev->lock.
 */
bool ds5_desc_apply(struct ds5 *state);

/* Re-fetch after a HW reset and refresh every node of this camera.
 * @dev_type is what the recovery path just validated.
 */
void ds5_desc_reload_after_reset(struct ds5 *state, u16 dev_type);

/* Release every parsed table. Module exit only. */
void ds5_desc_free_all(void);

#endif /* __D4XX_DESC_H__ */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * d4xx_desc.h - camera-resident format descriptor, driver-facing interface
 *
 * Copyright (c) 2026, RealSense, Inc.  All rights reserved.
 */

#ifndef __D4XX_DESC_H__
#define __D4XX_DESC_H__

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

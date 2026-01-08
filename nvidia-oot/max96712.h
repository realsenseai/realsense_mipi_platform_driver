/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/**
 * @file
 * <b>MAX96712 API: For Maxim Integrated MAX96712 deserializer</b>
 *
 * @b Description: Defines elements used to set up and use a
 *  Maxim Integrated MAX96712 deserializer.
 */

#ifndef __MAX96712_H__
#define __MAX96712_H__

#include <linux/types.h>
#include <media/gmsl-link.h>
/**
 * \defgroup max96712 MAX96712 deserializer driver
 *
 * Controls the MAX96712 deserializer module.
 *
 * @ingroup serdes_group
 * @{
 */

int max96712_get_available_pipe_id(struct device *dev, int vc_id);
int max96712_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
		     u8 data_type2, u32 vc_id);
int max96712_release_pipe(struct device *dev, int pipe_id);
void max96712_reset_oneshot(struct device *dev);
int max96712_setup_link(struct device *dev, struct device *s_dev);
int max96712_setup_control(struct device *dev, struct device *s_dev);
int max96712_reset_control(struct device *dev, struct device *s_dev);
int max96712_sdev_register(struct device *dev, struct gmsl_link_ctx *g_ctx);
int max96712_sdev_unregister(struct device *dev, struct device *s_dev);
int max96712_power_on(struct device *dev);
void max96712_power_off(struct device *dev);
int max96712_init_settings(struct device *dev);

#endif  /* __MAX96712_H__ */

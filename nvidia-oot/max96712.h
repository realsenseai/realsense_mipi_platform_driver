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
/**
 * @brief  Returns the multi-VC pipe dedicated to the link that owns vc_id,
 * allocating one on first request. Used for MAX96717 serializers, which route
 * all of a camera's virtual channels through a single pipe. The pipe is sticky
 * for the driver's lifetime and is never freed by max96712_release_pipe().
 *
 * @param  [in]  dev    The deserializer device handle.
 * @param  [in]  vc_id  A virtual channel id of the requesting camera; the link
 *                      is derived as vc_id / 4.
 *
 * @return  pipe id (>= 0) on success, negative error code otherwise.
 */
int max96712_get_multi_vc_pipe_id(struct device *dev, int vc_id);
int max96712_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
		     u8 data_type2, u32 vc_id);
int max96712_release_pipe(struct device *dev, int pipe_id);
void max96712_reset_oneshot(struct device *dev);
/* RSDEV-12608: flush one GMSL link's pixel line buffer after a camera HW reset;
 * called per-link from d4xx's ds5_hw_reset_with_recovery(). */
void max96712_reset_oneshot_link(struct device *dev, u32 vc_id);
int max96712_setup_link(struct device *dev, struct device *s_dev);
int max96712_setup_control(struct device *dev, struct device *s_dev);
int max96712_reset_control(struct device *dev, struct device *s_dev);
int max96712_sdev_register(struct device *dev, struct gmsl_link_ctx *g_ctx);
int max96712_sdev_unregister(struct device *dev, struct device *s_dev);
int max96712_power_on(struct device *dev);
void max96712_power_off(struct device *dev);
int max96712_init_settings(struct device *dev);
/**
 * @brief  Maps dserializer to serializer pipe id
 *
 * @param [in]  dev             The deserializer device handle.
 * @param [in]  dser_pipe_id    Deserializer pipe id.
 * @param [in]  ser_pipe_id     Serializer pipe id.
 * @param [in]  vc_id           vc_id associated with this pipe path.
 */
int max96712_bind_ser_to_dser_pipe(struct device *dev, int dser_pipe_id, int ser_pipe_id, u32 vc_id);

#endif  /* __MAX96712_H__ */

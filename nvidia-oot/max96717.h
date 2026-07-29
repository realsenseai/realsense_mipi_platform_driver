/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/**
 * @file
 * <b>MAX96717 API: For Maxim Integrated MAX96717 serializer</b>
 *
 * @b Description: Defines elements used to set up and use a
 *  Maxim Integrated MAX96717 serializer.
 */

#ifndef __MAX96717_H__
#define __MAX96717_H__

#include <linux/types.h>
#include <media/gmsl-link.h>
/**
 * \defgroup max96717 MAX96717 serializer driver
 *
 * Controls the MAX96717 serializer module.
 *
 * @ingroup serdes_group
 * @{
 */

int max96717_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
		     u8 data_type2, u32 vc_id);

/**
 * @brief  Notifies the serializer that a stream (identified by its serializer
 * virtual channel) has stopped. When the last active stream stops, the MIPI RX
 * PHY is re-armed (mipi_rx_reset pulse) so the next stream re-locks cleanly.
 *
 * @param  [in]  dev    The serializer device handle.
 * @param  [in]  vc_id  The serializer virtual channel id of the stopped stream.
 *
 * @return  0 on success, error code otherwise.
 */
int max96717_stream_stop(struct device *dev, u32 vc_id);

/**
 * @brief  Powers on a serializer device and performs the I2C overrides
 * for sensor and serializer devices.
 *
 * The I2C overrides include setting proxy I2C slave addresses for the devices.
 *
 * Before the client calls this function it must ensure that
 * the deserializer device is in exclusive link mode.
 *
 * @param  [in]  dev            The serializer device handle.
 *
 * @return  0 for success, or -1 otherwise.
 */
int max96717_setup_control(struct device *dev);

/**
 * Reverts I2C overrides and resets a serializer device.
 *
 * @param  [in]  dev            The serializer device handle.
 *
 * @return  0 for success, or -1 otherwise.
 */
int max96717_reset_control(struct device *dev);

/**
 * @brief  Pairs a sensor device with a serializer device.
 *
 * To be called by sensor client driver.
 *
 * @param  [in]  dev            The deserializer device handle.
 * @param  [in]  g_ctx          The @ref gmsl_link_ctx structure handle.
 *
 * @return  0 for success, or -1 otherwise.
 */
int max96717_sdev_pair(struct device *dev, struct gmsl_link_ctx *g_ctx);

/**
 * @brief Unpairs a sensor device from a serializer device.
 *
 * To be called by sensor client driver.
 *
 * @param  [in]  dev            The serializer device handle.
 * @param  [in]  s_dev          The sensor device handle.
 *
 * @return  0 for success, or -1 otherwise.
 */
int max96717_sdev_unpair(struct device *dev, struct device *s_dev);

int max96717_init_settings(struct device *dev);

int max96717_enable_gpio_tunneling(struct device *dev);
int max96717_disable_gpio_tunneling(struct device *dev);

/** @} */

#endif  /* __MAX96717_H__ */

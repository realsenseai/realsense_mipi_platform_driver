/*
 * max96717.c - MAX96717 GMSL2 Serializer driver (Tunnel Mode)
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

/**
 * @file
 * <b>MAX96717 API: For Analog Devices MAX96717 GMSL2 serializer</b>
 *
 * @b Description: Defines elements used to set up and use an
 *  Analog Devices MAX96717 GMSL2 serializer in Tunnel Mode.
 */

#ifndef __MAX96717_H__
#define __MAX96717_H__

#include <linux/types.h>
#include <media/gmsl-link.h>

/**
 * \defgroup max96717 MAX96717 serializer driver
 *
 * Controls the MAX96717 GMSL2 serializer module (Tunnel Mode).
 *
 * @ingroup serdes_group
 * @{
 */

/**
 * @brief  Configures a single pipe on the serializer.
 *
 * In Tunnel Mode, per-pipe DT/VC filtering is not active on the serializer
 * side (all VCs are tunneled transparently). This function maintains API
 * compatibility with the d4xx sensor driver framework.
 *
 * @param  [in]  dev          The serializer device handle.
 * @param  [in]  pipe_id      Pipe index (0..3).
 * @param  [in]  data_type1   Primary CSI-2 data type.
 * @param  [in]  data_type2   Secondary CSI-2 data type (e.g. embedded).
 * @param  [in]  vc_id        Virtual channel ID.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
		      u8 data_type2, u32 vc_id);

/**
 * @brief  Toggle SER TX to restore tunnel detection after DES ONESHOT.
 *
 * A DES ONESHOT resets the video data path. The serializer must cycle
 * its TX enable (REG2 0x03→0x43) for the deserializer to re-acquire
 * the tunnel stream.
 *
 * @param  [in]  dev          The serializer device handle.
 */
void max96717_retrigger_tx(struct device *dev);
void max96717_log_control_status(struct device *dev);

/**
 * @brief  Powers on the serializer and performs I2C overrides
 * for sensor and serializer devices.
 *
 * Must be called after the deserializer's max96724_setup_link().
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_setup_control(struct device *dev);

/**
 * @brief  Reverts I2C overrides and resets the serializer.
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_reset_control(struct device *dev);

/**
 * @brief  Pairs a sensor device with the serializer.
 *
 * To be called by the sensor client driver.
 *
 * @param  [in]  dev          The serializer device handle.
 * @param  [in]  g_ctx        The gmsl_link_ctx structure handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_sdev_pair(struct device *dev, struct gmsl_link_ctx *g_ctx);

/**
 * @brief  Unpairs a sensor device from the serializer.
 *
 * @param  [in]  dev          The serializer device handle.
 * @param  [in]  s_dev        The sensor device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_sdev_unpair(struct device *dev, struct device *s_dev);

/**
 * @brief  Sets up the serializer's internal streaming pipeline.
 *
 * Configures CSI-2 input mode, lane mapping, and enables Tunnel Mode
 * (EXT11[7]=1 at register 0x0383).
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_setup_streaming(struct device *dev);

/**
 * @brief  Applies initial register settings for the serializer.
 *
 * Configures default pipe settings and Tunnel Mode registers.
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_init_settings(struct device *dev);

/**
 * @brief  No-op hook for GPIO-over-GMSL tunneling for FSIN/FOUT signals.
 *
 * The signal map is documented, but the concrete MFP_CFG values are
 * not available yet. This hook currently logs and returns success.
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_setup_gpio_tunneling(struct device *dev);

/**
 * @brief  Disables GPIO-over-GMSL tunneling for FSIN/FOUT signals.
 *
 * Restores the serializer GPIOs to their default non-tunneled function.
 *
 * @param  [in]  dev          The serializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96717_disable_gpio_tunneling(struct device *dev);

/** @} */

#endif  /* __MAX96717_H__ */

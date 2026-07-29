/* SPDX-License-Identifier: GPL-2.0 */
/*
 * max96724.h - MAX96724 GMSL2 Quad Deserializer driver (Tunnel Mode)
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
 * <b>MAX96724 API: For Analog Devices MAX96724 GMSL2 quad deserializer</b>
 *
 * @b Description: Defines elements used to set up and use an
 *  Analog Devices MAX96724 quad GMSL2 deserializer in Tunnel Mode.
 */

#ifndef __MAX96724_H__
#define __MAX96724_H__

#include <linux/types.h>
#include <media/gmsl-link.h>

/**
 * \defgroup max96724 MAX96724 deserializer driver
 *
 * Controls the MAX96724 quad GMSL2 deserializer module (Tunnel Mode).
 *
 * @ingroup serdes_group
 * @{
 */

int max96724_get_available_pipe_id(struct device *dev, int vc_id);
int max96724_set_pipe(struct device *dev, int pipe_id, u8 data_type1,
		      u8 data_type2, u32 vc_id);
int max96724_release_pipe(struct device *dev, int pipe_id);
void max96724_reset_oneshot(struct device *dev);
void max96724_retrigger_datapath(struct device *dev);

/**
 * @brief  Assigns serializer addresses on all configured GMSL links.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  s_dev        The sensor device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_setup_link(struct device *dev, struct device *s_dev);

/**
 * @brief  Sets up the deserializer link's control pipeline.
 *
 * Configures I2C pass-through and enables tunnel-mode pipe routing
 * for the source.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  s_dev        The sensor device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_setup_control(struct device *dev, struct device *s_dev);

/**
 * @brief  Resets the deserializer link control pipeline.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  s_dev        The sensor device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_reset_control(struct device *dev, struct device *s_dev);

/**
 * @brief  Registers a source sensor device with the deserializer.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  g_ctx        The gmsl_link_ctx structure handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_sdev_register(struct device *dev, struct gmsl_link_ctx *g_ctx);

/**
 * @brief  Unregisters a source sensor device from the deserializer.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  s_dev        The sensor device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_sdev_unregister(struct device *dev, struct device *s_dev);

/**
 * @brief  Powers on the MAX96724 deserializer module.
 *
 * Asserts shared reset GPIO and powers on the regulator.
 *
 * @param  [in]  dev          The deserializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_power_on(struct device *dev);

/**
 * @brief  Powers off the MAX96724 deserializer module.
 *
 * @param  [in]  dev          The deserializer device handle.
 */
void max96724_power_off(struct device *dev);

/**
 * @brief  Applies initial register settings for the deserializer.
 *
 * Configures default pipe mappings, tunnel mode, and CSI output for
 * the D5xx/SC1.2 use case.
 *
 * @param  [in]  dev          The deserializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_init_settings(struct device *dev);

/**
 * @brief  Enables the MAX96724 internal FSYNC generator.
 *
 * Programs the deserializer as the FSYNC master and broadcasts the
 * generated sync signal over the configured GMSL GPIO channel.
 *
 * @param  [in]  dev          The deserializer device handle.
 * @param  [in]  fps          Desired frame-sync rate in Hz.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_setup_fsync(struct device *dev, u32 fps);

/**
 * @brief  Disables the MAX96724 internal FSYNC generator.
 *
 * @param  [in]  dev          The deserializer device handle.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_disable_fsync(struct device *dev);

/**
 * @brief  Maps deserializer pipe ID to serializer pipe ID.
 *
 * In tunnel mode with MAX96717, all data flows through a single pipe
 * per camera, so the mapping is 1:1.
 *
 * @param  [in]  dev             The deserializer device handle.
 * @param  [in]  dser_pipe_id    Deserializer pipe ID.
 * @param  [in]  ser_pipe_id     Serializer pipe ID.
 * @param  [in]  vc_id           VC ID associated with this pipe.
 *
 * @return  0 for success, or negative error code.
 */
int max96724_bind_ser_to_dser_pipe(struct device *dev, int dser_pipe_id,
				   int ser_pipe_id, u32 vc_id);

/** @} */

#endif  /* __MAX96724_H__ */

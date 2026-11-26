/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stand-in for NVIDIA's media/camera_common.h, so the SerDes drivers build
 * off-Tegra from the unmodified NVIDIA sources (RSDEV-15171).
 *
 * max9295, max9296, max96712, max96717 and max96724 all include this header
 * but reference no symbol *declared* in it - what they actually take from it
 * is its transitive includes, NVIDIA's version being a kitchen-sink header.
 * So this shim is just that subset. Pulling in the real Tegra header instead
 * would drag camera_common_data and the tegra-camera-platform graph onto a
 * platform that has neither.
 *
 * If a future SerDes change needs a symbol genuinely declared by NVIDIA's
 * camera_common.h, port it deliberately rather than widening this shim.
 */

#ifndef __QCOM_MEDIA_CAMERA_COMMON_H__
#define __QCOM_MEDIA_CAMERA_COMMON_H__

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>

#endif /* __QCOM_MEDIA_CAMERA_COMMON_H__ */

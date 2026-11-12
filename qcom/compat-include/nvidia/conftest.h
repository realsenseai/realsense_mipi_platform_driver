/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stand-in for NVIDIA's generated nvidia/conftest.h, so the SerDes drivers
 * build off-Tegra from the unmodified NVIDIA sources (RSDEV-15171).
 *
 * NVIDIA probes the kernel at build time and emits these; off-Tegra we decide
 * them from LINUX_VERSION_CODE instead. Keep this list to what the SerDes
 * sources actually reference - an unused define here hides a real porting gap.
 */

#ifndef __QCOM_NVIDIA_CONFTEST_H__
#define __QCOM_NVIDIA_CONFTEST_H__

#include <linux/version.h>

/* i2c_driver::probe lost its i2c_device_id argument in v6.3. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG
#endif

/* i2c_driver::remove went from int to void in v6.1. The NVIDIA macro names the
 * old int-returning form, so it must stay undefined on v6.1 and later. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
#define NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT
#endif

#endif /* __QCOM_NVIDIA_CONFTEST_H__ */

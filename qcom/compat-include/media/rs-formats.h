/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RealSense private media-bus codes and pixel formats for the Qualcomm build
 * (RSDEV-15171).
 *
 * On Jetson these live in a kernel UAPI patch because the Tegra VI format rows
 * are in-tree. Here every consumer is out-of-tree - d4xx and our camss fork -
 * so the codes need no kernel change at all. Userspace takes its fourccs from
 * librealsense's own headers, not from the kernel's.
 *
 * Values are the wire registry shared with the Jetson builds and the camera
 * firmware: keep them identical across platforms, and append only.
 */

#ifndef __QCOM_MEDIA_RS_FORMATS_H__
#define __QCOM_MEDIA_RS_FORMATS_H__

#include <linux/media-bus-format.h>
#include <linux/videodev2.h>

/* Carriers whose wire data type does not describe the payload, so the capture
 * driver picks geometry and surface handling from the code, not the DT.
 */
#ifndef MEDIA_BUS_FMT_RS_NV12_FLAT_1X8
#define MEDIA_BUS_FMT_RS_NV12_FLAT_1X8	0x5008
#endif
#ifndef MEDIA_BUS_FMT_RS_SBGGR10P_1X8
#define MEDIA_BUS_FMT_RS_SBGGR10P_1X8	0x5009
#endif
#ifndef MEDIA_BUS_FMT_RS_VARLEN_1X8
#define MEDIA_BUS_FMT_RS_VARLEN_1X8	0x500a
#endif
#ifndef MEDIA_BUS_FMT_RS_H264_1X8
#define MEDIA_BUS_FMT_RS_H264_1X8	0x500b
#endif
#ifndef MEDIA_BUS_FMT_RS_JPEG_1X8
#define MEDIA_BUS_FMT_RS_JPEG_1X8	0x500c
#endif

/* Z16, Y8I, Y12I, Y16I, GREY and SGRBG16 are already upstream from v6.8, so
 * only these two are ours.
 */
#ifndef V4L2_PIX_FMT_RW16
#define V4L2_PIX_FMT_RW16	v4l2_fourcc('R', 'W', '1', '6')
#endif
#ifndef V4L2_PIX_FMT_RS_VARLEN
#define V4L2_PIX_FMT_RS_VARLEN	v4l2_fourcc('R', 'S', 'V', 'L')
#endif

#endif /* __QCOM_MEDIA_RS_FORMATS_H__ */

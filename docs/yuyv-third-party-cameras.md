# YUYV colour with RealSense and third-party GMSL cameras on Jetson

## Summary

The RealSense MIPI/GMSL driver package changes one shared Jetson video format entry, YUYV. With that change, a third-party camera that sends YUYV on the same Jetson shows wrong colours (green and magenta). RealSense colour streams are correct.

You can choose one of two configurations:

- **Option A: keep the change (default).** Use this when only RealSense cameras send YUYV on the Jetson. RealSense colour is correct with no extra processing.
- **Option B: remove the YUYV change.** Use this when a third-party YUYV camera shares the Jetson with a RealSense camera. The third-party camera is correct. The RealSense colour bytes must then be swapped in software (by librealsense or by your application).

## Symptoms

The third-party camera streams at the correct size and frame rate, and edges and shapes are correct. Only the colours are wrong: the image is mostly green, with magenta where there should be red or blue. This happens because the luma (Y) and chroma (U/V) bytes of each pixel pair change places.

You can see the problem when all of these are true:

- The Jetson kernel was built with the RealSense MIPI driver patches, driver version 1.0.4.9 or later.
- A camera from another vendor is connected to the same Jetson, through GMSL or direct MIPI CSI-2.
- That camera's driver reports its colour stream as YUYV (media-bus code `MEDIA_BUS_FMT_YUYV8_1X16`).

The first report was a third-party GMSL2 depth camera's colour stream next to a RealSense D401 GMSL camera.

## Why it happens

The Jetson video input (VI) unit has one format table for the whole system. Each camera driver reports a media-bus code, and the VI uses the matching table row to decide how to store the bytes in memory. All cameras that report the same code use the same row.

The RealSense colour stream reports YUYV, but the bytes reach the VI in the opposite pair order. To store RealSense colour correctly without a software copy, driver 1.0.4.9 changed the byte order of the shared YUYV row:

| YUYV row setting | Byte order in memory | RealSense colour | Third-party YUYV camera |
| --- | --- | --- | --- |
| NVIDIA default (`T_Y8_U8__Y8_V8`) | As sent by the camera | Swapped, needs software fix | Correct |
| RealSense patch (`T_U8_Y8__V8_Y8`) | Each byte pair swapped | Correct | Swapped (green/magenta) |

One row cannot be right for both cameras. The change is in the patch `Fix-vi5-yuv422-tegra-format-mapping` (named `Fix-vi-yuv422-tegra-format-mapping` on JetPack 5.1.x).

## Your options

| | Option A: keep the change (default) | Option B: remove the YUYV change |
| --- | --- | --- |
| Use when | Only RealSense cameras send YUYV | A third-party YUYV camera shares the Jetson |
| Third-party YUYV camera | Wrong colours | Correct |
| RealSense colour | Correct, no CPU work | Correct after a software byte swap |
| CPU cost | None | None for RGB output in librealsense; one byte-swap pass per frame for YUYV output, or in your own application |
| librealsense | Works as released | One-line code change (see below) |
| Build change | None | Change one line before you build |

### Option A: keep the change

Do nothing. This is the configuration we ship and test. Do not connect cameras from other vendors that report YUYV to the same Jetson.

### Option B: remove the YUYV change

Option B reverts only the YUYV row to the NVIDIA default. Keep the rest of the patch: it also fixes the RealSense IR stereo (Y8I) stream.

1. Prepare the sources as usual: `./setup_workspace.sh <JetPack>` and then `./apply_patches.sh <JetPack>`.
2. Revert the YUYV row in the patched sources.
    - JetPack 6.x and 7.x:

      ```bash
      sed -i 's/\(YUYV8_1X16, 2, 1, \)T_U8_Y8__V8_Y8/\1T_Y8_U8__Y8_V8/' \
          sources_<JetPack>/nvidia-oot/drivers/media/platform/tegra/camera/vi/vi5_formats.h
      ```

    - JetPack 5.1.2 and 5.1.6: run the same command on `vi4_formats.h` and `vi5_formats.h` in `sources_<JetPack>/kernel/nvidia/drivers/media/platform/tegra/camera/vi/`.
    - JetPack 5.0.2: run the same command on `vi5_formats.h` only, in the same folder. The 5.0.2 patch does not change `vi4_formats.h`.
3. Make sure the row now reads:

    ```c
    TEGRA_VIDEO_FORMAT(YUV422, 16, YUYV8_1X16, 2, 1, T_Y8_U8__Y8_V8,
                YUV422_8, YUYV, "YUV 4:2:2"),
    ```

4. Build with `./build_all.sh <JetPack>`.
5. Install the complete module set and reboot, for example with `./scripts/deploy_kernel.sh`. Do not copy single `.ko` files.
6. Fix the RealSense colour byte order in software (next section).

### Software byte swap for Option B

After Option B, RealSense colour frames are labelled YUYV but the bytes are in UYVY order (`U Y0 V Y1`). To get YUYV, swap each pair of adjacent bytes: `U Y0 V Y1` becomes `Y0 U Y1 V`.

- **librealsense:** change one line and rebuild (see the next section).
- **Your own V4L2 or GStreamer application:** read the RealSense colour stream as UYVY, or swap the byte pairs before you convert the colour. Do not apply the swap to the third-party camera.

### librealsense change for Option B

librealsense selects the colour converter in `d400_color::register_processing_blocks()`, in [`src/ds/d400/d400-color.cpp`](https://github.com/realsenseai/librealsense/blob/c84fe6438bd14a4cc69f46deae92c993268232c6/src/ds/d400/d400-color.cpp#L353-L387). With driver 1.0.4.9 or later it uses the plain YUYV converter and does not swap. Older drivers take the Jetson branch, which swaps the bytes with `uyvy_converter` and `uyvy_to_yuyv` ([lines 368–377](https://github.com/realsenseai/librealsense/blob/c84fe6438bd14a4cc69f46deae92c993268232c6/src/ds/d400/d400-color.cpp#L368-L377)).

1. Change the condition at [line 360](https://github.com/realsenseai/librealsense/blob/c84fe6438bd14a4cc69f46deae92c993268232c6/src/ds/d400/d400-color.cpp#L360) so that MIPI devices always take the swap branch:

    ```cpp
    // Before
    if ( !_is_mipi_device || platform::get_jetson_driver_version() >= rsutils::version("1.0.4.9"))
    // After (Option B)
    if ( !_is_mipi_device )
    ```

2. Do not change the IR stereo (Y8I) check at [`src/ds/d400/d400-device.cpp` line 739](https://github.com/realsenseai/librealsense/blob/c84fe6438bd14a4cc69f46deae92c993268232c6/src/ds/d400/d400-device.cpp#L739). Option B keeps the driver's Y8I fix, so that check must stay as it is.
3. Rebuild and reinstall librealsense.

Use this librealsense change only together with the driver change from Option B. With the default driver (Option A), it swaps bytes that are already correct, and RealSense colour turns green and magenta.

The line numbers are for librealsense `master` at commit `c84fe64`. In other versions, search `d400-color.cpp` for `"1.0.4.9"`; the first match is the control check, so change the second one, in `register_processing_blocks()`.

## Tested configuration

Option B was tested on 2026-11-12 with a RealSense D401 GMSL camera, without a third-party camera:

| Item | Version |
| --- | --- |
| Platform | Jetson AGX Orin, JetPack 6.2 (L4T 36.4.3) |
| Camera | RealSense D401, firmware 5.17.6.4 |
| RealSense MIPI driver | 1.0.8.8 (`dev` branch, commit `7c05f40`) |
| librealsense | 2.59.1 (`master`, commit `c84fe64`) |

| Driver | librealsense | RealSense colour |
| --- | --- | --- |
| Option A | As released | Correct |
| Option A | With the Option B change | Green/magenta |
| Option B | As released | Green/magenta |
| Option B | With the Option B change | Correct (RGB and YUYV output) |

With Option B, a raw V4L2 capture of the RealSense colour node showed UYVY byte order, and depth streaming was not affected. The JetPack 5.x steps were not tested.

## Affected versions

The change is in RealSense MIPI driver 1.0.4.9 and later, on these JetPack versions:

| JetPack | Patch file in the driver repository |
| --- | --- |
| 7.2 | `nvidia-oot/7.2/0009-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 7.1 | `nvidia-oot/7.1/0009-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 7.0 | `nvidia-oot/7.0/0009-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 6.2, 6.2.1, 6.2.2 | `nvidia-oot/6.2/0010-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 6.1 | `nvidia-oot/6.1/0010-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 6.0 | `nvidia-oot/6.0/0010-Fix-vi5-yuv422-tegra-format-mapping.patch` |
| 5.1.6 | `kernel/nvidia/5.1.6/0011-Fix-vi-yuv422-tegra-format-mapping.patch` |
| 5.1.2 | `kernel/nvidia/5.1.2/0011-Fix-vi-yuv422-tegra-format-mapping.patch` |
| 5.0.2 | `kernel/nvidia/5.0.2/0021-Fix-vi5-yuv422-tegra-format-mapping.patch` |

JetPack 6.1 and 7.2 builds made before this patch was added to them keep the NVIDIA default YUYV row, so they behave like Option B.

## Next steps

- We plan to add a librealsense setting that forces the colour byte swap for Option B.
- Until then, choose Option A or Option B for your system. Contact RealSense support if you need help with the build steps.

#!/bin/bash
# Jetson Linux
# JP6.0 https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v2.0/sources/public_sources.tbz2
set -e

if [[ $# < 1 || "$1" == "-h" ]]; then
    echo "apply_patches_ext.sh [--one-cam | --dual-cam] JetPack_version [JetPack_source]"
    exit 1
fi

# Default to single camera DT for JetPack 5.0.2
# single - jp5 [default] single cam GMSL board
# dual - dual cam GMSL board SC20220126
JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
if [[ "$1" == "--one-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
    shift
fi
if [[ "$1" == "--dual-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-dual.dtsi"
    shift
fi

. scripts/setup-common

if [[ "$JETPACK_VERSION" == "5.x" ]]; then
    D4XX_SRC_DST=kernel/nvidia
else
    D4XX_SRC_DST=nvidia-oot
fi

TARGET="sources_${JP_INPUT_VERSION}"
[[ -n "$2" ]] && TARGET="$2"

apply_external_patches() {
    ls -Ld "${PWD}/$2/$1"
    ls -Lw1 "${PWD}/$2/$1"
    cat "${PWD}/$2/$1/"* | patch --quiet -p1 --directory="${PWD}/$TARGET/$2"
}

apply_external_patches "$1" "${D4XX_SRC_DST}"
apply_external_patches "$1" "${KERNEL_DIR}"

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public" "$2"
elif [[ "$JETPACK_VERSION" != "7.x" ]]; then
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/platform/t19x/galen/kernel-dts" "$2"
fi

# For JP5 we override the i2c driver and ignore the previous that was created from patches
cp kernel/realsense/d4xx.c "$TARGET/${D4XX_SRC_DST}/drivers/media/i2c/"
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    # jp6 overlay
    cp hardware/realsense/tegra234-camera-d4xx-overlay*.dts "$TARGET/hardware/nvidia/t23x/nv-public/overlay/"
    # max96712 header
    cp nvidia-oot/max96712.h "$TARGET/nvidia-oot/include/media/"
    # max96717 header and source
    cp nvidia-oot/max96717.h "$TARGET/nvidia-oot/include/media/"
    cp nvidia-oot/max96717.c "$TARGET/nvidia-oot/drivers/media/i2c/"
    # max96724 tunnel-mode deserializer header and source
    cp nvidia-oot/max96724.h "$TARGET/nvidia-oot/include/media/"
    cp nvidia-oot/max96724.c "$TARGET/nvidia-oot/drivers/media/i2c/"
elif [[ "$JETPACK_VERSION" == "5.x" ]]; then
    cp "hardware/realsense/${JP5_D4XX_DTSI}" "$TARGET/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
    # max96712 header
    cp kernel/nvidia/max96712.h "$TARGET/kernel/nvidia/include/media/"
    # MAX96717/MAX96724 use the same implementation on JP5 and JP6.
    cp nvidia-oot/max96717.h "$TARGET/kernel/nvidia/include/media/"
    cp nvidia-oot/max96717.c "$TARGET/kernel/nvidia/drivers/media/i2c/"
    cp nvidia-oot/max96724.h "$TARGET/kernel/nvidia/include/media/"
    cp nvidia-oot/max96724.c "$TARGET/kernel/nvidia/drivers/media/i2c/"
fi

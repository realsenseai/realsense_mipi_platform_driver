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

# set JP4 devicetree
if [[ "$JETPACK_VERSION" == "4.6.1" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx.dtsi"
fi
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    D4XX_SRC_DST=nvidia-oot
else
    D4XX_SRC_DST=kernel/nvidia
fi

TARGET="sources_${JETPACK_VERSION}"
[[ -n "$2" ]] && TARGET="$2"

# NVIDIA SDK Manager's JetPack 4.6.1 source_sync.sh doesn't set the right folder name, it mismatches with the direct tar
# package source code. Correct the folder name.
if [ -d "$TARGET/hardware/nvidia/platform/t19x/galen-industrial-dts" ]; then
    mv "$TARGET/hardware/nvidia/platform/t19x/galen-industrial-dts" "$TARGET/hardware/nvidia/platform/t19x/galen-industrial"
fi

apply_external_patches() {
    ls -Ld "${PWD}/$2/$1"
    ls -Lw1 "${PWD}/$2/$1"
    cat "${PWD}/$2/$1/"* | patch --quiet -p1 --directory="${PWD}/$TARGET/$2"
}

apply_external_patches "$1" "${D4XX_SRC_DST}"
apply_external_patches "$1" "${KERNEL_DIR}"

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public" "$2"
else
    apply_external_patches "$1" "hardware/nvidia/platform/t19x/galen/kernel-dts" "$2"
fi

# For a common driver for JP4 + JP5 we override the i2c driver and ignore the previous that was created from patches
cp kernel/realsense/d4xx.c "$TARGET/${D4XX_SRC_DST}/drivers/media/i2c/"
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    # jp6 overlay
    cp hardware/realsense/tegra234-camera-d4xx-overlay*.dts "$TARGET/hardware/nvidia/t23x/nv-public/overlay/"
else
    cp "hardware/realsense/${JP5_D4XX_DTSI}" "$TARGET/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
fi

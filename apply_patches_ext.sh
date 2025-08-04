#!/bin/bash
# Jetson Linux
# JP6.0 https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v2.0/sources/public_sources.tbz2
set -e

if [[ $# < 1 ]]; then
    echo "apply_patches_ext.sh [--one-cam | --dual-cam] [JetPack_version] [JetPack_source]"
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

DEVDIR=$(cd `dirname $0` && pwd)

. $DEVDIR/scripts/setup-common "$1"

cd "$DEVDIR"

# set JP4 devicetree
if [[ "$JETPACK_VERSION" == "4.6.1" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx.dtsi"
fi
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    D4XX_SRC_DST=nvidia-oot
else
    D4XX_SRC_DST=kernel/nvidia
fi
# NVIDIA SDK Manager's JetPack 4.6.1 source_sync.sh doesn't set the right folder name, it mismatches with the direct tar
# package source code. Correct the folder name.
if [ -d $JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts ]; then
    mv $JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts $JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial
fi

apply_external_patches() {
    TARGET=sources_$JETPACK_VERSION
    [[ -n $3 ]] && TARGET=$3
    git -C $PWD/$TARGET/$2 status > /dev/null
    if ! git -C $PWD/$TARGET/$2 diff --quiet || ! git -C $PWD/$TARGET/$2 diff --cached --quiet; then
	read -p "Repo $PWD/$TARGET/$2 has changes that may disturb applying patches. Continue (y/N)? " confirm
	if [[ ! "$confirm" == "y" && ! "$confirm" == "Y" ]]; then
	    exit 1
	fi
    fi
    ls -Ld ${PWD}/$2/$1
    ls -Lw1 ${PWD}/$2/$1
    cat ${PWD}/$2/$1/* | patch --quiet -p1 --directory=${PWD}/$TARGET/$2
}

apply_external_patches $1 $D4XX_SRC_DST $2

if [ -d sources_$JETPACK_VERSION/$KERNEL_DIR ]; then
    apply_external_patches $1 $KERNEL_DIR $2
fi

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches $JETPACK_VERSION hardware/nvidia/t23x/nv-public $2
else
    apply_external_patches $1 hardware/nvidia/platform/t19x/galen/kernel-dts $2
fi

# For a common driver for JP4 + JP5 we override the i2c driver and ignore the previous that was created from patches
cp $DEVDIR/kernel/realsense/d4xx.c $DEVDIR/sources_$JETPACK_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    # jp6 overlay
    cp $DEVDIR/hardware/realsense/tegra234-camera-d4xx-overlay*.dts $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/t23x/nv-public/overlay/
else
    cp $DEVDIR/hardware/realsense/$JP5_D4XX_DTSI $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi
fi

#!/bin/bash

set -e

if [[ $# < 1 ]]; then
    echo "apply_patches.sh [--one-cam | --dual-cam] apply [JetPack_version]"
    echo "apply_patches.sh reset [JetPack_version]"
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

. $DEVDIR/scripts/setup-common "$2"

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
if [[ $1 == apply && -d sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts ]]; then
    mv sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial
fi
if [[ $1 == reset && -d sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial ]]; then
    rm -rfv sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial > /dev/null
fi

apply_external_patches() {
    git -C sources_$JETPACK_VERSION/$3 status > /dev/null
    if [ $1 = 'apply' ]; then
        if ! git -C sources_$JETPACK_VERSION/$3 diff --quiet || ! git -C sources_$JETPACK_VERSION/$3 diff --cached --quiet; then
	    read -p "Repo sources_$JETPACK_VERSION/$3 has changes that may disturb applying patches. Continue (y/N)? " confirm
            if [[ ! "$confirm" == "y" && ! "$confirm" == "Y" ]]; then
                exit 1
            fi
        fi
        ls -Ld ${PWD}/$3/$2
        ls -Lw1 ${PWD}/$3/$2
        git -C sources_$JETPACK_VERSION/$3 apply ${PWD}/$3/$2/*
    elif [ $1 = 'reset' ]; then
        if ! git -C sources_$JETPACK_VERSION/$3 diff --quiet || ! git -C sources_$JETPACK_VERSION/$3 diff --cached --quiet; then
            read -p "Repo sources_$JETPACK_VERSION/$3 has changes that will be hard reset. Continue (y/N)? " confirm
            if [[ ! "$confirm" == "y" && ! "$confirm" == "Y" ]]; then
                exit 1
            fi
        fi
        echo -n $(ls -d sources_$JETPACK_VERSION/$3):\ 
            git -C sources_$JETPACK_VERSION/$3 reset --hard $4
    fi
}

apply_external_patches $1 $2 $D4XX_SRC_DST $L4T_VERSION

if [ -d sources_$JETPACK_VERSION/$KERNEL_DIR ]; then
    apply_external_patches $1 $2 $KERNEL_DIR $L4T_VERSION
fi

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches $1 $JETPACK_VERSION hardware/nvidia/t23x/nv-public $L4T_VERSION
else
    apply_external_patches $1 $2 hardware/nvidia/platform/t19x/galen/kernel-dts $L4T_VERSION
fi

if [ $1 = 'apply' ]; then
    cp $DEVDIR/kernel/realsense/d4xx.c $DEVDIR/sources_$JETPACK_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/
    if [[ "$JETPACK_VERSION" == "6.x" ]]; then
        # jp6 overlay
        cp $DEVDIR/hardware/realsense/tegra234-camera-d4xx-overlay*.dts $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/t23x/nv-public/overlay/
    else
        cp $DEVDIR/hardware/realsense/$JP5_D4XX_DTSI $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi
    fi
elif [ $1 = 'reset' ]; then
    [[ -f $DEVDIR/sources_$JETPACK_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/d4xx.c ]] && rm $DEVDIR/sources_$JETPACK_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/d4xx.c
    [[ -f $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi ]] && rm $DEVDIR/sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi
    true
fi

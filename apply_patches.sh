#!/bin/bash

set -e

if [[ $# < 1 ]]; then
    echo "apply_patches.sh [--one-cam | --dual-cam] JetPack_version [apply]"
    echo "apply_patches.sh JetPack_version reset"
    exit 1
fi

# Default to single camera DT for JetPack 5.0.2
# single - jp5 [default] single cam GMSL board
# dual - dual cam GMSL board SC20220126
JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
if [[ "$1" == "--one-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
    shift
elif [[ "$1" == "--dual-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-dual.dtsi"
    shift
fi

. scripts/setup-common "$1"

ACTION="$2"
[[ -z "$ACTION" ]] && ACTION="apply"

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
if [[ "$ACTION" == apply && -d "sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts" ]]; then
    mv sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial
fi
if [[ "$ACTION" == reset && -d "sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial" ]]; then
    rm -rfv "sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen-industrial" > /dev/null
fi

apply_external_patches() {
    git -C "sources_$JETPACK_VERSION/$3" status > /dev/null
    if [[ "$1" == 'apply' ]]; then
        if ! git -C "sources_$JETPACK_VERSION/$3" diff --quiet || ! git -C "sources_$JETPACK_VERSION/$3" diff --cached --quiet; then
	    read -p "Repo sources_$JETPACK_VERSION/$3 has changes that may disturb applying patches. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        ls -Ld "${PWD}/$3/$2"
        ls -Lw1 "${PWD}/$3/$2"
        git -C "sources_$JETPACK_VERSION/$3" apply "${PWD}/$3/$2"/*
    elif [ "$1" = "reset" ]; then
        if ! git -C "sources_$JETPACK_VERSION/$3" diff --quiet || ! git -C "sources_$JETPACK_VERSION/$3" diff --cached --quiet; then
            read -p "Repo sources_$JETPACK_VERSION/$3 has changes that will be hard reset. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        echo -n "$(ls -d "sources_$JETPACK_VERSION/$3"): "
        git -C "sources_$JETPACK_VERSION/$3" reset --hard $4
    fi
}

apply_external_patches "$ACTION" "$1" "$D4XX_SRC_DST" "$L4T_VERSION"

[[ -d "sources_$JETPACK_VERSION/$KERNEL_DIR" ]] && apply_external_patches "$ACTION" "$1" "$KERNEL_DIR" "$L4T_VERSION"

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches "$ACTION" "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public" "$L4T_VERSION"
else
    apply_external_patches "$ACTION" "$1" "hardware/nvidia/platform/t19x/galen/kernel-dts" "$L4T_VERSION"
fi

if [[ "$ACTION" = "apply" ]]; then
    cp -i kernel/realsense/d4xx.c "sources_$JETPACK_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/"
    if [[ "$JETPACK_VERSION" == "6.x" ]]; then
        # jp6 overlay
        cp hardware/realsense/tegra234-camera-d4xx-overlay*.dts "sources_$JETPACK_VERSION/hardware/nvidia/t23x/nv-public/overlay/"
    else
        cp "hardware/realsense/${JP5_D4XX_DTSI}" "sources_$JETPACK_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
    fi
fi

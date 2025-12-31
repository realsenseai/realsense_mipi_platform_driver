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
D4XX_SRC="d4xx.c"
if [[ "$1" == "--one-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
    shift
elif [[ "$1" == "--dual-cam" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-dual.dtsi"
    shift
elif [[ "$1" == "--fg12-16ch" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch.dtsi"
    D4XX_SRC="d4xx_max96712.c"
    shift
elif [[ "$1" == "--fg12-16ch-dual" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch-dual.dtsi"
    D4XX_SRC="d4xx_max96712.c"
    shift
fi

. scripts/setup-common "$1"

# Determine which sources directory exists (specific version like 6.0 or normalized like 6.x)
if [[ -d "sources_$1" ]]; then
    SOURCES_VERSION="$1"
elif [[ -d "sources_$JETPACK_VERSION" ]]; then
    SOURCES_VERSION="$JETPACK_VERSION"
else
    SOURCES_VERSION="$1"  # Default to original input if neither exists yet
fi

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
if [[ "$ACTION" == apply && -d "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts" ]]; then
    mv sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen-industrial-dts sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen-industrial
fi
if [[ "$ACTION" == reset && -d "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen-industrial" ]]; then
    rm -rfv "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen-industrial" > /dev/null
fi

# Create nvethernetrm symlink for JP 6.x (moved from source_sync_6.x.sh)
# JP 5.x handles nvethernetrm differently (full path clone, not a symlink)
# Must remove the directory first since git reset restores it as a real directory
# and ln -sf cannot replace a directory with a symlink
if [[ "$JETPACK_VERSION" =~ ^6\. ]]; then
    if [[ "$ACTION" == reset ]] || [[ "$ACTION" == apply ]]; then
        rm -rf "sources_$SOURCES_VERSION/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
        ln -sf ../../../../../../nvethernetrm "sources_$SOURCES_VERSION/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
    fi
fi

apply_external_patches() {
    git -C "sources_$SOURCES_VERSION/$3" status > /dev/null
    if [[ "$1" == 'apply' ]]; then
        if ! git -C "sources_$SOURCES_VERSION/$3" diff --quiet || ! git -C "sources_$SOURCES_VERSION/$3" diff --cached --quiet; then
	    read -p "Repo sources_$SOURCES_VERSION/$3 has changes that may disturb applying patches. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        ls -Ld "${PWD}/$3/$2"
        ls -Lw1 "${PWD}/$3/$2"
        # Store the original commit hash before applying patches
        ORIGINAL_COMMIT=$(git -C "sources_$SOURCES_VERSION/$3" rev-parse HEAD)
        echo "$ORIGINAL_COMMIT" > "sources_$SOURCES_VERSION/$3/.realsense_patch_base"
        git -C "sources_$SOURCES_VERSION/$3" apply "${PWD}/$3/$2"/*
    elif [ "$1" = "reset" ]; then
        if ! git -C "sources_$SOURCES_VERSION/$3" diff --quiet || ! git -C "sources_$SOURCES_VERSION/$3" diff --cached --quiet; then
            read -p "Repo sources_$SOURCES_VERSION/$3 has changes that will be hard reset. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        echo -n "$(ls -d "sources_$SOURCES_VERSION/$3"): "
        # Reset to original commit if .realsense_patch_base exists, otherwise use L4T_VERSION
        if [[ -f "sources_$SOURCES_VERSION/$3/.realsense_patch_base" ]]; then
            RESET_TARGET=$(cat "sources_$SOURCES_VERSION/$3/.realsense_patch_base")
            git -C "sources_$SOURCES_VERSION/$3" reset --hard "$RESET_TARGET"
            rm -f "sources_$SOURCES_VERSION/$3/.realsense_patch_base"
        else
            git -C "sources_$SOURCES_VERSION/$3" reset --hard $4
        fi
    fi
}

apply_external_patches "$ACTION" "$1" "$D4XX_SRC_DST" "$L4T_VERSION"

[[ -d "sources_$SOURCES_VERSION/$KERNEL_DIR" ]] && apply_external_patches "$ACTION" "$1" "$KERNEL_DIR" "$L4T_VERSION"

if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    apply_external_patches "$ACTION" "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public" "$L4T_VERSION"
else
    apply_external_patches "$ACTION" "$1" "hardware/nvidia/platform/t19x/galen/kernel-dts" "$L4T_VERSION"
fi

if [[ "$ACTION" = "apply" ]]; then
    cp -i kernel/realsense/d4xx.c "sources_$SOURCES_VERSION/${D4XX_SRC_DST}/drivers/media/i2c/"
    if [[ "$JETPACK_VERSION" == "6.x" ]]; then
        # jp6 overlay
        cp hardware/realsense/tegra234-camera-d4xx-overlay*.dts "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public/overlay/"
    else
        cp "hardware/realsense/${JP5_D4XX_DTSI}" "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
    fi
    
    # Stage all modified files after patching
    git -C "sources_$SOURCES_VERSION/$D4XX_SRC_DST" add -A
    [[ -d "sources_$SOURCES_VERSION/$KERNEL_DIR" ]] && git -C "sources_$SOURCES_VERSION/$KERNEL_DIR" add -A
    if [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" add -A
    elif [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" add -A
    fi
    
    # Get author identity from root repo
    GIT_AUTHOR_NAME=$(git config user.name)
    GIT_AUTHOR_EMAIL=$(git config user.email)
    
    # Update local git identity for subrepos
    git -C "sources_$SOURCES_VERSION/$D4XX_SRC_DST" config user.name "$GIT_AUTHOR_NAME"
    git -C "sources_$SOURCES_VERSION/$D4XX_SRC_DST" config user.email "$GIT_AUTHOR_EMAIL"
    if [[ -d "sources_$SOURCES_VERSION/$KERNEL_DIR" ]]; then
        git -C "sources_$SOURCES_VERSION/$KERNEL_DIR" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_$SOURCES_VERSION/$KERNEL_DIR" config user.email "$GIT_AUTHOR_EMAIL"
    fi
    if [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.email "$GIT_AUTHOR_EMAIL"
    elif [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" config user.email "$GIT_AUTHOR_EMAIL"
    fi

    # Commit all staged files
    git -C "sources_$SOURCES_VERSION/$D4XX_SRC_DST" commit -m "RS patched" || true
    [[ -d "sources_$SOURCES_VERSION/$KERNEL_DIR" ]] && git -C "sources_$SOURCES_VERSION/$KERNEL_DIR" commit -m "RS patched" || true
    if [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/t23x/nv-public" commit -m "RS patched" || true
    elif [[ -d "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_$SOURCES_VERSION/hardware/nvidia/platform/t19x/galen/kernel-dts" commit -m "RS patched" || true
    fi
fi

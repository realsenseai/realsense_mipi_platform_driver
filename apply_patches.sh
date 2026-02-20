#!/bin/bash

set -e

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
elif [[ "$1" == "--max96712-EVB" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-max96712-EVB.dtsi"
    shift
elif [[ "$1" == "--fg12-16ch" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch.dtsi"
    shift
elif [[ "$1" == "--fg12-16ch-dual" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch-dual.dtsi"
    shift
fi

ACTION="apply"
if [[ "$1" == reset ]]; then
	ACTION="reset"
	shift
fi

. scripts/setup-common "$1"

if [[ "$2" == reset ]]; then
	ACTION="reset"
fi

# set JP4 devicetree
if [[ "$JETPACK_VERSION" == "4.x" ]]; then
    JP5_D4XX_DTSI="tegra194-camera-d4xx.dtsi"
fi
if version_lt "$JETPACK_VERSION" "6.0"; then
    D4XX_SRC_DST=kernel/nvidia
else
    D4XX_SRC_DST=nvidia-oot
fi

# NVIDIA SDK Manager's JetPack 4.6.1 source_sync.sh doesn't set the right folder name, it mismatches with the direct tar
# package source code. Correct the folder name.
if [[ "$ACTION" == apply && -d "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen-industrial-dts" ]]; then
    mv sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen-industrial-dts sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen-industrial
fi
if [[ "$ACTION" == reset && -d "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen-industrial" ]]; then
    rm -rfv "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen-industrial" > /dev/null
fi

# Create nvethernetrm symlink for JP 6.x (moved from source_sync_6.x.sh)
# JP 5.x handles nvethernetrm differently (full path clone, not a symlink)
# Must remove the directory first since git reset restores it as a real directory
# and ln -sf cannot replace a directory with a symlink
if ! version_lt "$JETPACK_VERSION" 6.0; then
    if [[ "$ACTION" == reset ]] || [[ "$ACTION" == apply ]]; then
        rm -rf "sources_${JETPACK_VERSION}/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
        ln -sf ../../../../../../nvethernetrm "sources_${JETPACK_VERSION}/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
    fi
fi

apply_external_patches() {
    git -C "sources_${JETPACK_VERSION}/$2" status > /dev/null
    if [[ "$ACTION" == 'apply' ]]; then
        if ! git -C "sources_${JETPACK_VERSION}/$2" diff --quiet || ! git -C "sources_${JETPACK_VERSION}/$2" diff --cached --quiet; then
            read -p "Repo sources_${JETPACK_VERSION}/$2 has changes that may disturb applying patches. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
	echo -e "\e[33m$(ls -Ld ${PWD}/$2/$1)\e[0m"
        ls -Lw1 "${PWD}/$2/$1"
        git -C "sources_${JETPACK_VERSION}/$2" apply --verbose --reject "${PWD}/$2/$1"/*
    elif [[ "$ACTION" = "reset" ]]; then
        if ! git -C "sources_${JETPACK_VERSION}/$2" diff --quiet || ! git -C "sources_${JETPACK_VERSION}/$2" diff --cached --quiet; then
            read -p "Repo sources_${JETPACK_VERSION}/$2 has changes that will be hard reset. Continue (y/N)? " confirm
            [[ "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        echo -n "$(ls -d "sources_${JETPACK_VERSION}/$2"): "
		git -C "sources_${JETPACK_VERSION}/$2" reset --hard $L4T_VERSION
    fi
}

if [[ ! -d "sources_${JETPACK_VERSION}" ]]; then
	echo "Sources folder not found. Run ./setup_workspace.sh first"
	exit 2
fi

apply_external_patches "$version" "$D4XX_SRC_DST"
apply_external_patches "$version" "$KERNEL_DIR"

if version_lt "$JETPACK_VERSION" "6.0"; then
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/platform/t19x/galen/kernel-dts"
else
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public"
fi

if [[ "$ACTION" = "apply" ]]; then
    version_lt "$JETPACK_VERSION" "5.0" || cp -i kernel/realsense/d4xx.c "sources_${JETPACK_VERSION}/${D4XX_SRC_DST}/drivers/media/i2c/"
    if version_lt "$JETPACK_VERSION" "6.0"; then
        # device tree
        cp "hardware/realsense/${JP5_D4XX_DTSI}" "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
        # max96712 header
        cp kernel/nvidia/max96712.h "sources_${JETPACK_VERSION}/kernel/nvidia/include/media/"
    else
        # max96712 header
        cp nvidia-oot/max96712.h "sources_${JETPACK_VERSION}/nvidia-oot/include/media/"
        if version_lt "$JETPACK_VERSION" "7.0"; then
            # jp6 overlay
            cp hardware/realsense/tegra234-camera-d4xx-overlay*.dts "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public/overlay/"
        else
            cp sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public/include/platforms/dt-bindings/tegra234-p3737-0000+p3701-0000.h \
                    sources_${JETPACK_VERSION}/kernel/kernel-noble-src/include/dt-bindings/
            for dts in hardware/realsense/tegra234-camera-d4xx-overlay*.dts; do
                    # need to add o to file extension to meet kernel DT make rules
                    cp $dts "sources_${JETPACK_VERSION}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/$(basename ${dts})o"
            done
        fi
    fi

    # Stage all modified files after patching
    git -C "sources_${JETPACK_VERSION}/$D4XX_SRC_DST" add -A
    [[ -d "sources_${JETPACK_VERSION}/$KERNEL_DIR" ]] && git -C "sources_${JETPACK_VERSION}/$KERNEL_DIR" add -A
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" add -A
    fi
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" add -A
    fi

    # Get author identity from root repo
    if git config user.name > /dev/null; then
	    GIT_AUTHOR_NAME=$(git config user.name)
    else
            read -p "Enter your git user name: " GIT_AUTHOR_NAME
	    git config user.name "$GIT_AUTHOR_NAME"
    fi
    if git config user.email > /dev/null; then
	    GIT_AUTHOR_EMAIL=$(git config user.email)
    else
            read -p "Enter your git user e-mail: " GIT_AUTHOR_EMAIL
	    git config user.email "$GIT_AUTHOR_EMAIL"
    fi

    # Update local git identity for subrepos
    git -C "sources_${JETPACK_VERSION}/$D4XX_SRC_DST" config user.name "$GIT_AUTHOR_NAME"
    git -C "sources_${JETPACK_VERSION}/$D4XX_SRC_DST" config user.email "$GIT_AUTHOR_EMAIL"
    if [[ -d "sources_${JETPACK_VERSION}/$KERNEL_DIR" ]]; then
        git -C "sources_${JETPACK_VERSION}/$KERNEL_DIR" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_${JETPACK_VERSION}/$KERNEL_DIR" config user.email "$GIT_AUTHOR_EMAIL"
    fi
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.email "$GIT_AUTHOR_EMAIL"
	fi
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" config user.name "$GIT_AUTHOR_NAME"
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" config user.email "$GIT_AUTHOR_EMAIL"
    fi

    # Commit all staged files
    git -C "sources_${JETPACK_VERSION}/$D4XX_SRC_DST" commit -m "RS patched" || true
    [[ -d "sources_${JETPACK_VERSION}/$KERNEL_DIR" ]] && git -C "sources_${JETPACK_VERSION}/$KERNEL_DIR" commit -m "RS patched" || true
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/t23x/nv-public" commit -m "RS patched" || true
	fi
    if [[ -d "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts" commit -m "RS patched" || true
    fi
elif [[ "$ACTION" = "reset" ]]; then
    if version_lt "$JETPACK_VERSION" "5.0"; then
		rm "sources_${JETPACK_VERSION}/${D4XX_SRC_DST}/drivers/media/i2c/d4xx.c" || true
		rm "sources_${JETPACK_VERSION}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi" || true
	fi
fi

#!/bin/bash

set -e

ACTION="apply"
# Default to single camera DT for JetPack 5.0.2
# single - jp5 [default] single cam GMSL board
# dual - dual cam GMSL board SC20220126
JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
while [[ $# -gt 0 ]]; do
	if [[ "$1" == "--one-cam" ]]; then
		JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
	elif [[ "$1" == "--dual-cam" ]]; then
		JP5_D4XX_DTSI="tegra194-camera-d4xx-dual.dtsi"
	elif [[ "$1" == "--max96712-EVB" ]]; then
		JP5_D4XX_DTSI="tegra194-camera-d4xx-max96712-EVB.dtsi"
	elif [[ "$1" == "--fg12-16ch" ]]; then
		JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch.dtsi"
	elif [[ "$1" == "--fg12-16ch-dual" ]]; then
		JP5_D4XX_DTSI="tegra194-camera-d4xx-fg12-16ch-dual.dtsi"
	elif [[ "$1" == reset ]]; then
		ACTION="reset"
	elif [[ $1 == "-h" ]]; then
		echo Usage:
		echo "$0 [--one-cam | --dual-cam | --max96712-EVB | --fg12-16ch | --fg12-16ch-dual ] [reset] [-h]"
		echo -e 'reset\t: hard reset (git) to version from jetpack_version file'
		echo -e '-h\t: show this help'
		version=$1
		break
	else
		version=$1
	fi
	shift
done

. scripts/setup-common ${version}

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
if [[ "$ACTION" == apply && -d "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial-dts" ]]; then
	mv -T ${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial-dts ${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial
fi
if [[ "$ACTION" == reset && -d "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial" ]]; then
	rm -rfv "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial" > /dev/null
fi

# Create nvethernetrm symlink for JP 6.x (moved from source_sync_6.x.sh)
# JP 5.x handles nvethernetrm differently (full path clone, not a symlink)
# Must remove the directory first since git reset restores it as a real directory
# and ln -sf cannot replace a directory with a symlink
if ! version_lt "$JETPACK_VERSION" 6.0; then
	if [[ "$ACTION" == reset ]] || [[ "$ACTION" == apply ]]; then
		rm -rf "${BUILD_SRCS}/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
		ln -sf ../../../../../../nvethernetrm "${BUILD_SRCS}/nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm"
	fi
fi

apply_external_patches() {
	local source="${BUILD_SRCS}/$2"
	git -C "${source}" status > /dev/null
	if [[ "$ACTION" == 'apply' ]]; then
		if ! git -C "${source}" diff --quiet || ! git -C "${source}" diff --cached --quiet; then
			read -p "Repo ${source} has changes that may disturb applying patches. Continue (Y/n)? " confirm
			[[ -n "$confirm" && "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
		fi
		echo -e "\e[33m$(ls -Ld ${PWD}/$2/$1)\e[0m"
		ls -Lw1 "${PWD}/$2/$1"
		git -C "${source}" apply "${PWD}/$2/$1"/*
		# Update local git identity for subrepos
		git -C "${source}" config user.name > /dev/null || git -C "${source}" config user.name "$GIT_AUTHOR_NAME"
		git -C "${source}" config user.email > /dev/null || git -C "${source}" config user.email "$GIT_AUTHOR_EMAIL"
		git -C "${source}" add -u
		git -C "${source}" commit -m "RS patched"
	elif [[ "$ACTION" = "reset" ]]; then
		if ! git -C "${source}" diff --quiet || ! git -C "${source}" diff --cached --quiet; then
			read -p "Repo ${source} has changes that will be hard reset. Continue (Y/n)? " confirm
			[[ -n "$confirm" && "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
		fi
		echo -n "$(ls -d ${source}): "
		git -C "${source}" reset --hard $L4T_VERSION
	fi
}

if [[ ! -d "${BUILD_SRCS}" ]]; then
	echo "Sources folder not found. Run ./setup_workspace.sh first"
	exit 2
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

apply_external_patches "$JP_INPUT_VERSION" "$D4XX_SRC_DST"
apply_external_patches "$JP_INPUT_VERSION" "$KERNEL_DIR"

if version_lt "$JETPACK_VERSION" "6.0"; then
	apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/platform/t19x/galen/kernel-dts"
elif version_lt "$JETPACK_VERSION" "7.0"; then
	# from JP7 DT files are handled in kernel tree
	apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public"
fi

echo "Patches applied successfully"

if [[ "$ACTION" = "apply" ]]; then
	# for jetpack prior to 5.0, the d4xx driver source code is created from patches
	version_lt "$JETPACK_VERSION" "5.0" || ln -sfr "kernel/realsense/d4xx.c" "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/media/i2c/"
	if version_lt "$JETPACK_VERSION" "6.0"; then
		# device tree
		ln -sfr "hardware/realsense/${JP5_D4XX_DTSI}" "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
		# max96712 header
		ln -sfr "kernel/nvidia/max96712.h" "${BUILD_SRCS}/kernel/nvidia/include/media/"
	else
		# max96712 header
		ln -sfr "nvidia-oot/max96712.h" "${BUILD_SRCS}/nvidia-oot/include/media/"
		if version_lt "$JETPACK_VERSION" "7.0"; then
			# jp6 overlay
			ln -sfr hardware/realsense/tegra234-camera-d4xx-overlay*.dts "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/overlay/"
			ln -sfr "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/include/platforms/dt-bindings/tegra234-p3737-0000+p3701-0000.h" \
				"${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/"
		else
			# link tegra264-gpio.h for Thor overlay compilation if not already present
			if [[ ! -f "${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/gpio/tegra264-gpio.h" ]]; then
				ln -sfr "${BUILD_SRCS}/$KERNEL_DIR/3rdparty/canonical/linux-noble/include/dt-bindings/gpio/tegra264-gpio.h" \
					"${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/gpio/" 2>/dev/null || true
			fi
			ln -sfr hardware/realsense/tegra234-camera-d4xx-overlay*.dts "${BUILD_SRCS}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/"
			ln -sfr hardware/realsense/tegra264-camera-d4xx-overlay*.dtso "${BUILD_SRCS}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/"
			ln -sfr ${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/nv-platform/tegra234-*dts "${BUILD_SRCS}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/"
			ln -sfr ${BUILD_SRCS}/hardware/nvidia/t264/nv-public/nv-platform/tegra264-*dts "${BUILD_SRCS}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/"
		fi
	fi
elif [[ "$ACTION" = "reset" ]]; then
    if version_lt "$JP_INPUT_VERSION" "5.0"; then
        rm "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/media/i2c/d4xx.c" || true
        rm "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi" || true
    fi
fi

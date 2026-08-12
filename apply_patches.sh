#!/bin/bash

set -e

ACTION="apply"
SOURCE_OVERLAY_MODE="${RS_SOURCE_OVERLAY_MODE:-link}"
# D5xx-only patches touch NVIDIA code shared with D4xx, so they stay opt-in.
USE_D5XX=0
[[ "${RS_USE_D5XX^^}" == "ON" || "${RS_USE_D5XX}" == "1" ]] && USE_D5XX=1
# Default to single camera DT for JetPack 5.0.2
# single - jp5 [default] single cam GMSL board
# dual - dual cam GMSL board SC20220126
JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--one-cam" ]]; then
        JP5_D4XX_DTSI="tegra194-camera-d4xx-single.dtsi"
    elif [[ "$1" == "--dual-cam" ]]; then
        JP5_D4XX_DTSI="tegra194-camera-d4xx-dual.dtsi"
    elif [[ "$1" == "--d5xx" ]]; then
        USE_D5XX=1
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
        echo "$0 [--one-cam | --dual-cam | --d5xx | --max96712-EVB | --fg12-16ch | --fg12-16ch-dual ] [reset] [-h]"
        echo -e '--d5xx\t: also apply D5xx-only patches (or set RS_USE_D5XX=ON)'
        echo -e 'reset\t: hard reset (git) to version from jetpack_version file'
        echo -e '-h\t: show this help'
        exit 0
    else break
    fi
    shift
done

. scripts/setup-common

case "$SOURCE_OVERLAY_MODE" in
    copy|link) ;;
    *)
        echo "ERROR: RS_SOURCE_OVERLAY_MODE must be 'copy' or 'link' (got: ${SOURCE_OVERLAY_MODE})"
        exit 2
        ;;
esac

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
    mv ${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial-dts ${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen-industrial
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

cleanup_reset_artifacts() {
    local source="$1"
    local -a reset_artifacts=(
        "drivers/media/i2c/d4xx.c"
        "drivers/media/i2c/d5xx.c"
        "drivers/media/i2c/max96717.c"
        "drivers/media/i2c/max96724.c"
        "include/media/max96712.h"
        "include/media/max96717.h"
        "include/media/max96724.h"
    )

    git -C "${source}" clean -f -- "${reset_artifacts[@]}" > /dev/null 2>&1 || true
}

copy_source_overlay() {
    local overlay_dir="$1"
    local source="$2"
    local git_dir manifest rel
    local -a overlay_paths

    [[ -d "${overlay_dir}" ]] || return 0

    mapfile -t overlay_paths < <(cd "${overlay_dir}" && find . \( -type f -o -type l \) -printf '%P\n' | sort)
    [[ ${#overlay_paths[@]} -gt 0 ]] || return 0

    git_dir="$(git -C "${source}" rev-parse --absolute-git-dir)"
    manifest="${git_dir}/rs-source-overlay.manifest"

    printf 'source overlay (%s): %s -> %s\n' "${SOURCE_OVERLAY_MODE}" "${overlay_dir}" "${source}"

    if [[ -f "${manifest}" ]]; then
        while IFS= read -r rel; do
            [[ -n "${rel}" ]] || continue
            if ! printf '%s\n' "${overlay_paths[@]}" | grep -Fxq -- "${rel}"; then
                if git -C "${source}" ls-files --error-unmatch -- "${rel}" > /dev/null 2>&1; then
                    printf 'ERROR: previous overlay path is now tracked by native source: %s\n' "${rel}" >&2
                    printf '       convert this file to a patch instead of overlay copy.\n' >&2
                    exit 1
                fi
                rm -f -- "${source}/${rel}"
            fi
        done < "${manifest}"
    fi

    for rel in "${overlay_paths[@]}"; do
        if git -C "${source}" ls-files --error-unmatch -- "${rel}" > /dev/null 2>&1; then
            printf 'ERROR: source overlay refuses to overwrite native tracked file: %s\n' "${rel}" >&2
            printf '       use a patch for existing kernel files instead of copying over them.\n' >&2
            exit 1
        fi
    done

    if [[ "$SOURCE_OVERLAY_MODE" == "link" ]]; then
        for rel in "${overlay_paths[@]}"; do
            mkdir -p "${source}/$(dirname "${rel}")"
            rm -f -- "${source}/${rel}"
            ln -sfr "${overlay_dir}/${rel}" "${source}/${rel}"
        done
    else
        cp -a "${overlay_dir}/." "${source}/"
    fi
    printf '%s\n' "${overlay_paths[@]}" > "${manifest}"
    git -C "${source}" add -A -- "${overlay_paths[@]}"
}

apply_external_patches() {
	local source="${BUILD_SRCS}/$2"
    git -C "${source}" status > /dev/null
    if [[ "$ACTION" == 'apply' ]]; then
        if ! git -C "${source}" diff --quiet || ! git -C "${source}" diff --cached --quiet; then
            read -p "Repo ${source} has changes that may disturb applying patches. Continue (Y/n)? " confirm || confirm=""
            [[ -n "$confirm" && "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        printf '%s\n' "$(ls -Ld ${PWD}/$2/$1)"
        ls -Lw1 "${PWD}/$2/$1"
        git -C "${source}" apply "${PWD}/$2/$1"/*
    elif [[ "$ACTION" = "reset" ]]; then
        if ! git -C "${source}" diff --quiet || ! git -C "${source}" diff --cached --quiet; then
            read -p "Repo ${source} has changes that will be hard reset. Continue (Y/n)? " confirm || confirm=""
            [[ -n "$confirm" && "$confirm" != "y" && "$confirm" != "Y" ]] && exit 1
        fi
        cleanup_reset_artifacts "${source}"
        echo -n "$(ls -d ${source}): "
        git -C "${source}" reset --hard $L4T_VERSION
    fi
}

apply_d5xx_only_patches() {
	local dir="${PWD}/$2/$1-d5xx"
    [[ "$ACTION" == 'apply' && "$USE_D5XX" == 1 && -d "$dir" ]] || return 0
    printf '%s\n' "$(ls -Ld "$dir")"
    ls -Lw1 "$dir"
    git -C "${BUILD_SRCS}/$2" apply "$dir"/*
}

if [[ ! -d "${BUILD_SRCS}" ]]; then
    echo "Sources folder not found. Run ./setup_workspace.sh first"
    exit 2
fi

apply_external_patches "$JP_INPUT_VERSION" "$D4XX_SRC_DST"
apply_d5xx_only_patches "$JP_INPUT_VERSION" "$D4XX_SRC_DST"
apply_external_patches "$JP_INPUT_VERSION" "$KERNEL_DIR"

if version_lt "$JETPACK_VERSION" "6.0"; then
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/platform/t19x/galen/kernel-dts"
elif version_lt "$JETPACK_VERSION" "7.0"; then
	# from JP7 DT files are handled in kernel tree
    apply_external_patches "$JETPACK_VERSION" "hardware/nvidia/t23x/nv-public"
fi

echo "Patches applied successfully"

if [[ "$ACTION" = "apply" ]]; then
    version_lt "$JETPACK_VERSION" "5.0" || ln -f -s "$(pwd)/kernel/realsense/d4xx.c" "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/media/i2c/"
    version_lt "$JETPACK_VERSION" "5.0" || ln -f -s "$(pwd)/kernel/realsense/d5xx.c" "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/media/i2c/"
    if version_lt "$JETPACK_VERSION" "6.0"; then
        # device tree
        cp "hardware/realsense/${JP5_D4XX_DTSI}" "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi"
        # max96712 header
        cp kernel/nvidia/max96712.h "${BUILD_SRCS}/kernel/nvidia/include/media/"
    else
        copy_source_overlay "$(pwd)/nvidia-oot/files/${JP_INPUT_VERSION}" "${BUILD_SRCS}/nvidia-oot"
        # max96712 header
        ln -f nvidia-oot/max96712.h "${BUILD_SRCS}/nvidia-oot/include/media/"
        if version_lt "$JETPACK_VERSION" "7.0"; then
            # jp6 overlay
            ln -f hardware/realsense/tegra234-camera-d4xx-overlay*.dts "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/overlay/"
            ln -f hardware/realsense/tegra234-camera-d5xx-overlay*.dts "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/overlay/"
            JP6_OVERLAY_MAKEFILE="${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/overlay/Makefile"
            if [[ -f "$JP6_OVERLAY_MAKEFILE" ]] && ! grep -q '^dtbo-y += tegra234-camera-d5xx-overlay.dtbo$' "$JP6_OVERLAY_MAKEFILE"; then
                sed -i '/^dtbo-y += tegra234-camera-d4xx-overlay.dtbo$/a dtbo-y += tegra234-camera-d5xx-overlay.dtbo' "$JP6_OVERLAY_MAKEFILE"
            fi
            if [[ -f "$JP6_OVERLAY_MAKEFILE" ]] && ! grep -q '^dtbo-y += tegra234-camera-d5xx-overlay-fg24-4ch.dtbo$' "$JP6_OVERLAY_MAKEFILE"; then
                sed -i '/^dtbo-y += tegra234-camera-d5xx-overlay.dtbo$/a dtbo-y += tegra234-camera-d5xx-overlay-fg24-4ch.dtbo' "$JP6_OVERLAY_MAKEFILE"
            fi
            if [[ -f "$JP6_OVERLAY_MAKEFILE" ]] && ! grep -q '^dtbo-y += tegra234-camera-d5xx-overlay-fg24-5ch-dual-rgb.dtbo$' "$JP6_OVERLAY_MAKEFILE"; then
                sed -i '/^dtbo-y += tegra234-camera-d5xx-overlay-fg24-4ch.dtbo$/a dtbo-y += tegra234-camera-d5xx-overlay-fg24-5ch-dual-rgb.dtbo' "$JP6_OVERLAY_MAKEFILE"
            fi
            ln -f ${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/include/platforms/dt-bindings/tegra234-p3737-0000+p3701-0000.h \
                    ${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/
            ln -f ${BUILD_SRCS}/hardware/nvidia/t23x/nv-public/include/platforms/dt-bindings/tegra234-p3767-0000-common.h \
                    ${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/
        else
            # Copy tegra264-gpio.h for Thor overlay compilation if not already present
            if [[ ! -f "${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/gpio/tegra264-gpio.h" ]]; then
                ln -f "${BUILD_SRCS}/$KERNEL_DIR/3rdparty/canonical/linux-noble/include/dt-bindings/gpio/tegra264-gpio.h" \
                    "${BUILD_SRCS}/$KERNEL_DIR/include/dt-bindings/gpio/" 2>/dev/null || true
            fi
            ln -f hardware/realsense/tegra264-camera-d4xx-overlay*.dtso "${BUILD_SRCS}/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/"
        fi
    fi

    # Stage all modified files after patching
    if ! version_lt "$JETPACK_VERSION" "5.0"; then
        git -C "${BUILD_SRCS}/$D4XX_SRC_DST" add drivers/media/i2c/d4xx.c drivers/media/i2c/d5xx.c
    fi
    if ! version_lt "$JETPACK_VERSION" "6.0"; then
        [[ -e "${BUILD_SRCS}/${D4XX_SRC_DST}/include/media/max96712.h" ]] && \
            git -C "${BUILD_SRCS}/$D4XX_SRC_DST" add include/media/max96712.h
        [[ -e "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm" || \
            -L "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm" ]] && \
            git -C "${BUILD_SRCS}/$D4XX_SRC_DST" add -Af drivers/net/ethernet/nvidia/nvethernet/nvethernetrm
    fi
    git -C "${BUILD_SRCS}/$D4XX_SRC_DST" add -u
    [[ -d "${BUILD_SRCS}/$KERNEL_DIR" ]] && git -C "${BUILD_SRCS}/$KERNEL_DIR" add -A
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" add -A
    fi
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" add -A
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
    git -C "${BUILD_SRCS}/$D4XX_SRC_DST" config user.name "$GIT_AUTHOR_NAME"
    git -C "${BUILD_SRCS}/$D4XX_SRC_DST" config user.email "$GIT_AUTHOR_EMAIL"
    if [[ -d "${BUILD_SRCS}/$KERNEL_DIR" ]]; then
        git -C "${BUILD_SRCS}/$KERNEL_DIR" config user.name "$GIT_AUTHOR_NAME"
        git -C "${BUILD_SRCS}/$KERNEL_DIR" config user.email "$GIT_AUTHOR_EMAIL"
    fi
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.name "$GIT_AUTHOR_NAME"
        git -C "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" config user.email "$GIT_AUTHOR_EMAIL"
    fi
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" config user.name "$GIT_AUTHOR_NAME"
        git -C "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" config user.email "$GIT_AUTHOR_EMAIL"
    fi

    # Commit all staged files
    git -C "${BUILD_SRCS}/$D4XX_SRC_DST" commit -m "RS patched" || true
    [[ -d "${BUILD_SRCS}/$KERNEL_DIR" ]] && git -C "${BUILD_SRCS}/$KERNEL_DIR" commit -m "RS patched" || true
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/t23x/nv-public" commit -m "RS patched" || true
    fi
    if [[ -d "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" ]]; then
        git -C "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts" commit -m "RS patched" || true
    fi
elif [[ "$ACTION" = "reset" ]]; then
    if version_lt "$JP_INPUT_VERSION" "5.0"; then
        rm "${BUILD_SRCS}/${D4XX_SRC_DST}/drivers/media/i2c/d4xx.c" || true
        rm "${BUILD_SRCS}/hardware/nvidia/platform/t19x/galen/kernel-dts/common/tegra194-camera-d4xx.dtsi" || true
    fi
fi

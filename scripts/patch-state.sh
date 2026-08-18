#!/usr/bin/env bash

set -euo pipefail

ACTION=${1:?record or verify is required}
REPO_ROOT=${2:?repository root is required}
BUILD_ROOT=${3:?generated source root is required}
JP_INPUT_VERSION=${4:?JetPack version is required}
JP_FAMILY=${5:?JetPack family is required}
KERNEL_DIR=${6:?kernel source directory is required}
DRIVER_SRC_DST=${7:?driver source directory is required}
STATE_FILE="$BUILD_ROOT/.rs-patch-state"

declare -a PATCH_MAP=()
PATCH_MAP+=("$REPO_ROOT/$DRIVER_SRC_DST/$JP_INPUT_VERSION|$BUILD_ROOT/$DRIVER_SRC_DST")
PATCH_MAP+=("$REPO_ROOT/$KERNEL_DIR/$JP_INPUT_VERSION|$BUILD_ROOT/$KERNEL_DIR")

case "$JP_FAMILY" in
	5.x)
		PATCH_MAP+=("$REPO_ROOT/hardware/nvidia/platform/t19x/galen/kernel-dts/$JP_FAMILY|$BUILD_ROOT/hardware/nvidia/platform/t19x/galen/kernel-dts")
		;;
	6.x)
		PATCH_MAP+=("$REPO_ROOT/hardware/nvidia/t23x/nv-public/$JP_FAMILY|$BUILD_ROOT/hardware/nvidia/t23x/nv-public")
		;;
esac

hash_file()
{
	local root=$1
	local file=$2

	if [[ -f "$file" ]]; then
		printf 'file=%s\n' "${file#$root/}"
		sha256sum "$file" | awk '{print $1}'
	else
		printf 'missing=%s\n' "${file#$root/}"
	fi
}

hash_patch_inputs()
{
	local mapping patch_dir patch file
	local -a shared_inputs=(
		"$REPO_ROOT/apply_patches.sh"
		"$REPO_ROOT/apply_patches_ext.sh"
		"$REPO_ROOT/build_all.sh"
		"$REPO_ROOT/scripts/patch-state.sh"
		"$REPO_ROOT/scripts/setup-common"
		"$REPO_ROOT/kernel/realsense/d4xx.c"
		"$REPO_ROOT/kernel/nvidia/max96712.h"
		"$REPO_ROOT/nvidia-oot/max96712.h"
		"$REPO_ROOT/nvidia-oot/max96717.c"
		"$REPO_ROOT/nvidia-oot/max96717.h"
		"$REPO_ROOT/nvidia-oot/max96724.c"
		"$REPO_ROOT/nvidia-oot/max96724.h"
	)

	{
		printf 'jp=%s\nfamily=%s\n' "$JP_INPUT_VERSION" "$JP_FAMILY"
		for mapping in "${PATCH_MAP[@]}"; do
			patch_dir=${mapping%%|*}
			if [[ ! -d "$patch_dir" ]]; then
				printf 'missing-patch-dir=%s\n' "${patch_dir#$REPO_ROOT/}"
				continue
			fi
			while IFS= read -r patch; do
				hash_file "$REPO_ROOT" "$patch"
			done < <(find -L "$patch_dir" -maxdepth 1 -type f -name '*.patch' | sort)
		done

		if [[ -d "$REPO_ROOT/hardware/realsense" ]]; then
			while IFS= read -r file; do
				hash_file "$REPO_ROOT" "$file"
			done < <(
				find "$REPO_ROOT/hardware/realsense" -maxdepth 1 -type f \
					\( -name '*.dts' -o -name '*.dtsi' -o -name '*.dtso' \) | sort
			)
		fi

		for file in "${shared_inputs[@]}"; do
			hash_file "$REPO_ROOT" "$file"
		done
	} | sha256sum | awk '{print $1}'
}

hash_generated_sources()
{
	local mapping patch_dir source_dir path file
	local -a staged_sources=(
		"$BUILD_ROOT/$DRIVER_SRC_DST/drivers/media/i2c/d4xx.c"
		"$BUILD_ROOT/$DRIVER_SRC_DST/drivers/media/i2c/max96717.c"
		"$BUILD_ROOT/$DRIVER_SRC_DST/drivers/media/i2c/max96724.c"
		"$BUILD_ROOT/$DRIVER_SRC_DST/include/media/max96712.h"
		"$BUILD_ROOT/$DRIVER_SRC_DST/include/media/max96717.h"
		"$BUILD_ROOT/$DRIVER_SRC_DST/include/media/max96724.h"
	)

	{
		for mapping in "${PATCH_MAP[@]}"; do
			patch_dir=${mapping%%|*}
			source_dir=${mapping#*|}
			if [[ ! -d "$patch_dir" ]]; then
				printf 'missing-patch-dir=%s\n' "${patch_dir#$REPO_ROOT/}"
				continue
			fi
			while IFS= read -r path; do
				[[ -n "$path" ]] && hash_file "$BUILD_ROOT" "$source_dir/$path"
			done < <(
				find -L "$patch_dir" -maxdepth 1 -type f -name '*.patch' -print0 |
					sort -z |
					xargs -0 -r sed -n 's|^+++ b/||p' |
					grep -v '^/dev/null$' | sort -u
			)
		done

		for file in "${staged_sources[@]}"; do
			hash_file "$BUILD_ROOT" "$file"
		done
	} | sha256sum | awk '{print $1}'
}

read_state_value()
{
	local key=$1
	awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$STATE_FILE"
}

current_commit=$(git -C "$REPO_ROOT" rev-parse HEAD)
current_inputs_sha=$(hash_patch_inputs)
current_generated_sha=$(hash_generated_sources)

case "$ACTION" in
	record)
		tmp_state="$STATE_FILE.tmp.$$"
		{
			printf 'schema=1\n'
			printf 'jp=%s\n' "$JP_INPUT_VERSION"
			printf 'top_commit=%s\n' "$current_commit"
			printf 'inputs_sha=%s\n' "$current_inputs_sha"
			printf 'generated_sha=%s\n' "$current_generated_sha"
		} > "$tmp_state"
		mv "$tmp_state" "$STATE_FILE"
		printf 'Recorded patch state: %s\n' "$STATE_FILE"
		;;
	verify)
		if [[ ! -f "$STATE_FILE" ]]; then
			printf 'ERROR: generated sources have no patch-state record: %s\n' "$STATE_FILE" >&2
			printf 'Run: ./apply_patches.sh reset && ./apply_patches.sh\n' >&2
			exit 1
		fi
		if [[ "$(read_state_value schema)" != "1" ||
		      "$(read_state_value jp)" != "$JP_INPUT_VERSION" ||
		      "$(read_state_value top_commit)" != "$current_commit" ||
		      "$(read_state_value inputs_sha)" != "$current_inputs_sha" ||
		      "$(read_state_value generated_sha)" != "$current_generated_sha" ]]; then
			printf 'ERROR: generated sources do not match the current branch or patch inputs.\n' >&2
			printf 'Run: ./apply_patches.sh reset && ./apply_patches.sh\n' >&2
			exit 1
		fi
		printf 'Patch state verified for JetPack %s.\n' "$JP_INPUT_VERSION"
		;;
	*)
		printf 'ERROR: unsupported patch-state action: %s\n' "$ACTION" >&2
		exit 2
		;;
esac

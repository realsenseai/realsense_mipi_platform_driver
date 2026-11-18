#!/bin/bash

set -e

function DisplayNvidiaLicense {

    # verify that curl is installed
    if  ! which curl > /dev/null  ; then
      echo "curl is not installed."
      echo "curl can be installed by 'sudo apt-get install curl'."
      exit 1
    fi

    local release;
    IFS='.' read -a release <<< "$1"

    RELEASE="r${release[0]}_Release_v${release[1]}.${release[2]:-0}"

    local URL="https://developer.download.nvidia.com/embedded/L4T/${RELEASE}/$2/Tegra_Software_License_Agreement-Tegra-Linux.txt"

    echo -e "\nPlease notice: This script will download the kernel source (from nv-tegra, NVIDIA's public git repository) which is subject to the following license:\n${URL}\n"

    local LICENSE=$(curl -Ls ${URL})
    [[ -z $LICENSE || "$LICENSE" == "Not found" ]] && echo "License link not found" && exit 2

    ## display the page ##
    echo -e "${LICENSE}\n"

    read -n 1 -s -r -e -p $'\e[33mPress any key to ACCEPT and continue...\e[0m'
    echo
}

SOURCES_ARCHIVE=""
# Options are consumed before setup-common, which reads $1 as the JetPack version.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-archive)
            [[ -f "$2" ]] || { echo "--sources-archive: file not found: $2"; exit 1; }
            SOURCES_ARCHIVE="$(realpath "$2")"
            shift 2
            ;;
        -h)
            echo Usage:
            echo "$0 [--sources-archive <file>] [JetPack]"
            echo -e '--sources-archive\t: NVIDIA public_sources.tbz2, or a backup_sources_<JetPack>.tar.gz git-tree cache'
            break
            ;;
        *) break ;;
    esac
done

. scripts/setup-common

# Unpack an NVIDIA public_sources.tbz2 and make each patched dir its own git repo tagged
# $L4T_VERSION, so apply_patches.sh (git apply, reset) works on it as on an nv-tegra clone.
extract_public_sources() {
    local tmp root
    tmp="$(mktemp -d "${PWD}/.sources-archive.XXXXXX")"
    tar --no-same-owner -xf "$1" -C "$tmp"
    root="$tmp/Linux_for_Tegra/source"
    version_lt "$JETPACK_VERSION" "6.0" && root="$root/public"
    tar --no-same-owner -xf "$root/kernel_src.tbz2" -C "$root"
    if ! version_lt "$JETPACK_VERSION" "6.0"; then
        for bundle in kernel_oot_modules_src.tbz2 nvidia_kernel_display_driver_source.tbz2; do
            [[ -f "$root/$bundle" ]] && tar --no-same-owner -xf "$root/$bundle" -C "$root"
        done
    fi
    mv "$root" "${BUILD_SRCS}"
    rm -rf "$tmp"
    for repo in "${PATCHED_REPOS[@]}"; do
        local dir
        dir="$(realpath "${BUILD_SRCS}/$repo")"
        local g=(git -C "$dir" -c "safe.directory=$dir" -c user.name=setup_workspace -c user.email=setup@localhost -c init.defaultBranch=master)
        echo "Creating git baseline ${L4T_VERSION} in ${BUILD_SRCS}/$repo"
        "${g[@]}" init -q
        "${g[@]}" add -A
        "${g[@]}" commit -q --no-verify -m "L4T ${L4T_VERSION} baseline"
        "${g[@]}" tag "${L4T_VERSION}"
    done
}

echo "Setup JetPack ${JP_INPUT_VERSION} to ${BUILD_SRCS}"

# Display NVIDIA license
DisplayNvidiaLicense "${REVISION}" "${LICENSE}"

# Install L4T gcc if not installed
if [[ $(uname -m) == aarch64 ]]; then
    echo
    echo Native build
    echo
else
    if [[ ! -d "l4t-gcc/$JETPACK_VERSION/bin/" ]]; then
        echo "Installing build toolchain"
        mkdir -p "l4t-gcc/$JETPACK_VERSION"
        pushd l4t-gcc/$JETPACK_VERSION
        if [[ "$JETPACK_VERSION" == "7.x" ]]; then
            wget --quiet https://developer.nvidia.com/downloads/embedded/L4T/r38_Release_v2.0/release/x-tools.tbz2 -O x-tools.tbz2
            tar xf x-tools.tbz2 ./x-tools/aarch64-none-linux-gnu --strip-components 3
        elif [[ "$JETPACK_VERSION" == "6.x" ]]; then
            wget --quiet https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v3.0/toolchain/aarch64--glibc--stable-2022.08-1.tar.bz2 -O aarch64--glibc--stable-final.tar.bz2
            tar xf aarch64--glibc--stable-final.tar.bz2 --strip-components 1
        elif [[ "$JETPACK_VERSION" == "5.x" ]]; then
            wget --quiet https://developer.nvidia.com/embedded/jetson-linux/bootlin-toolchain-gcc-93 -O aarch64--glibc--stable-final.tar.gz
            tar xf aarch64--glibc--stable-final.tar.gz
        fi
        popd
    fi
fi

[[ -d ${BUILD_SRCS} ]] && echo -e "\e[33mIn a case you have local changes you may reset them with ./apply_patches.sh reset\e[0m\n"

if [[ -n "$SOURCES_ARCHIVE" && -d "${BUILD_SRCS}" ]]; then
    echo "${BUILD_SRCS} already exists; remove it before setting up from $SOURCES_ARCHIVE"
    exit 1
fi
# Without --sources-archive, a git-tree cache in /home/nvidia_sources_cache is used when present
: "${SOURCES_ARCHIVE:=/home/nvidia_sources_cache/backup_${BUILD_SRCS}.tar.gz}"

if [[ ! -d "${BUILD_SRCS}" && -f "$SOURCES_ARCHIVE" ]]; then
    echo "Extracting sources from $SOURCES_ARCHIVE instead of cloning from NVIDIA repository..."
    EXTRACTED_DIR=$(tar -tf "$SOURCES_ARCHIVE" | head -1 | cut -d'/' -f1)
    case "$EXTRACTED_DIR" in
        Linux_for_Tegra)
            extract_public_sources "$SOURCES_ARCHIVE"
            ;;
        sources_*)
            tar --no-same-owner -xf "$SOURCES_ARCHIVE"
            # The tar ball might contain sources_6.x instead of sources_6.0
            if [[ "$EXTRACTED_DIR" != "${BUILD_SRCS}" ]]; then
                echo "Renaming extracted directory from $EXTRACTED_DIR to ${BUILD_SRCS}..."
                mv "$EXTRACTED_DIR" "${BUILD_SRCS}"
            fi
            ;;
        *)
            echo "Unknown archive layout (top directory '$EXTRACTED_DIR'): expected Linux_for_Tegra/ or sources_*/"
            exit 1
            ;;
    esac
    echo -e "\e[32mSources extracted successfully from $SOURCES_ARCHIVE\e[0m"
else
    echo "Cloning sources from NVIDIA repository..."
    ./scripts/sync-sources.sh -t $L4T_VERSION -d ${BUILD_SRCS} -k
fi

# copy Makefile for jp6
if ! version_lt "$JETPACK_VERSION" "6.0"; then
    cp ./nvidia-oot/Makefile "${BUILD_SRCS}/"
    cp ./$KERNEL_DIR/Makefile "${BUILD_SRCS}/kernel/"
fi


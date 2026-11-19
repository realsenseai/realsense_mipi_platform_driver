#!/bin/bash
# Fetch everything the Qualcomm (Ubuntu 24.04 / RB3 Gen 2) build needs
# (RSDEV-15171).
#
#   ./setup_workspace_qcom.sh [series]
#
# Produces sources_qcom_<series>/{kernel,nv-oot} and a cross toolchain. Run
# ./apply_patches_qcom.sh next, then ./build_all_qcom.sh.

set -e

if [[ $1 == "-h" ]]; then
    echo "Usage: $0 [series]"
    . scripts/setup-common-qcom -h
    exit 0
fi

. scripts/setup-common-qcom "$1"

DEVDIR=$(cd "$(dirname "$0")" && pwd)
CACHE="${BUILD_SRCS}/.cache"

echo "Setup Ubuntu ${QCOM_INPUT_VERSION} to ${BUILD_SRCS}"

# --- toolchain ---------------------------------------------------------------
if [[ $(uname -m) == aarch64 ]]; then
    echo "Native aarch64 build - no cross toolchain needed"
elif [[ -n "$(qcom_cross_compile "${DEVDIR}")" ]]; then
    echo "Using existing toolchain: $(qcom_cross_compile "${DEVDIR}")"
else
    echo "Installing ARM GNU toolchain ${QCOM_TOOLCHAIN_VER} (aarch64-none-linux-gnu)"
    mkdir -p "${QCOM_GCC_DIR}"
    TARBALL="${QCOM_GCC_DIR}/arm-gnu-toolchain.tar.xz"
    wget --quiet --show-progress "${QCOM_TOOLCHAIN_URL}" -O "${TARBALL}"
    tar xf "${TARBALL}" -C "${QCOM_GCC_DIR}" --strip-components 1
    rm -f "${TARBALL}"
fi

mkdir -p "${CACHE}"

# --- Ubuntu linux-qcom source ------------------------------------------------
# The Debian source package, not git: Canonical's kernel git is behind an auth
# wall and its cgit times out, while the archive is public and versioned. The
# .diff.gz carries Canonical's delta, which is where the sc7280 CAMSS backport
# lives - mainline only got it in v6.18.
if [[ ! -d "${QCOM_KERNEL_SRC}" ]]; then
    ORIG="${CACHE}/$(basename "${QCOM_KERNEL_ORIG_URL}")"
    DIFF="${CACHE}/$(basename "${QCOM_KERNEL_DIFF_URL}")"
    [[ -f "${ORIG}" ]] || { echo "Fetching kernel source (~230 MB)";
        wget --quiet --show-progress "${QCOM_KERNEL_ORIG_URL}" -O "${ORIG}"; }
    [[ -f "${DIFF}" ]] || { echo "Fetching Ubuntu delta";
        wget --quiet --show-progress "${QCOM_KERNEL_DIFF_URL}" -O "${DIFF}"; }

    echo "Unpacking ${QCOM_KERNEL_SRCPKG} ${QCOM_KERNEL_VER}"
    tar xzf "${ORIG}" -C "${BUILD_SRCS}"
    mv "${BUILD_SRCS}/linux-${QCOM_KERNEL_UPSTREAM%.0}" "${QCOM_KERNEL_SRC}"
    zcat "${DIFF}" | patch -s -p1 -d "${QCOM_KERNEL_SRC}" --no-backup-if-mismatch

    # Named base so apply_patches_qcom.sh reset has a stable target.
    git -C "${QCOM_KERNEL_SRC}" init --quiet
    git -C "${QCOM_KERNEL_SRC}" add -A
    git -C "${QCOM_KERNEL_SRC}" -c user.name=rs -c user.email=rs@local \
        commit --quiet -m "${QCOM_KERNEL_SRCPKG} ${QCOM_KERNEL_VER}"
    git -C "${QCOM_KERNEL_SRC}" tag -f rs-base > /dev/null
else
    echo "Kernel source already present: ${QCOM_KERNEL_SRC}"
fi

# --- SerDes donor tree -------------------------------------------------------
if [[ ! -d "${QCOM_NVOOT_SRC}/.git" ]]; then
    echo "Cloning SerDes donor ${NVOOT_REPO} (${NVOOT_TAG})"
    if ! git clone --quiet --depth 1 --branch "${NVOOT_TAG}" \
            "${NVOOT_REPO}" "${QCOM_NVOOT_SRC}"; then
        echo -e "\e[31mFailed to clone the SerDes donor.\e[0m"
        echo "If you already have a JetPack workspace, point at it instead:"
        echo "  ln -s \$PWD/sources_6.2/nvidia-oot ${QCOM_NVOOT_SRC}"
        exit 1
    fi
    git -C "${QCOM_NVOOT_SRC}" tag -f rs-base HEAD > /dev/null
else
    echo "SerDes donor already present: ${QCOM_NVOOT_SRC}"
fi

echo
echo -e "\e[32mWorkspace ready.\e[0m Next: ./apply_patches_qcom.sh ${QCOM_INPUT_VERSION}"

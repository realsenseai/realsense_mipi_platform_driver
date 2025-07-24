#!/bin/bash

set -e

if [[ "$1" == "-h" ]]; then
    echo "build_all.sh [--dev-dbg] [JetPack_version] [JetPack_Linux_source]"
    echo "build_all.sh -h"
    exit 1
fi

DEVDBG=0
if [[ "$1" == "--dev-dbg" ]]; then
    DEVDBG=1
    shift
fi

export DEVDIR=$(cd `dirname $0` && pwd)
NPROC=$(nproc)

. $DEVDIR/scripts/setup-common "$1"

SRCS="$DEVDIR/sources_$JETPACK_VERSION"
if [[ -n "$2" ]]; then
    SRCS=$(realpath $2)
fi

if [[ $(uname -m) == aarch64 ]]; then
    echo
    echo Native build
    echo
else
    if [[ "$JETPACK_VERSION" == "6.x" ]]; then
        export CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-buildroot-linux-gnu-
    elif [[ "$JETPACK_VERSION" == "5.x" ]]; then
        export CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-buildroot-linux-gnu-
    elif [[ "$JETPACK_VERSION" == "4.6.1" ]]; then
        export CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-linux-gnu-
    fi
fi

export LOCALVERSION=-tegra
export TEGRA_KERNEL_OUT=$DEVDIR/images/$JETPACK_VERSION
mkdir -p $TEGRA_KERNEL_OUT
export KERNEL_MODULES_OUT=$TEGRA_KERNEL_OUT/modules

# Check if BUILD_NUMBER is set as it will add a postfix to the kernel name "vermagic" (normally it happens on CI who have BUILD_NUMBER defined)
[[ -n "${BUILD_NUMBER}" ]] && echo "Warning! You have BUILD_NUMBER set to ${BUILD_NUMBER}, This will affect your vermagic"

# Build jp6 out-of-tree modules
# following: 
# https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Kernel/KernelCustomization.html#building-the-jetson-linux-kernel
if [[ "$JETPACK_VERSION" == "6.x" ]]; then
    cd $SRCS
    export KERNEL_HEADERS=$SRCS/kernel/kernel-jammy-src
    ln -sf $TEGRA_KERNEL_OUT $SRCS/out
    if [[ "$DEVDBG" == "1" ]]; then
        cd $KERNEL_HEADERS
        # Generate .config file from default defconfig
        make ARCH=arm64 defconfig
        # Update the CONFIG_DYNAMIC_DEBUG and CONFIG_DEBUG_CORE flags in .config file
        scripts/config --enable DYNAMIC_DEBUG
        scripts/config --enable DYNAMIC_DEBUG_CORE
        # Convert the .config file into defconfig 
        make ARCH=arm64 savedefconfig
        # Save the new generated file as custom_defconfig
        cp defconfig ./arch/arm64/configs/custom_defconfig
        # Remove unwanted
        rm defconfig .config
        make ARCH=arm64 mrproper
        cd $SRCS
        # Building the Image with custom_defconfig
        make ARCH=arm64 KERNEL_DEF_CONFIG=custom_defconfig -C kernel
    else
        # Building the Image with default defconfig
        make ARCH=arm64 -C kernel
    fi
    make ARCH=arm64 modules
    make ARCH=arm64 dtbs
    mkdir -p $TEGRA_KERNEL_OUT/rootfs/boot/dtb
    cp $SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-p3737-0000+p3701-0000-nv.dtb $TEGRA_KERNEL_OUT/rootfs/boot/dtb/
    cp $SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-p3737-0000+p3701-0005-nv.dtb $TEGRA_KERNEL_OUT/rootfs/boot/dtb/
    cp $SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-camera-d4xx-overlay*.dtbo $TEGRA_KERNEL_OUT/rootfs/boot/
    export INSTALL_MOD_PATH=$TEGRA_KERNEL_OUT/rootfs/
    make ARCH=arm64 install -C kernel
    make ARCH=arm64 modules_install
    # iio support
    KERNELVERSION=$(cat $KERNEL_HEADERS/include/config/kernel.release)
    KERNEL_MODULES_OUT=$INSTALL_MOD_PATH/lib/modules/${KERNELVERSION}
    mkdir -p $KERNEL_MODULES_OUT/extra
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/buffer/kfifo_buf.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/buffer/industrialio-triggered-buffer.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/common/hid-sensors/hid-sensor-iio-common.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/hid/hid-sensor-hub.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/accel/hid-sensor-accel-3d.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/gyro/hid-sensor-gyro-3d.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/common/hid-sensors/hid-sensor-trigger.ko $KERNEL_MODULES_OUT/extra/
    # RealSense cameras support
    cp $KERNEL_MODULES_OUT/kernel/drivers/media/usb/uvc/uvcvideo.ko $KERNEL_MODULES_OUT/extra/
    cp $KERNEL_MODULES_OUT/kernel/drivers/media/v4l2-core/videodev.ko $KERNEL_MODULES_OUT/extra/
else
#jp4/5
    cd $SRCS/$KERNEL_DIR
    make ARCH=arm64 O=$TEGRA_KERNEL_OUT tegra_defconfig
    if [[ "$DEVDBG" == "1" ]]; then
        scripts/config --file $TEGRA_KERNEL_OUT/.config --enable DYNAMIC_DEBUG
    fi
    make ARCH=arm64 O=$TEGRA_KERNEL_OUT -j${NPROC}
    make ARCH=arm64 O=$TEGRA_KERNEL_OUT modules_install INSTALL_MOD_PATH=$KERNEL_MODULES_OUT
fi


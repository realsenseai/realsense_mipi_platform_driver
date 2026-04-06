#!/bin/bash

set -e

CLEAN=0
DEVDBG=0

# Parse optional flags
while [[ "$1" == --* ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --dev-dbg)
            DEVDBG=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

export DEVDIR=$(cd `dirname $0` && pwd)
NPROC=$(nproc)

if [[ "$1" == "-h" ]]; then
    echo "build_all.sh [--clean] [--dev-dbg] [JetPack_version [JetPack_Linux_source]]"
    echo "build_all.sh -h"
fi

. scripts/setup-common "$1"

BUILD_SRCS="${DEVDIR}/${BUILD_SRCS}"
if [[ -n "$2" ]]; then
    BUILD_SRCS=$(realpath $2)
fi

if [[ $(uname -m) == aarch64 ]]; then
    echo
    echo Native build
    echo
else
    if [[ "$JETPACK_VERSION" == "7.x" ]]; then
        CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-none-linux-gnu-
    elif [[ "$JETPACK_VERSION" == "6.x" ]]; then
        CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-buildroot-linux-gnu-
    elif [[ "$JETPACK_VERSION" == "5.x" ]]; then
        CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-buildroot-linux-gnu-
    elif [[ "$JETPACK_VERSION" == "4.x" ]]; then
        CROSS_COMPILE=$DEVDIR/l4t-gcc/$JETPACK_VERSION/bin/aarch64-linux-gnu-
    fi
    export CROSS_COMPILE
fi

export LOCALVERSION=-tegra
export TEGRA_KERNEL_OUT="$DEVDIR/images/${JP_INPUT_VERSION}"

# Clean if requested
if [[ $CLEAN == 1 ]]; then
    echo "Cleaning build artifacts for ${JP_INPUT_VERSION}..."
    rm -rf $TEGRA_KERNEL_OUT
    rm -rf $BUILD_SRCS/out
fi

mkdir -p $TEGRA_KERNEL_OUT
export KERNEL_MODULES_OUT=$TEGRA_KERNEL_OUT/modules

# Check if BUILD_NUMBER is set as it will add a postfix to the kernel name "vermagic" (normally it happens on CI who have BUILD_NUMBER defined)
[[ -n "${BUILD_NUMBER}" ]] && echo "Warning! You have BUILD_NUMBER set to ${BUILD_NUMBER}, This will affect your vermagic"

# Build jp6 out-of-tree modules
# following: 
# https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Kernel/KernelCustomization.html#building-the-jetson-linux-kernel
if version_lt "$JETPACK_VERSION" "6.0"; then
    #JP4/5
    cd $BUILD_SRCS/$KERNEL_DIR
    make O=$TEGRA_KERNEL_OUT tegra_defconfig
    if [[ "$DEVDBG" == "1" ]]; then
        scripts/config --file $TEGRA_KERNEL_OUT/.config --enable DYNAMIC_DEBUG
    fi
    make O=$TEGRA_KERNEL_OUT -j${NPROC}
    make O=$TEGRA_KERNEL_OUT modules_install INSTALL_MOD_PATH=$KERNEL_MODULES_OUT
    D4XX_CMD_FILE="$(find "$TEGRA_KERNEL_OUT" -name '.d4xx.o.cmd' 2>/dev/null | head -1)"
else
    cd $BUILD_SRCS
    export KERNEL_HEADERS=${BUILD_SRCS}/${KERNEL_DIR}
    ln -sf $TEGRA_KERNEL_OUT $BUILD_SRCS/out
    if [[ "$DEVDBG" == "1" ]]; then
        cd $KERNEL_HEADERS
        # Generate .config file from default defconfig
        make defconfig
        # Update the CONFIG_DYNAMIC_DEBUG and CONFIG_DEBUG_CORE flags in .config file
        scripts/config --enable DYNAMIC_DEBUG
        scripts/config --enable DYNAMIC_DEBUG_CORE
        # Convert the .config file into defconfig 
        make savedefconfig
        # Save the new generated file as custom_defconfig
        cp defconfig ./arch/arm64/configs/custom_defconfig
        # Remove unwanted
        rm defconfig .config
        make mrproper
        cd $BUILD_SRCS
        # Building the Image with custom_defconfig
        make KERNEL_DEF_CONFIG=custom_defconfig -C kernel
    else
        # Building the Image with default defconfig
        make -C kernel
    fi
    make modules
    D4XX_CMD_FILE="$BUILD_SRCS/nvidia-oot/drivers/media/i2c/.d4xx.o.cmd"
    mkdir -p $TEGRA_KERNEL_OUT/rootfs/boot/dtb
    if version_lt "$JETPACK_VERSION" "7.0"; then
        make dtbs
        cp $BUILD_SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-camera-d4xx-overlay*.dtbo $TEGRA_KERNEL_OUT/rootfs/boot/
        cp $BUILD_SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-p3737-0000+p3701-0000-nv.dtb $TEGRA_KERNEL_OUT/rootfs/boot/dtb/
        cp $BUILD_SRCS/nvidia-oot/device-tree/platform/generic-dts/dtbs/tegra234-p3737-0000+p3701-0005-nv.dtb $TEGRA_KERNEL_OUT/rootfs/boot/dtb/
    else
        cp $BUILD_SRCS/$KERNEL_DIR/arch/arm64/boot/dts/nvidia/tegra2[36]4-camera-d4xx-overlay*.dtbo $TEGRA_KERNEL_OUT/rootfs/boot/
    fi
    export INSTALL_MOD_PATH=$TEGRA_KERNEL_OUT/rootfs/
    make -C kernel install
    make modules_install
    # iio support
    KERNELVERSION=$(cat $KERNEL_HEADERS/include/config/kernel.release)
    KERNEL_MODULES_OUT=$INSTALL_MOD_PATH/lib/modules/${KERNELVERSION}
    mkdir -p $KERNEL_MODULES_OUT/extra
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/buffer/kfifo_buf.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/buffer/industrialio-triggered-buffer.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/common/hid-sensors/hid-sensor-iio-common.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/hid/hid-sensor-hub.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/accel/hid-sensor-accel-3d.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/gyro/hid-sensor-gyro-3d.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/iio/common/hid-sensors/hid-sensor-trigger.ko $KERNEL_MODULES_OUT/extra/ || true
    # RealSense cameras support
    cp $KERNEL_MODULES_OUT/kernel/drivers/media/usb/uvc/uvcvideo.ko $KERNEL_MODULES_OUT/extra/ || true
    cp $KERNEL_MODULES_OUT/kernel/drivers/media/v4l2-core/videodev.ko $KERNEL_MODULES_OUT/extra/ || true
fi

# Generate .vscode/compile_commands.json from the cached module build artefact
echo "Generating .vscode/compile_commands.json..."
if [ -n "${D4XX_CMD_FILE:-}" ] && [ -f "$D4XX_CMD_FILE" ]; then
    "$DEVDIR/scripts/generate_compile_commands.sh" "$D4XX_CMD_FILE" || \
        echo "Warning: compile_commands.json generation failed (non-fatal)"
else
    echo "Warning: .d4xx.o.cmd not found; skipping compile_commands.json generation"
fi

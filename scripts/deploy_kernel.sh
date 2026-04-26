
#!/bin/bash

# Display help message
if [ "$#" -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: $0 <JETPACK_VERSION> [TARGET] [USERNAME] [REMOTE_PATH] [REMOTE_BOOT_FOLDER]"
    echo ""
    echo "Package kernel modules, optionally copy them to the TARGET, update boot files and reboot the TARGET."
    echo ""
    echo "Arguments:"
    echo "  JETPACK_VERSION   JetPack version (e.g., 5.0.2, 5.1.2, 6.0, 6.1, 6.2, 6.2.1, 7.0, 7.1) - REQUIRED"
    echo "  TARGET            Target device hostname or IP address"
    echo "  USERNAME          SSH username for TARGET (default: administrator)"
    echo "  REMOTE_PATH       Remote path to copy files to (default: dev)"
    echo "  REMOTE_BOOT_FOLDER   Folder name under /boot on TARGET (default: dev)"
    echo ""
    echo "Example:"
    echo "  $0 6.2 192.168.1.100 - pack and copy to /home/administrator/dev, update /boot/dev and reboot TARGET"
    echo "  $0 6.2 192.168.1.100 nvidia - pack and copy to /home/nvidia/dev, update /boot/dev and reboot TARGET"
    echo "  $0 6.2 192.168.1.100 nvidia foo - pack and copy to /home/nvidia/foo, update /boot/dev and reboot TARGET"
    echo "  $0 6.2 192.168.1.100 nvidia foo bar - pack and copy to /home/nvidia/foo, update /boot/bar and reboot TARGET"
    exit 0
fi

# Set defaults
JETPACK_VERSION="$1"
TARGET="$2"
USERNAME="${3:-administrator}"
REMOTE_PATH="${4:-dev}"
REMOTE_BOOT_FOLDER="${5:-dev}"
LOCAL_DIR="."

# Check if kernel_mod directory exists, create if not
if [ ! -d ${LOCAL_DIR}/kernel_mod ]; then
    echo "Creating directory ${LOCAL_DIR}/kernel_mod..."
    mkdir -p ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}
else
    mkdir -p ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}
fi

# Clean version folder if files exist
if [ "$(ls -A ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION} 2>/dev/null)" ]; then
     echo "Cleaning ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}..."
     rm -rf ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}/*
fi


# Copy specific kernel files for compatibility with legacy scripts (only for 5.0.2)
IMG_DIR="images/${JETPACK_VERSION}"
DEST_DIR="${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}"

if [ "${JETPACK_VERSION}" = "5.0.2" ]; then
    echo "Copying kernel files to ${DEST_DIR} (5.0.2 only)..."
    cp "${IMG_DIR}/arch/arm64/boot/Image" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/arch/arm64/boot/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/drivers/media/i2c/d4xx.ko" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/drivers/media/i2c/max96712.ko" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/drivers/media/usb/uvc/uvcvideo.ko" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/drivers/media/v4l2-core/videobuf-core.ko" "${DEST_DIR}/" 2>/dev/null || true
    cp "${IMG_DIR}/drivers/media/v4l2-core/videobuf-vmalloc.ko" "${DEST_DIR}/" 2>/dev/null || true
elif [ "${JETPACK_VERSION}" = "6.0" ] || [ "${JETPACK_VERSION}" = "6.1" ] || [ "${JETPACK_VERSION}" = "6.2" ] || [ "${JETPACK_VERSION}" = "6.2.1" ] || [ "${JETPACK_VERSION}" = "7.0" ] || [ "${JETPACK_VERSION}" = "7.1" ]; then
    echo "Packing ${DEST_DIR}/rootfs.tar.gz"
    tar czf ${DEST_DIR}/rootfs.tar.gz -C images/${JETPACK_VERSION}/rootfs boot lib
fi

if [ -z "${TARGET}" ]; then
    echo "No TARGET specified, skipping copy and reboot."
    exit 0
else
    echo "Copying files and setting permissions on remote host..."
    cp ${LOCAL_DIR}/scripts/install_to_kernel.sh ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION}/
    # Use SSH ControlMaster to reuse a single SSH connection
    CONTROL_PATH="/tmp/ssh-control-${USERNAME}-${TARGET}"
    ssh -o ControlMaster=yes -o ControlPath="${CONTROL_PATH}" -o ControlPersist=10s -fN ${USERNAME}@${TARGET}
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "rm -rf ${REMOTE_PATH}/kernel_mod/${JETPACK_VERSION} && mkdir -p ${REMOTE_PATH}/kernel_mod"
    scp -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" -r ${LOCAL_DIR}/kernel_mod/${JETPACK_VERSION} ${USERNAME}@${TARGET}:${REMOTE_PATH}/kernel_mod/
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "chmod +x ${REMOTE_PATH}/kernel_mod/${JETPACK_VERSION}/install_to_kernel.sh"
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "cd ${REMOTE_PATH}/kernel_mod/${JETPACK_VERSION} && ./install_to_kernel.sh ${JETPACK_VERSION} ${REMOTE_BOOT_FOLDER}"
    ssh -o ControlPath="${CONTROL_PATH}" -O exit ${USERNAME}@${TARGET} 2>/dev/null
fi

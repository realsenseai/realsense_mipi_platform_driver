
#!/bin/bash

# Display help message
if [ "$#" -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: $0 [TARGET] [USERNAME] [REMOTE_PATH] [REMOTE_BOOT_FOLDER]"
    echo ""
    echo "Package kernel modules, optionally copy them to the TARGET, update boot files and reboot the TARGET."
    echo ""
    echo "Arguments:"
    echo "  TARGET        Target device hostname or IP address"
    echo "  USERNAME      SSH username for TARGET (default: administrator)"
    echo "  REMOTE_PATH   Remote path to copy files to (default: dev)"
    echo "  REMOTE_BOOT_FOLDER   Folder name under /boot on TARGET (default: dev)"
    echo ""
    echo "Example:"
    echo "  $0 192.168.1.100 - pack and copy to /home/administrator/dev, update /boot/dev and reboot TARGET"
    echo "  $0 192.168.1.100 nvidia - pack and copy to /home/nvidia/dev, update /boot/dev and reboot TARGET"
    echo "  $0 192.168.1.100 nvidia foo - pack and copy to /home/nvidia/foo, update /boot/dev and reboot TARGET"
    echo "  $0 192.168.1.100 nvidia foo bar - pack and copy to /home/nvidia/foo, update /boot/bar and reboot TARGET"
    exit 0
fi

# Set defaults
TARGET="$1"
USERNAME="${2:-administrator}"
REMOTE_PATH="${3:-dev}"
REMOTE_BOOT_FOLDER="${4:-dev}"
LOCAL_DIR="."

# Check if kernel_mod directory exists, create if not
if [ ! -d ${LOCAL_DIR}/kernel_mod ]; then
    echo "Creating directory ${LOCAL_DIR}/kernel_mod..."
    mkdir -p ${LOCAL_DIR}/kernel_mod/6.2
else
    mkdir -p ${LOCAL_DIR}/kernel_mod/6.2
fi

# Clean 6.2 folder if files exist
if [ "$(ls -A ${LOCAL_DIR}/kernel_mod/6.2 2>/dev/null)" ]; then
     echo "Cleaning ${LOCAL_DIR}/kernel_mod/6.2..."
     rm -rf ${LOCAL_DIR}/kernel_mod/6.2/*
fi

echo "Packing ${LOCAL_DIR}/kernel_mod/6.2/rootfs.tar.gz"
tar czf ${LOCAL_DIR}/kernel_mod/6.2/rootfs.tar.gz -C images/6.2/rootfs boot lib

if [ "$#" -eq 0 ]; then
    echo "No TARGET specified, skipping copy and reboot."
else
    echo "Copying files and setting permissions on remote host..."
    cp ${LOCAL_DIR}/install_to_kernel_6.2.sh ${LOCAL_DIR}/kernel_mod/6.2/
    # Use SSH ControlMaster to reuse a single SSH connection
    CONTROL_PATH="/tmp/ssh-control-${USERNAME}-${TARGET}"
    ssh -o ControlMaster=yes -o ControlPath="${CONTROL_PATH}" -o ControlPersist=10s -fN ${USERNAME}@${TARGET}
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "rm -rf ${REMOTE_PATH}/kernel_mod/6.2 && mkdir -p ${REMOTE_PATH}/kernel_mod"
    scp -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" -r ${LOCAL_DIR}/kernel_mod/6.2 ${USERNAME}@${TARGET}:${REMOTE_PATH}/kernel_mod/
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "chmod +x ${REMOTE_PATH}/kernel_mod/6.2/install_to_kernel_6.2.sh"
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${TARGET} "cd ${REMOTE_PATH}/kernel_mod/6.2 && ./install_to_kernel_6.2.sh ${REMOTE_BOOT_FOLDER}"
    ssh -o ControlPath="${CONTROL_PATH}" -O exit ${USERNAME}@${TARGET} 2>/dev/null
fi

#!/bin/bash

# Display help message
show_help() {
    echo "Usage: $0 <KERNEL_VERSION> [IP_ADDRESS] [USERNAME] [REMOTE_PATH]"
    echo ""
    echo "Aggregate kernel modules and optionally copy them to a remote device."
    echo ""
    echo "Arguments:"
    echo "  KERNEL_VERSION  Kernel version to aggregate (6.0, 6.1, or 6.2) (required)"
    echo "  IP_ADDRESS      Target device IP address (optional - if not provided, only aggregates locally)"
    echo "  USERNAME        SSH username for remote device (default: administrator)"
    echo "  REMOTE_PATH     Remote path to copy files to (default: /home/USERNAME/)"
    echo ""
    echo "Example:"
    echo "  $0 6.2"
    echo "  $0 6.2 192.168.1.100"
    echo "  $0 6.1 192.168.1.100 nvidia"
    echo "  $0 6.0 192.168.1.100 administrator REMOTE_PATH/"
    exit 0
}

# Check if help is requested or no arguments provided
if [ "$#" -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    show_help
fi

# Check if kernel version is provided
if [ "$#" -lt 1 ]; then
    echo "Error: Missing required kernel version"
    echo ""
    show_help
fi

KERNEL_VERSION="$1"
IP_ADDRESS="${2:-}"
USERNAME="${3:-administrator}"
REMOTE_PATH="/home/${USERNAME}/${4:-ymodlin/}/"

# Get script directory and repo root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Validate kernel version
if [[ "$KERNEL_VERSION" != "6.0" && "$KERNEL_VERSION" != "6.1" && "$KERNEL_VERSION" != "6.2" ]]; then
    echo "Error: Invalid kernel version '$KERNEL_VERSION'. Must be 6.0, 6.1, or 6.2"
    echo ""
    show_help
fi

# Check if kernel_mod directory exists, create if not
if [ ! -d "$REPO_ROOT/kernel_mod" ]; then
    echo "Creating directory $REPO_ROOT/kernel_mod..."
    mkdir -p "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}"
else
    mkdir -p "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}"
fi

# Clean version folder if files exist
if [ "$(ls -A "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}" 2>/dev/null)" ]; then
     echo "Cleaning $REPO_ROOT/kernel_mod/${KERNEL_VERSION}..."
     rm -rf "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}"/*
fi

echo "Packing $REPO_ROOT/kernel_mod/${KERNEL_VERSION}..."
tar czf "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}/rootfs.tar.gz" -C "$REPO_ROOT/images/${KERNEL_VERSION}/rootfs" boot lib
cp "$SCRIPT_DIR/install_to_kernel_dev.sh" "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}/"

# Only copy to remote if IP address is provided
if [ -n "$IP_ADDRESS" ]; then
    echo "Copying files and setting permissions on remote host..."
    # Use SSH ControlMaster to reuse a single SSH connection
    CONTROL_PATH="/tmp/ssh-control-${USERNAME}-${IP_ADDRESS}"
    ssh -o ControlMaster=yes -o ControlPath="${CONTROL_PATH}" -o ControlPersist=10s -fN ${USERNAME}@${IP_ADDRESS}
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${IP_ADDRESS} "mkdir -p ${REMOTE_PATH}kernel_mod"
    scp -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" -r "$REPO_ROOT/kernel_mod/${KERNEL_VERSION}" ${USERNAME}@${IP_ADDRESS}:${REMOTE_PATH}kernel_mod/
    ssh -o ControlMaster=no -o ControlPath="${CONTROL_PATH}" ${USERNAME}@${IP_ADDRESS} "chmod +x ${REMOTE_PATH}kernel_mod/${KERNEL_VERSION}/install_to_kernel_dev.sh"
    ssh -o ControlPath="${CONTROL_PATH}" -O exit ${USERNAME}@${IP_ADDRESS} 2>/dev/null
    echo "Done! Files copied to ${USERNAME}@${IP_ADDRESS}:${REMOTE_PATH}kernel_mod/${KERNEL_VERSION}/"
else
    echo "Done! Files aggregated to $REPO_ROOT/kernel_mod/${KERNEL_VERSION}/"
fi

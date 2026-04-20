#!/bin/bash
set -e

# Display help message
if [ "$#" -lt 1 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
      echo "Usage: $0 <JETPACK_VERSION> [KERNEL_FOLDER] [DELAY_SECONDS]"
      echo ""
      echo "Update the kernel modules and boot files on the local device for a specific JetPack version."
      echo ""
      echo "Arguments:"
      echo "  JETPACK_VERSION   JetPack version (e.g., 5.0.2, 5.1.2, 5.1.6, 6.0, 6.1, 6.2, 6.2.1, 6.2.2, 7.0, 7.1)"
      echo "  KERNEL_FOLDER     Folder name under /boot to install kernel files to (default: dev)"
      echo "  DELAY_SECONDS     Delay before reboot in seconds (default: 0)"
      echo ""
      echo "Example:"
      echo "  $0 6.2 foo"
      exit 0
fi

JETPACK_VERSION="$1"
FOLDER="/boot/${2:-dev}"
DELAY="${3:-0}"

# Validate DELAY is a non-negative integer
if ! [[ "${DELAY}" =~ ^[0-9]+$ ]]; then
    echo "Error: DELAY_SECONDS must be a non-negative integer"
    exit 1
fi

echo "Extracting rootfs tarball..."
tar -C "${JETPACK_VERSION}" -xf ${JETPACK_VERSION}/rootfs.tar.*

KERNEL_VERSION=$(ls ${JETPACK_VERSION}/lib/modules)
MODULES_DIR="/lib/modules/${KERNEL_VERSION}"

echo "Installing "${KERNEL_VERSION}" kernel files for JetPack ${JETPACK_VERSION}..."
echo "Install new modules to ${MODULES_DIR}"
sudo -S cp -r ${JETPACK_VERSION}${MODULES_DIR} /lib/modules/
[ -d "${FOLDER}" ] || sudo mkdir "${FOLDER}"
echo "Install new device tree files to ${FOLDER}/dtb/"
sudo cp -r "${JETPACK_VERSION}/boot/dtb" "${FOLDER}/"
echo "Install new image to ${FOLDER}/"
kernel_image=$(ls -t ${JETPACK_VERSION}/boot/vmlinu?-${KERNEL_VERSION}|head -n1)
sudo cp "${kernel_image}" "${FOLDER}"/
kernel_image=$(basename "${kernel_image}")
echo "Image file: ${kernel_image}"
echo "Creating symbolic link to new kernel image: ${FOLDER}/Image"
sudo ln -sfT "${FOLDER}/${kernel_image}" ${FOLDER}/Image

sudo depmod ${KERNEL_VERSION}
sudo update-initramfs -ck ${KERNEL_VERSION}
echo "Creating symbolic link to new initrd: ${FOLDER}/initrd"
sudo ln -sfT /boot/initrd.img-${KERNEL_VERSION} "${FOLDER}/initrd"
# do not reboot if new image is not a boot option
! grep "LINUX\ ${FOLDER}" /boot/extlinux/extlinux.conf && echo "Not a boot option, not rebooting" && exit 0
echo "Done - scheduling reboot in ${DELAY} seconds (backgrounded via sudo -n)"
sudo -n bash -c "nohup sh -c \"sleep ${DELAY}; /sbin/reboot\" >/dev/null 2>&1 &"
sudo reboot

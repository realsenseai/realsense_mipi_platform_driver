# Intel® RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Orin™ JetPack 6.x 
The Intel® RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.
The system shall include:
* NVIDIA® Jetson™ platform Supported JetPack versions are:
    - [6.2 production release](https://developer.nvidia.com/embedded/jetpack-sdk-62)
    - [6.1 production release](https://developer.nvidia.com/embedded/jetpack-sdk-61)
    - [6.0 production release](https://developer.nvidia.com/embedded/jetpack-sdk-60)
* RealSense™ [De-Serialize board](https://store.intelrealsense.com/buy-intel-realsense-des457.html)
* Jetson AGX Orin™ Passive adapter board from [Leopard Imaging® LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera [D457](https://store.intelrealsense.com/buy-intel-realsense-depth-camera-d457.html)

![orin_adapter](https://github.com/dmipx/realsense_mipi_platform_driver/assets/104717350/524e3eb6-6e6b-41cf-9562-9c0f920dd821)


> Note: This MIPI reference driver is based on RealSense™ de-serialize board. For other de-serialize boards, modification might be needed. 

### Links
- Intel® RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.x](./README_JP6.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

## NVIDIA® Jetson AGX Orin™ board setup on x86-64 host for cross-compile

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the NVIDIA® Jetson AGX Orin™ with NVIDIA® SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.



## Build environment prerequisites
```
sudo apt-get install -y build-essential bc wget flex bison curl libssl-dev xxd
```
## Build NVIDIA® kernel drivers, dtb and D457 driver - cross compile x86-64

1. Clone [realsense_mipi_platform_driver](https://github.com/IntelRealSense/realsense_mipi_platform_driver.git) repo.
2. The developers can set up build environment, ARM64 compiler, kernel sources and NVIDIA's Jetson git repositories by using the setup script.
3. Apply patches for kernel drivers, nvidia-oot module and tegra devicetree.
4. Build cross-compile project on host (Build PC).
5. Apply build results to target (Jetson).
6. Configure target.

```
git clone https://github.com/IntelRealSense/realsense_mipi_platform_driver.git
cd realsense_mipi_platform_driver
./setup_workspace.sh 6.2
./apply_patches.sh 6.2
./build_all.sh 6.2
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg 6.2
```



## JetPack manual build - cross compile x86-64 (CI deploy)

[NVIDIA® JetPack 6.2: Jetson Linux 36.4.3](https://developer.nvidia.com/embedded/jetson-linux-r3643)
1. Download Jetson Linux Driver Package - [JetPack 6.2 BSP sources](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.3/release/jetson_linux_r36.4.3_aarch64.tbz2)
2. Download Toolchain ARM64 compiler - [Bootlin Toolchain gcc 11.3](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v3.0/toolchain/aarch64--glibc--stable-2022.08-1.tar.bz2)
3. Apply patches for kernel drivers, nvidia-oot module and tegra devicetree.
4. Build cross-compile project on host (Build PC).
5. Apply build results to target (Jetson).
6. Configure target.

```
# JetPack 6.2
mkdir -p l4t-gcc/6.x
cd ./l4t-gcc/6.x
wget https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v3.0/toolchain/aarch64--glibc--stable-2022.08-1.tar.bz2 -O aarch64--glibc--stable-final.tar.bz2
tar xf aarch64--glibc--stable-final.tar.bz2 --strip-components 1
cd ../..
wget https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.3/sources/public_sources.tbz2
tar xjf public_sources.tbz2
cd Linux_for_Tegra/source
tar xjf kernel_src.tbz2
tar xjf kernel_oot_modules_src.tbz2
tar xjf nvidia_kernel_display_driver_source.tbz2
cd ../..

./apply_patches_ext.sh 6.2 Linux_for_Tegra/source

cp ./nvidia-oot/Makefile Linux_for_Tegra/source
cp ./kernel/kernel-jammy-src/Makefile Linux_for_Tegra/source/kernel

# build kernel, dtb and D457 driver
./build_all.sh 6.2 ./Linux_for_Tegra/source
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg 6.2 ./Linux_for_Tegra/source
```

## Archive JetPack 6.x build results (optional) on build host
Assuming 6.2 (or 6.1) build the kernel version is 5.15.148-tegra. For 6.0 the kernel version is 5.15.136-tegra.
- kernel image : `images/6.2/rootfs/boot/Image`
- dtb: `images/6.2/rootfs/boot/dtb/tegra234-p3737-0000+p3701-0000-nv.dtb`
- dtb overlay: `images/6.2/rootfs/boot/tegra234-camera-d4xx-overlay.dtbo`
- dtb dual camera overlay: `images/6.2/rootfs/boot/tegra234-camera-d4xx-overlay-dual.dtbo`
- nvidia-oot modules: `images/6.2/rootfs/lib/modules/5.15.148-tegra/updates`
- kernel modules: `images/6.2/rootfs/lib/modules/5.15.148-tegra/extra`

```
echo "Archiving boot configuration build results"
tar -czf ./images/6.2/boot-config.tar.gz -C "./images/6.2/rootfs/" ./boot
echo "Archiving nvidia-oot modules"
tar -czf ./images/6.2/nvidia-oot-modules.tar.gz -C "./images/6.2/rootfs/" ./lib/modules/5.15.148-tegra/updates
echo "Archiving kernel HID modules"
tar -czf ./images/6.2/kernel-hid-modules.tar.gz -C "./images/6.2/rootfs/" ./lib/modules/5.15.148-tegra/extra
```
## Backup JetPack 6.2 boot configuration and drivers (optional)
```
echo "Backup boot configuration"
sudo cp /boot/tegra234-p3737-0000+p3701-0000-nv.dtb /boot/tegra234-p3737-0000+p3701-0000-nv-bkp.dtb

# Note: If using a production board and not a dev kit copy the relevant dtb file below
sudo cp /boot/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/tegra234-p3737-0000+p3701-0005-nv-bkp.dtb

echo "backup nvidia-oot modules"
sudo tar -czf /lib/modules/5.15.148-tegra/updates.tar.gz -C /lib/modules/5.15.148-tegra/ updates
```

## Install kernel drivers, extra modules and device-tree to Jetson AGX Orin

Following steps required:

1.	Copy entire directory `images/6.2/rootfs/lib/modules/5.15.148-tegra/updates` from host to `/lib/modules/5.15.148-tegra/` on Orin target
2.	Copy entire directory `images/6.2/rootfs/lib/modules/5.15.148-tegra/extra` from host to `/lib/modules/5.15.148-tegra/` on Orin target
3.	Copy `tegra234-camera-d4xx-overlay.dtbo` from host to `/boot/tegra234-camera-d4xx-overlay.dtbo` on Orin target
4.      Copy `tegra234-p3737-0000+p3701-0000-nv.dtb` from host to `/boot/` on Orin
5.      Copy `Image` from host to `/boot/` on Orin (only if `--dev-dbg` option is enabled, else not needed as the kernel will be unmodified)
6.	Run  $ `sudo /opt/nvidia/jetson-io/jetson-io.py`
    1.	Configure Jetson AGX CSI Connector
    2.	Configure for compatible hardware
    3.	Choose appropriate configuration:
        1. Jetson RealSense Camera D457
        2. Jetson RealSense Camera D457 dual
    4.  Enable depmod scan for "extra" & "kernel" modules $ `sudo sed -i 's/search updates/search extra updates kernel/g' /etc/depmod.d/ubuntu.conf`
        ```
        $ cat /etc/depmod.d/ubuntu.conf
        search extra updates kernel ubuntu built-in
        ```
    5.	$ `sudo depmod`
    6.	$ `echo "d4xx" | sudo tee /etc/modules-load.d/d4xx.conf`
    
9.  Verify bootloader configuration

    ```
    cat /boot/extlinux/extlinux.conf
    ----<CUT>----
    LABEL JetsonIO
        MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457>
        LINUX /boot/Image
        FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
        APPEND ${cbootargs} root=PARTUUID=bbb3b34e-......
        OVERLAYS /boot/tegra234-camera-d4xx-overlay.dtbo
    ----<CUT>----
    ```
10.	Reboot

## Deploy build results on Jetson target
On build host, copy build results to the right places.
Assuming Jetson has ip: `10.0.0.116`

```
# Configuration files
scp -r images/6.2/rootfs/boot nvidia@10.0.0.116:~/
# RealSense support for NVIDIA Tegra
scp -r images/6.2/rootfs/lib/modules/5.15.148-tegra/updates nvidia@10.0.0.116:~/
# RealSense metadata patched kernel modules and IMU HID support
scp -r images/6.2/rootfs/lib/modules/5.15.148-tegra/extra nvidia@10.0.0.116:~/
# Updated kernel modules to match the new compiled kernel image
scp -r images/6.2/rootfs/lib/modules/5.15.148-tegra/kernel nvidia@10.0.0.116:~/
```

On Jetson target, assuming backup step was followed:

```
# enable RealSense extra formats and metadata
sudo cp -r ~/extra /lib/modules/5.15.148-tegra/
# enable RealSense MIPI support for D457 GMSL
sudo cp -r ~/updates /lib/modules/5.15.148-tegra/
sudo cp -r ~/kernel/ /lib/modules/5.15.148-tegra/
sudo cp ~/boot/tegra234-camera-d4xx-overlay.dtbo /boot/

# (replace with tegra234-p3737-0000+p3701-0000-nv.dtb on production Orin board)
sudo cp ~/boot/dtb/tegra234-p3737-0000+p3701-0000-nv.dtb /boot/tegra234-p3737-0000+p3701-0000-nv.dtb

sudo cp ./boot/Image /boot/Image
# Enable d4xx overlay for single camera:
sudo /opt/nvidia/jetson-io/config-by-hardware.py -n 2="Jetson RealSense Camera D457"

# For dual camera setup:
# sudo /opt/nvidia/jetson-io/config-by-hardware.py -n 3="Jetson RealSense Camera D457 dual"

# enable extra & kernel modules
sudo sed -i 's/search updates/search extra updates kernel/g' /etc/depmod.d/ubuntu.conf

# Enable d4xx autoload:
echo "d4xx" | sudo tee /etc/modules-load.d/d4xx.conf
# Enable depmod scan for "extra" modules
# original file content: cat /etc/depmod.d/ubuntu.conf -- search updates ubuntu built-in
sudo sed -i 's/search updates/search extra updates/g' /etc/depmod.d/ubuntu.conf
# update driver cache
sudo depmod

#Reboot machine.
sudo reboot
```

### Verify driver loaded - on Jetson:
- Driver API manual page [Driver API page](./README_driver.md)

```
nvidia@ubuntu:~$ sudo dmesg | grep tegra-capture-vi
[    9.357521] platform 13e00000.host1x:nvcsi@15a00000: Fixing up cyclic dependency with tegra-capture-vi
[    9.419926] tegra-camrtc-capture-vi tegra-capture-vi: ep of_device is not enabled endpoint.
[    9.419932] tegra-camrtc-capture-vi tegra-capture-vi: ep of_device is not enabled endpoint.
[   10.001170] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound
[   10.025295] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 12-001a bound
[   10.040934] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 13-001a bound
[   10.056151] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 14-001a bound
[   10.288088] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.324025] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.324631] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.325056] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound

nvidia@ubuntu:~$ sudo dmesg | grep d4xx
[    9.443608] d4xx 9-001a: Probing driver for D45x
[    9.983168] d4xx 9-001a: ds5_chrdev_init() class_create
[    9.989521] d4xx 9-001a: D4XX Sensor: DEPTH, firmware build: 5.15.1.0
[   10.007813] d4xx 12-001a: Probing driver for D45x
[   10.013899] d4xx 12-001a: D4XX Sensor: RGB, firmware build: 5.15.1.0
[   10.025787] d4xx 13-001a: Probing driver for D45x
[   10.029095] d4xx 13-001a: D4XX Sensor: Y8, firmware build: 5.15.1.0
[   10.041282] d4xx 14-001a: Probing driver for D45x
[   10.044759] d4xx 14-001a: D4XX Sensor: IMU, firmware build: 5.15.1.0

```

### Known issues
- Camera not recognized
Verify I2C MUX detected. If "probe failed" reported, replace extension board adapter (LI-JTX1-SUB-ADPT).
```
nvidia@ubuntu:~$ sudo dmesg | grep pca954x
[    3.933113] pca954x 2-0072: probe failed
```

- Configuration with jetson-io tool system fail to boot with message "couldn't find root partition"
Verfiy bootloader configuration
`/boot/extlinux/extlinux.conf`
Sometimes configuration tool missing APPEND parameters. Duplicate `primary` section `APPEND` line to `JetsonIO` `APPEND` section, verify it's similar.

Exaple Bad:
```
LABEL primary
      MENU LABEL primary kernel
      LINUX /boot/Image
      INITRD /boot/initrd
      APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config

LABEL JetsonIO
        MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual>
        LINUX /boot/Image
        FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
        INITRD /boot/initrd
        APPEND ${cbootargs}
        OVERLAYS /boot/tegra234-camera-d4xx-overlay-dual.dtbo
```
Exaple Good:
```
LABEL primary
      MENU LABEL primary kernel
      LINUX /boot/Image
      INITRD /boot/initrd
      APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config

LABEL JetsonIO
        MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual>
        LINUX /boot/Image
        FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
        INITRD /boot/initrd
        APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config
        OVERLAYS /boot/tegra234-camera-d4xx-overlay-dual.dtbo
```
- Configuration tool jetson-io terminates without configuration menu.
verify that `/boot/dtb` has only one dtb file
```
nvidia@ubuntu:~$ ls /boot/dtb/
kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
```

- kernel does not recognize the I2C device
```
# Make sure which Jetson Carrier board is used:
#   p3701-0000 → Dev kit carrier board
#   p3701-0005 → Production carrier board or custom carrier
# if you have the *0005* board, replace the relevant dtb file in in the instructions above

Example: 
sudo cat /proc/device-tree/compatible

Output:
nvidia,p3701-0000
```
---

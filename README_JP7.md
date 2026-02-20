# RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Orin™ JetPack 6.x 
The RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.
The system shall include:
* NVIDIA® Jetson™ platform Supported JetPack versions are:
    - 7.1 production release
    - 7.0 production release
* RealSense™ De-Serialize board
* Jetson AGX Orin™ Passive adapter board from [Leopard Imaging® LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera [D457](https://store.realsenseai.com/buy-intel-realsense-depth-camera-d457.html)

![orin_adapter](https://github.com/dmipx/realsense_mipi_platform_driver/assets/104717350/524e3eb6-6e6b-41cf-9562-9c0f920dd821)


> Note: This MIPI reference driver is based on RealSense™ de-serialize board. For other de-serialize boards, modification might be needed. 

### Links
- RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.0](./README_JP6.0.md) setup guide
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.2](./README_JP6.2.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

## NVIDIA® Jetson AGX Orin™ board setup

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the NVIDIA® Jetson AGX Orin™ with NVIDIA® SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.

## Build environment prerequisites
```
sudo apt-get install -y build-essential bc wget flex bison curl libssl-dev xxd tar
```
## Build NVIDIA® kernel drivers, dtb and D457 driver

These are descriptiver steps. Bash commands to be issued follow:
1. Clone [realsense_mipi_platform_driver](https://github.com/realsenseai/realsense_mipi_platform_driver.git) repo.
2. Checkout dev branch.
3. The developers can set up build environment, ARM64 compiler, kernel sources and NVIDIA's Jetson git repositories by using the setup script.
4. Apply patches for kernel drivers, nvidia-oot module and tegra devicetree.
5. Build project
6. Apply build results to target (Jetson).
7. Configure target.

Assuming building for 7.1. One can also build for 7.0 just replace the last parameter.
Build version can be specified only once. It will be written to jetpack_version.txt file and used for later steps.
You can display the current version cating the file jetpack_version. It will be show at the beginning of each script.
```
git clone --branch dev --single-branch https://github.com/realsenseai/realsense_mipi_platform_driver.git
cd realsense_mipi_platform_driver
./setup_workspace.sh 7.1
./apply_patches.sh
./build_all.sh
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg
```

## Install kernel drivers, extra modules and device-tree to Jetson AGX Orin

Following steps required:

1. Copy build artifacts:
If you build locally (native build on Jetson) use the following bash commands:
```
sudo cp -r ./images/7.1/rootfs/lib/modules/6.8.12-tegra /lib/modules/
sudo cp    ./images/7.1/rootfs/boot/tegra234-camera-d4xx-overlay.dtbo /boot/
sudo cp    ./images/7.1/rootfs/boot/dtb/tegra234-p3737-0000+p3701-0000-nv.dtb /boot/dtb/
sudo cp    ./images/7.1/rootfs/boot/Image /boot/
```
In case of crossbuild scp copy from host to Jetson target use the following commands:
```
tar xf rootfs.tar.gz
sudo cp -r ./lib/modules/6.8.12-tegra /lib/modules/.
sudo cp    ./boot/tegra234-camera-d4xx-overlay.dtbo /boot/.
sudo cp    ./boot/Image /boot/
```
2.	Run  $ `sudo /opt/nvidia/jetson-io/jetson-io.py`, to exit choose save & reboot:
	1.	Configure Jetson AGX CSI Connector
	2.	Configure for compatible hardware
	3.	Choose appropriate configuration:
 		i.	Jetson RealSense Camera D457
		ii. Jetson RealSense Camera D457 dual
    5.	Choose to save & reboot

3.	Enable and run depmod scan for "extra" & "kernel" modules
```
# enable extra & kernel modules
# original file content: cat /etc/depmod.d/ubuntu.conf -- search updates ubuntu built-in
sudo sed -i 's/search updates/search extra updates kernel/g' /etc/depmod.d/ubuntu.conf
# update driver cache
sudo depmod
echo "d4xx" | sudo tee /etc/modules-load.d/d4xx.conf
```
4.
Verify bootloader configuration
```
cat /boot/extlinux/extlinux.conf
----<CUT>----
LABEL JetsonIO
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457>
    LINUX /boot/dev/Image
    FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
    APPEND ${cbootargs} root=PARTUUID=bbb3b34e-......
    OVERLAYS /boot/tegra234-camera-d4xx-overlay.dtbo
----<CUT>----
```
On Jetson target (user home folder) assuming backup step was followed:


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
Verify bootloader configuration
`/boot/extlinux/extlinux.conf`
Sometimes configuration tool missing APPEND parameters. Duplicate `primary` section `APPEND` line to `JetsonIO` `APPEND` section, verify it's similar.

Example Bad:
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
Example Good:
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
### Notes
- With the introduction of meta data support for depth IR starting from release r/1.0.1.27, calibration format streaming requires a separate DTB that disables meta data since the camera does not metadata in calibration mode.
- If calibration is needed, it's recommended to add `d457_calib` boot option as shown below.

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

LABEL JetsonIO_calib
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual - Calibration>
    LINUX /boot/Image
    FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
    INITRD /boot/initrd
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config
    OVERLAYS /boot/tegra234-camera-d4xx-overlay-dual.calib.dtbo
```

- To generate `tegra234-camera-d4xx-overlay.calib.dtbo` and `tegra234-camera-d4xx-overlay-dual.calib.dtbo` on the Host build system, replace the contents of the below files:
    `tegra234-camera-d4xx-overlay.dts` by the contents of `tegra234-camera-d4xx-overlay.calib.dts`
    
    `tegra234-camera-d4xx-overlay-dual.dts` by the contents of `tegra234-camera-d4xx-overlay-dual.calib.dts`

- Reset and reapply patches and rebuild driver:
```
./apply_patches.sh 6.2 reset

./apply_patches.sh 6.2

./build_all.sh 6.2
```

- Deploy 2 DTBO files on the Jetson:
- In `./images/6.2/rootfs/boot/` folder, locate and rename the following DTBO files:
    - Rename `tegra234-camera-d4xx-overlay.dtbo` to `tegra234-camera-d4xx-overlay.calib.dtbo`
    - Rename `tegra234-camera-d4xx-overlay-dual.dtbo` to `tegra234-camera-d4xx-overlay-dual.calib.dtbo`
- Copy the two DTBO files from the build Host to `/boot/` on the Jetson. 

---

# RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Thor™ JetPack 7
The RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.
The system shall include:
* NVIDIA® Jetson™ platform Supported JetPack versions:
	- 7.1 production release
	- 7.0 production release
* RealSense™ De-Serialize board
* Jetson AGX Orin™ Passive adapter board from [Leopard Imaging® LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera [D457](https://store.realsenseai.com/buy-intel-realsense-depth-camera-d457.html)

![orin_adapter](https://github.com/dmipx/realsense_mipi_platform_driver/assets/104717350/524e3eb6-6e6b-41cf-9562-9c0f920dd821)


> Note: This MIPI reference driver is based on RealSense™ de-serialize board. For other de-serialize boards, modification might be needed. 

### Links
- RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.x](./README_JP6.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

## NVIDIA® Jetson AGX Thor™ board setup

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the NVIDIA® Jetson AGX Thor™ with NVIDIA® SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.

## Build environment prerequisites
```
sudo apt-get install -y build-essential bc wget flex bison curl libssl-dev xxd tar
```
## Build NVIDIA® kernel drivers, dtb and D457 driver

These are descriptiver steps. Bash commands to be issued follow:
1. Clone [realsense_mipi_platform_driver](https://github.com/realsenseai/realsense_mipi_platform_driver.git) repo.
2. Checkout the `dev` branch.
3. Set up build environment, ARM64 compiler, kernel sources and NVIDIA's Jetson git repositories by using the setup script.
4. Apply patches for kernel drivers, nvidia-oot module and tegra devicetree.
5. Build the project
6. Apply build results to the target (Jetson).
7. Configure the target.

Assuming building for 7.1. One can also build for 7.0 just replace the last parameter.
Build version can be specified only once. It will be written to jetpack_version.txt file and used for later steps.
You can display the current version running any script below with -h option. Effective version will be also shown while running any script.
```
git clone --branch dev --single-branch https://github.com/realsenseai/realsense_mipi_platform_driver.git
cd realsense_mipi_platform_driver
./setup_workspace.sh 7.1
./apply_patches.sh
./build_all.sh
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg`
```
./build_all.sh --dev-dbg
```

## Install kernel drivers, extra modules and device-tree to Jetson AGX Thor™

Following steps required:

1. Copy build artifacts:

If you build locally (native build on Jetson) use the following bash commands:
```
sudo cp -r images/$(cat jetpack_version)/rootfs/lib/modules/$(cat kernel_version) /lib/modules/
sudo cp -r images/$(cat jetpack_version)/rootfs/boot/dtb /boot/dev/
sudo cp -v images/$(cat jetpack_version)/rootfs/boot/vmlinu?-$(cat kernel_version) /boot/dev/
sudo ln -sfT /boot/dev/vmlinu?-$(cat kernel_version) /boot/dev/Image
```
In case of crossbuild on external host prepare a tarball to ssh-copy to Jetson target.
Example user 'nvidia' on Jetson with host name 'jetson.domain'
```
tar czf rootfs.tar.gz -C images/$(cat jetpack_version)/rootfs boot lib kernel_version
scp rootfs.tar.gz nvidia@jetson.domain:
```
Log in into Jetson target, extract the tarball and install extracted files:
```
rm -rf boot lib
tar xf rootfs.tar.gz
sudo cp -r lib/modules/$(cat kernel_version) /lib/modules/
sudo cp -r boot/dtb /boot/dev/
sudo cp -v boot/vmlinu?-$(cat kernel_version) /boot/dev/
sudo ln -sfT /boot/dev/vmlinu?-$(cat kernel_version) /boot/dev/Image
```
2.	Enable and run depmod scan for "extra" & "kernel" modules
```
# original file content: cat /etc/depmod.d/ubuntu.conf -- search updates ubuntu built-in
sudo sed -i 's/search updates/search extra updates kernel/g' /etc/depmod.d/ubuntu.conf
# update driver cache
sudo depmod
# create initramfs file in /boot/ for new kernel
sudo update-initramfs -u -k $(cat kernel_version)
sudo ln -sfT /boot/initrd.img-$(cat kernel_version) /boot/dev/initrd
```
3.	Run  $ `sudo /opt/nvidia/jetson-io/jetson-io.py`:
	1.	Configure Jetson AGX CSI Connector
	2.	Configure for compatible hardware
	3.	Choose appropriate configuration:
		i.	Jetson RealSense Camera D457
		ii.	Jetson RealSense Camera D457 dual
    5.	Save and exit

4.
Verify bootloader configuration
```
cat /boot/extlinux/extlinux.conf
----<CUT>----
LABEL JetsonIO
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457>
    LINUX /boot/dev/Image
    INITRD /boot/dev/initrd
    APPEND ${cbootargs} root=...
    FDT /boot/dtb/kernel_tegra264-p4071-0000+p3834-0008-nv.dtb
    OVERLAYS /boot/dev/dtb/tegra264-camera-d4xx-overlay...dtbo
----<CUT>----
```
5.
Reboot cycling the power or using shell command
```
sudo reboot
```

On Jetson target (user home folder) assuming backup step was followed:

### Verify driver loaded - on Jetson:
- Driver API manual page [Driver API page](./README_driver.md)

```
nvidia@ubuntu:~$ sudo dmesg | grep tegra-capture-vi
[    2.206518] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@0: Fixed dependency cycle(s) with /tegra-capture-vi
[    2.214883] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@1: Fixed dependency cycle(s) with /tegra-capture-vi
[    2.225349] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@2: Fixed dependency cycle(s) with /tegra-capture-vi
[    2.235476] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@3: Fixed dependency cycle(s) with /tegra-capture-vi
[    2.245945] kernel: /tegra-capture-vi: Fixed dependency cycle(s) with /bus@0/host1x@8181200000/nvcsi@8188000000/channel@0
[    2.256075] kernel: /tegra-capture-vi: Fixed dependency cycle(s) with /bus@0/host1x@8181200000/nvcsi@8188000000/channel@1
[    2.266548] kernel: /tegra-capture-vi: Fixed dependency cycle(s) with /bus@0/host1x@8181200000/nvcsi@8188000000/channel@2
[    2.276680] kernel: /tegra-capture-vi: Fixed dependency cycle(s) with /bus@0/host1x@8181200000/nvcsi@8188000000/channel@3
[   12.881599] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@3: Fixed dependency cycle(s) with /tegra-capture-vi
[   12.903194] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@2: Fixed dependency cycle(s) with /tegra-capture-vi
[   12.924501] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@1: Fixed dependency cycle(s) with /tegra-capture-vi
[   12.934992] kernel: /bus@0/host1x@8181200000/nvcsi@8188000000/channel@0: Fixed dependency cycle(s) with /tegra-capture-vi
[   13.633789] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev 8181200000.host1x:nvcsi@8188000000--4 bound
[   13.633816] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev 8181200000.host1x:nvcsi@8188000000--3 bound
[   13.633820] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev 8181200000.host1x:nvcsi@8188000000--2 bound
[   13.633824] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev 8181200000.host1x:nvcsi@8188000000--1 bound
[   16.639389] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound
[   16.656921] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound
[   16.676673] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound
[   16.694458] kernel: tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound

nvidia@ubuntu:~$ sudo dmesg | grep d4xx
[   14.002791] kernel: d4xx 9-001a: Probing driver for D4xx
[   14.002840] kernel: d4xx 9-001a: supply vcc not found, using dummy regulator
[   14.005305] kernel: d4xx 9-001a: Using deserializer max96712
[   14.008158] kernel: d4xx 9-001a: Deserializer 9-0029 linked
[   15.257239] kernel: d4xx 9-001a: ds5_chrdev_init() class_create
[   15.257398] kernel: d4xx 9-001a: ds5_probe(): first probe instance, running HW reset recovery
...
[   16.625938] kernel: d4xx 9-001a: D4XX Sensor: DEPTH, firmware build: 5.17.0.10
[   16.640070] kernel: d4xx 9-001a: ds5_probe: driver version: 1.0.2.27
[   16.640220] kernel: d4xx 9-001b: Probing driver for D4xx
[   16.640236] kernel: d4xx 9-001b: supply vcc not found, using dummy regulator
[   16.640515] kernel: d4xx 9-001b: Using deserializer max96712
[   16.640519] kernel: d4xx 9-001b: peer instance, skipping SERDES setup
[   16.643657] kernel: d4xx 9-001b: D4XX Sensor: RGB, firmware build: 5.17.0.10
[   16.659673] kernel: d4xx 9-001b: ds5_probe: driver version: 1.0.2.27
[   16.659910] kernel: d4xx 9-001c: Probing driver for D4xx
[   16.659921] kernel: d4xx 9-001c: supply vcc not found, using dummy regulator
[   16.660034] kernel: d4xx 9-001c: Using deserializer max96712
[   16.660038] kernel: d4xx 9-001c: peer instance, skipping SERDES setup
[   16.663191] kernel: d4xx 9-001c: D4XX Sensor: Y8, firmware build: 5.17.0.10
[   16.677134] kernel: d4xx 9-001c: ds5_probe: driver version: 1.0.2.27
[   16.677723] kernel: d4xx 9-001d: Probing driver for D4xx
[   16.677732] kernel: d4xx 9-001d: supply vcc not found, using dummy regulator
[   16.677809] kernel: d4xx 9-001d: Using deserializer max96712
[   16.677815] kernel: d4xx 9-001d: peer instance, skipping SERDES setup
[   16.680969] kernel: d4xx 9-001d: D4XX Sensor: IMU, firmware build: 5.17.0.10
[   16.694839] kernel: d4xx 9-001d: ds5_probe: driver version: 1.0.2.27
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
Sometimes configuration tool skips APPEND parameters. Duplicate `primary` section `APPEND` line to `JetsonIO` `APPEND` section.

Example Bad:
```
LABEL primary
    MENU LABEL primary kernel
    LINUX /boot/Image
    INITRD /boot/initrd
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config

LABEL JetsonIO
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual>
    LINUX /boot/dev/vmlinux-<ver from previous step>
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
    LINUX /boot/dev/vmlinux-<ver from previous step>
    FDT /boot/dtb/kernel_tegra264-p4071-0000+p3834-0008-nv.dtb
    INITRD /boot/initrd.img-<ver from previous step>
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config
    OVERLAYS /boot/dev/dtb/tegra264-camera-d4xx-overlay.dtbo
```
- Configuration tool jetson-io terminates without configuration menu.
verify that `/boot/dtb` has only one dtb file
```
nvidia@ubuntu:~$ ls /boot/dtb/
kernel_tegra264-p4071-0000+p3834-0008-nv.dtb
```

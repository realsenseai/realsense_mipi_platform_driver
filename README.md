# Intel® RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Xavier™ and AGX Orin™
The RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.
The system shall include:
* NVIDIA® Jetson platform (Currently Supported JetPack versions are: 6.0, 5.1.2, 5.0.2, 4.6.1)
* De-Serialize board
* NVIDIA® Jetson AGX Orin™ Passive adapter board from [Leopard Imaging LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera (e.g. https://store.intelrealsense.com/buy-intel-realsense-depth-camera-d457.html)

> Note: This MIPI reference driver is based on RealSense de-serialize board. For other de-serialize boards, modification might be needed. 

![image](https://user-images.githubusercontent.com/64067618/216807681-ed679a79-71d6-43ab-bfde-e0abb019b72d.png)


# Documentation

- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.0](./README_JP6.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

## NVIDIA® Jetson AGX Xavier™ and AGX Orin™ board setup

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the Jetson AGX Xavier with NVIDIA SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.

**NOTE** : On Jetsons with modified factory setup modifications to build and deploy steps should be made by developer.

# JetPack build

- [JetPack 6.0](./README_JP6.md#build-environment-prerequisites)
- [JetPack 5.1.2](./README_JP5.md#build-kernel-dtb-and-d457-driver)
- [JetPack 5.0.2](./README_JP5.md#build-kernel-dtb-and-d457-driver)
- [JetPack 4.6.1](./README_JP4.md#build-kernel-dtb-and-d457-driver)

### Verify driver after installation
- Driver API manual page [Driver API page](./README_driver.md)

**NOTE**

- Each JetPack version's kernel may be different, the user needs to change the kernel version in file names and paths accordingly, for example for JetPack 4.6.1 the version is `4.9.253-d457` or `4.9.253-tegra`, depending on the build method applied.
- For JetPack 4.6.1, the dtb file is not included in the deb package. User needs to manually copy `images/4.6.1/arch/arm64/boot/dts/tegra194-p2888-0001-p2822-0000.dtb` file to board and edit `extlinux.conf` to point to it.
- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.

  
## Available directives on max9295/max9296 register setting

- Dump registers
```
cat /sys/bus/i2c/drivers/max9295/30-0040/register_dump
cat /sys/bus/i2c/drivers/max9296/30-0048/register_dump
```

- Dump setting version

```
cat /sys/module/max9295/parameters/max9295_setting_verison
cat /sys/module/max9296/parameters/max9296_setting_verison
```

- Disable updating setting dynamically (updating setting manually by running script).
  **0** means disable updating setting dynamically, while **1** means enable updating setting dynamically.

```
echo 0 | sudo tee /sys/module/max9295/parameters/max9295_dynamic_update
echo 0 | sudo tee /sys/module/max9296/parameters/max9296_dynamic_update
```

- Refresh max9295/max9295 register values, this is used for forcely set serdes setting when necessary

```
echo 1 | sudo tee /sys/bus/i2c/drivers/max9295/30-0040/refresh_setting
echo 1 | sudo tee /sys/bus/i2c/drivers/max9296/30-0048/refresh_setting
```

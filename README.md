# Intel® RealSense™ camera driver for GMSL* interface

# D457 MIPI on Jetson AGX Xavier and Orin 
The RealSense MIPI platform driver enables the user to control and stream RealSense 3D MIPI cameras.
The system shall include:
* Jetson platform (Currently Supported JetPack versions are: 6.0, 5.1.2, 5.0.2, 4.6.1)
* RealSense De-Serialize board (https://store.intelrealsense.com/buy-intel-realsense-des457.html)
* Jetson Orin Passive adapter board from [Leopard Imaging LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera (e.g. https://store.intelrealsense.com/buy-intel-realsense-depth-camera-d457.html)

> Note: This MIPI reference driver is based on RealSense de-serialize board. For other de-serialize boards, modification might be needed. 

![image](https://user-images.githubusercontent.com/64067618/216807681-ed679a79-71d6-43ab-bfde-e0abb019b72d.png)


# Documentation

- Jetson AGX Orin board setup - AGX Orin [JetPack 6.0](./README_JP6.md) setup guide
- Jetson AGX Xavier board setup - AGX Xavier [JetPack 5.0.2](./README_JP5.md) setup guide
- Jetson AGX Xavier board setup - AGX Xavier [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)


## Jetson AGX Xavier and Orin board setup

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the Jetson AGX Xavier with NVIDIA SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.

**NOTE** : On Jetsons with modified factory setup modifications to build and deploy steps should be made by developer.

# JetPack build

- [JetPack 6.0](./README_JP6.md#build-environment-prerequisites)
- [JetPack 5.1.2](./README_JP5.md#build-kernel-dtb-and-d457-driver)
- [JetPack 5.0.2](./README_JP5.md#build-kernel-dtb-and-d457-driver)
- [JetPack 4.6.1](./README_JP4.md#build-kernel-dtb-and-d457-driver)


**NOTE**

- Each JetPack version's kernel may be different, the user needs to change the kernel version in file names and paths accordingly, for example for JetPack 4.6.1 the version is `4.9.253-d457` or `4.9.253-tegra`, depending on the build method applied.
- For JetPack 4.6.1, the dtb file is not included in the deb package. User needs to manually copy `images/4.6.1/arch/arm64/boot/dts/tegra194-p2888-0001-p2822-0000.dtb` file to board and edit `extlinux.conf` to point to it.
- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.
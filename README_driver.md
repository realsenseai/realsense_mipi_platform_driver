# Intel® RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Xavier™ and AGX Orin™ driver API manual pages

### Links
- Intel® RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.x](./README_JP6.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

#### Verify driver installation
- Install V4L2 Utilities
```
sudo apt install v4l-utils
```

- Verify driver loaded - on Jetson JetPack 6.0:
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

- Check video devices, should be six video devices:
  - video0 - Depth stream
  - video1 - Depth metadata stream
  - video2 - Color RGB stream
  - video3 - Color RGB metadata stream
  - video4 - IR stream
  - video5 - IMU stream
```
$ ls -l /dev/video*
crw-rw----+ 1 root video 81,  0 Jun  1 04:36 /dev/video0
crw-rw----+ 1 root video 81,  1 Jun  1 04:36 /dev/video1
crw-rw----+ 1 root video 81, 11 Jun  1 04:36 /dev/video2
crw-rw----+ 1 root video 81, 12 Jun  1 04:36 /dev/video3
crw-rw----+ 1 root video 81, 18 Jun  1 04:36 /dev/video4
crw-rw----+ 1 root video 81, 24 Jun  1 04:36 /dev/video5
```
- Check DFU device:
```
ls -l /sys/class/d4xx-class/
total 0
lrwxrwxrwx 1 root root 0 Jun  2 15:39 d4xx-dfu-30-0010 -> ../../devices/virtual/d4xx-class/d4xx-dfu-30-0010

ls -l /dev/d4xx-dfu-*
crw-rw---- 1 root video 506, 0 Jun  1 04:36 /dev/d4xx-dfu-30-0010
```
#### Check capture driver:
```
v4l2-ctl -d0 -D
Driver Info (not using libv4l2):
        Driver name   : tegra-video
        Card type     : vi-output, DS5 mux 30-0010
        Bus info      : platform:15c10000.vi:0
        Driver version: 4.9.253
        Capabilities  : 0x84A00001
                Video Capture
                Metadata Capture
                Streaming
                Extended Pix Format
                Device Capabilities
        Device Caps   : 0x04200001
                Video Capture
                Streaming
                Extended Pix Format
```
#### Check available formats
```
v4l2-ctl -d0 -V
Format Video Capture:
        Width/Height      : 640/480
        Pixel Format      : 'Z16 '
        Field             : None
        Bytes per Line    : 1280
        Size Image        : 614400
        Colorspace        : sRGB
        Transfer Function : Default (maps to sRGB)
        YCbCr/HSV Encoding: Default (maps to ITU-R 601)
        Quantization      : Default (maps to Full Range)
        Flags             :

```
```
v4l2-ctl -d4 --list-formats
ioctl: VIDIOC_ENUM_FMT
        Index       : 0
        Type        : Video Capture
        Pixel Format: 'GREY'
        Name        : 8-bit Greyscale

        Index       : 1
        Type        : Video Capture
        Pixel Format: 'Y8I '
        Name        : Interleaved 8-bit Greyscale

        Index       : 2
        Type        : Video Capture
        Pixel Format: 'Y12I'
        Name        : Interleaved 12-bit Greyscale
```
- Depth formats
```
v4l2-ctl -d0 --list-formats
ioctl: VIDIOC_ENUM_FMT
        Index       : 0
        Type        : Video Capture
        Pixel Format: 'Z16 '
        Name        : 16-bit Depth
```
- Depth metadata format
```
v4l2-ctl -d1 --list-formats-meta
ioctl: VIDIOC_ENUM_FMT
        Index       : 0
        Type        : Metadata Capture
        Pixel Format: 'D4XX'
        Name        : D4XX meta data
```

<details>
<summary>Depth extended formats</summary>

```
v4l2-ctl -d0 --list-formats-ext
ioctl: VIDIOC_ENUM_FMT
        Index       : 0
        Type        : Video Capture
        Pixel Format: 'Z16 '
        Name        : 16-bit Depth
                Size: Discrete 1280x720
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                Size: Discrete 848x480
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                        Interval: Discrete 0.017s (60.000 fps)
                        Interval: Discrete 0.011s (90.000 fps)
                Size: Discrete 848x100
                        Interval: Discrete 0.010s (100.000 fps)
                Size: Discrete 640x480
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                        Interval: Discrete 0.017s (60.000 fps)
                        Interval: Discrete 0.011s (90.000 fps)
                Size: Discrete 640x360
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                        Interval: Discrete 0.017s (60.000 fps)
                        Interval: Discrete 0.011s (90.000 fps)
                Size: Discrete 480x270
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                        Interval: Discrete 0.017s (60.000 fps)
                        Interval: Discrete 0.011s (90.000 fps)
                Size: Discrete 424x240
                        Interval: Discrete 0.200s (5.000 fps)
                        Interval: Discrete 0.067s (15.000 fps)
                        Interval: Discrete 0.033s (30.000 fps)
                        Interval: Discrete 0.017s (60.000 fps)
                        Interval: Discrete 0.011s (90.000 fps)
                Size: Discrete 256x144
                        Interval: Discrete 0.011s (90.000 fps)
```

</details>



#### Verify camera controls
```
v4l2-ctl -d0 -l
```
- Read firmware version v4l2 control
```
v4l2-ctl -d0 -C fw_version
fw version: 84934657
```

<details>
<summary>Camera Controls</summary>

```
v4l2-ctl -d0 -l

Camera Controls

                  auto_exposure 0x009a0901 (menu)   : min=0 max=3 default=3 value=3 flags=volatile, execute-on-write
         exposure_time_absolute 0x009a0902 (u32)    : min=1 max=200000 step=1 default=33000 flags=volatile, execute-on-write
           sensor_configuration 0x009a2032 (u32)    : min=0 max=4294967295 step=1 default=0 [22] flags=read-only, volatile, has-payload
         sensor_mode_i2c_packet 0x009a2033 (u32)    : min=0 max=4294967295 step=1 default=0 [1026] flags=read-only, volatile, has-payload
      sensor_control_i2c_packet 0x009a2034 (u32)    : min=0 max=4294967295 step=1 default=0 [1026] flags=read-only, volatile, has-payload
                    bypass_mode 0x009a2064 (intmenu): min=0 max=1 default=0 value=0
                override_enable 0x009a2065 (intmenu): min=0 max=1 default=0 value=0
                   height_align 0x009a2066 (int)    : min=1 max=16 step=1 default=1 value=1
                     size_align 0x009a2067 (intmenu): min=0 max=2 default=0 value=0
               write_isp_format 0x009a2068 (int)    : min=1 max=1 step=1 default=1 value=1
       sensor_signal_properties 0x009a2069 (u32)    : min=0 max=4294967295 step=1 default=0 [30][18] flags=read-only, has-payload
        sensor_image_properties 0x009a206a (u32)    : min=0 max=4294967295 step=1 default=0 [30][16] flags=read-only, has-payload
      sensor_control_properties 0x009a206b (u32)    : min=0 max=4294967295 step=1 default=0 [30][36] flags=read-only, has-payload
              sensor_dv_timings 0x009a206c (u32)    : min=0 max=4294967295 step=1 default=0 [30][16] flags=read-only, has-payload
               low_latency_mode 0x009a206d (bool)   : default=0 value=0
               preferred_stride 0x009a206e (int)    : min=0 max=65535 step=1 default=0 value=0
                   sensor_modes 0x009a2082 (int)    : min=0 max=30 step=1 default=30 value=1 flags=read-only
                         logger 0x009a4000 (u8)     : min=0 max=0 step=1 default=0 [1024] flags=read-only, volatile, has-payload
             laser_power_on_off 0x009a4001 (bool)   : default=1 value=1 flags=volatile, execute-on-write
             manual_laser_power 0x009a4002 (int)    : min=0 max=360 step=30 default=150 value=360 flags=volatile, execute-on-write
                get_depth_calib 0x009a4003 (u8)     : min=0 max=0 step=1 default=0 [256] flags=read-only, volatile, has-payload
                set_depth_calib 0x009a4004 (u8)     : min=0 max=4294967295 step=1 default=240 [256] flags=has-payload
                get_coeff_calib 0x009a4005 (u8)     : min=0 max=0 step=1 default=0 [512] flags=read-only, volatile, has-payload
                set_coeff_calib 0x009a4006 (u8)     : min=0 max=4294967295 step=1 default=240 [512] flags=has-payload
                     fw_version 0x009a4007 (u32)    : min=0 max=0 step=1 default=0 [1] flags=read-only, volatile, has-payload
                            gvd 0x009a4008 (u8)     : min=0 max=0 step=1 default=0 [239] flags=read-only, volatile, has-payload
                     ae_roi_get 0x009a4009 (u8)     : min=0 max=0 step=1 default=0 [8] flags=read-only, volatile, has-payload
                     ae_roi_set 0x009a400a (u8)     : min=0 max=4294967295 step=1 default=240 [8] flags=has-payload
                ae_setpoint_get 0x009a400b (int)    : min=0 max=0 step=1 default=0 value=0 flags=read-only, volatile
                ae_setpoint_set 0x009a400c (int)    : min=0 max=4095 step=1 default=0 value=0
                erb_eeprom_read 0x009a400d (u8)     : min=0 max=4294967295 step=1 default=240 [1020] flags=has-payload
               ewb_eeprom_write 0x009a400e (u8)     : min=0 max=4294967295 step=1 default=240 [1020] flags=has-payload
                           hwmc 0x009a400f (u8)     : min=0 max=4294967295 step=1 default=240 [1028] flags=has-payload
         pwm_frequency_selector 0x009a4016 (int)    : min=0 max=1 step=1 default=1 value=1 flags=volatile, execute-on-write
                        hwmc_rw 0x009a4020 (u8)     : min=0 max=4294967295 step=1 default=240 [1024] flags=volatile, has-payload, execute-on-write

Image Source Controls

                  analogue_gain 0x009e0903 (int)    : min=16 max=248 step=1 default=16 value=16 flags=volatile, execute-on-write

```

</details>

---


#### Verify stream
```
v4l2-ctl -d0 --stream-mmap
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< 29.70 fps
```

- Verify FPS changes
```
v4l2-ctl -d0 -p 60
Frame rate set to 60.000 fps

v4l2-ctl -d0 --stream-mmap
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< 60.39 fps
<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< 60.19 fps
```

#### Verify Firmware Version
```
cat /dev/d4xx-dfu-30-0010
DFU info:       ver:  5.16.0.1
```

#### Verify media bindings

```
media-ctl --print-dot
```
- Use Graphviz tool to visualize media bindings - [Graphviz Visual Editor](http://magjac.com/graphviz-visual-editor/)

<details>
<summary>Media Bindings</summary>

media-ctl --print-dot

digraph board {
        rankdir=TB
        n00000001 [label="{{<port1> 1 | <port2> 2 | <port3> 3 | <port4> 4} | DS5 mux 9-001a\n/dev/v4l-subdev4 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000001:port0 -> n00000059:port0
        n00000007 [label="{{} | D4XX depth 9-001a\n/dev/v4l-subdev0 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000007:port0 -> n00000001:port1 [style=bold]
        n0000000b [label="{{} | D4XX ir 9-001a\n/dev/v4l-subdev1 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000000b:port0 -> n00000001:port3 [style=bold]
        n0000000f [label="{{} | D4XX rgb 9-001a\n/dev/v4l-subdev2 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000000f:port0 -> n00000001:port2 [style=bold]
        n00000013 [label="{{} | D4XX imu 9-001a\n/dev/v4l-subdev3 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000013:port0 -> n00000001:port4 [style=bold]
        n00000017 [label="{{<port1> 1 | <port2> 2 | <port3> 3 | <port4> 4} | DS5 mux 12-001a\n/dev/v4l-subdev9 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000017:port0 -> n00000092:port0
        n0000001d [label="{{} | D4XX depth 12-001a\n/dev/v4l-subdev5 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000001d:port0 -> n00000017:port1 [style=bold]
        n00000021 [label="{{} | D4XX ir 12-001a\n/dev/v4l-subdev6 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000021:port0 -> n00000017:port3 [style=bold]
        n00000025 [label="{{} | D4XX rgb 12-001a\n/dev/v4l-subdev7 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000025:port0 -> n00000017:port2 [style=bold]
        n00000029 [label="{{} | D4XX imu 12-001a\n/dev/v4l-subdev8 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000029:port0 -> n00000017:port4 [style=bold]
        n0000002d [label="{{<port1> 1 | <port2> 2 | <port3> 3 | <port4> 4} | DS5 mux 13-001a\n/dev/v4l-subdev14 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000002d:port0 -> n000000a3:port0
        n00000033 [label="{{} | D4XX depth 13-001a\n/dev/v4l-subdev10 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000033:port0 -> n0000002d:port1 [style=bold]
        n00000037 [label="{{} | D4XX ir 13-001a\n/dev/v4l-subdev11 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000037:port0 -> n0000002d:port3 [style=bold]
        n0000003b [label="{{} | D4XX rgb 13-001a\n/dev/v4l-subdev12 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000003b:port0 -> n0000002d:port2 [style=bold]
        n0000003f [label="{{} | D4XX imu 13-001a\n/dev/v4l-subdev13 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000003f:port0 -> n0000002d:port4 [style=bold]
        n00000043 [label="{{<port1> 1 | <port2> 2 | <port3> 3 | <port4> 4} | DS5 mux 14-001a\n/dev/v4l-subdev19 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000043:port0 -> n000000b0:port0
        n00000049 [label="{{} | D4XX depth 14-001a\n/dev/v4l-subdev15 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000049:port0 -> n00000043:port1 [style=bold]
        n0000004d [label="{{} | D4XX ir 14-001a\n/dev/v4l-subdev16 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n0000004d:port0 -> n00000043:port3 [style=bold]
        n00000051 [label="{{} | D4XX rgb 14-001a\n/dev/v4l-subdev17 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000051:port0 -> n00000043:port2 [style=bold]
        n00000055 [label="{{} | D4XX imu 14-001a\n/dev/v4l-subdev18 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000055:port0 -> n00000043:port4 [style=bold]
        n00000059 [label="{{<port0> 0} | 13e00000.host1x:nvcsi@15a00000-\n/dev/v4l-subdev20 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000059:port1 -> n0000005c
        n0000005c [label="vi-output, DS5 mux 9-001a\n/dev/video0", shape=box, style=filled, fillcolor=yellow]
        n00000064 [label="tegra-capture-vi-metadata-0\n/dev/video1", shape=box, style=filled, fillcolor=yellow]
        n00000092 [label="{{<port0> 0} | 13e00000.host1x:nvcsi@15a00000-\n/dev/v4l-subdev21 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
        n00000092:port1 -> n00000095
        n00000095 [label="vi-output, DS5 mux 12-001a\n/dev/video2", shape=box, style=filled, fillcolor=yellow]
        n0000009d [label="tegra-capture-vi-metadata-0\n/dev/video3", shape=box, style=filled, fillcolor=yellow]
        n000000a3 [label="{{<port0> 0} | 13e00000.host1x:nvcsi@15a00000-\n/dev/v4l-subdev22 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
        n000000a3:port1 -> n000000a6
        n000000a6 [label="vi-output, DS5 mux 13-001a\n/dev/video4", shape=box, style=filled, fillcolor=yellow]
        n000000b0 [label="{{<port0> 0} | 13e00000.host1x:nvcsi@15a00000-\n/dev/v4l-subdev23 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
        n000000b0:port1 -> n000000b3
        n000000b3 [label="vi-output, DS5 mux 14-001a\n/dev/video5", shape=box, style=filled, fillcolor=yellow]
}

</details>

![image](https://github.com/dmipx/realsense_mipi_platform_driver/assets/104717350/dfb6ba23-b577-4a7a-8a7b-8d88391013a0)

---

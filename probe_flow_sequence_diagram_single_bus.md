# RealSense MIPI Platform Driver - Probe Flow and Video Device Registration - Multi - port design

## System Architecture Overview

### V4L2 Subdevice to I2C Address Mapping

This block diagram shows how each physical sensor is mapped to an I2C address and exposed as a V4L2 subdevice:

```mermaid
graph TB
    subgraph I2C_Addr["I2C MUX"]
        I2C_Bus_0x30["I2C Address 0x30"]
    end

    subgraph I2C_Out["I2C Output"]
        I2C_Out_0x30["I2C Address 0x30"]
        I2C_Out_0x33["I2C Address 0x33 (Unused)"]
        I2C_Out_0x34["I2C Address 0x34 (Unused)"]
        I2C_Out_0x35["I2C Address 0x35 (Unused)"]
    end

    subgraph I2C_Bus["I2C Bus"]
        I2C_Addr_0x30["I2C Address 0x30"]
    end

    subgraph DT["Device Tree"]
        DT_Depth["ds5_depth@0x30<br/>compatible='intel,d4xx'<br/>reg=0x1a<br/>cam-type='DS5'"]
    end

    subgraph Driver["D4xx Driver (d4xx.c)"]
        Probe_DS5["ds5_probe()<br/>instance"]
    end

    subgraph V4L2_Subdevs["V4L2 Subdevices"]
        V4L2_Depth["Depth Sensor Subdev<br/>/dev/video0<br/>Format: Z16"]
        V4L2_RGB["RGB Sensor Subdev<br/>/dev/video2<br/>Format: YUYV/RGB"]
        V4L2_IR["IR Sensor Subdev<br/>/dev/video4<br/>Format: Y8/Y16"]
        V4L2_IMU["IMU Sensor Subdev<br/>/dev/video6<br/>Format: Metadata"]
    end

    subgraph Hardware["DS5 Camera"]
        HW_Depth["DS5 Depth Sensor"]
        HW_RGB["DS5 RGB Sensor"]
        HW_IR["DS5 IR Sensor"]
        HW_IMU["DS5 IMU Sensor"]
    end

    %% Device Tree to I2C Address
    DT_Depth -.->|defines| I2C_Addr_0x30

    %% I2C to Driver Probe
    I2C_Addr_0x30 -->|I2C match| Probe_DS5

    %% Driver to V4L2 Subdevices
    Probe_DS5 -->|creates| V4L2_Depth
    Probe_DS5 -->|creates| V4L2_RGB
    Probe_DS5 -->|creates| V4L2_IR
    Probe_DS5 -->|creates| V4L2_IMU

    %% Hardware connection
    HW_Depth <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_RGB <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_IR <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_IMU <-->|I2C Reg 0x1a| I2C_Bus_0x30

    %% Mux Output
    I2C_Bus_0x30 <--> I2C_Out_0x30
    I2C_Bus_0x30 <--> I2C_Out_0x33
    I2C_Bus_0x30 <--> I2C_Out_0x34
    I2C_Bus_0x30 <--> I2C_Out_0x35
    
    style I2C_Out fill:#e1f5ff,color:#000000
    style DT fill:#fff4e1,color:#000000
    style Driver fill:#e8f5e9,color:#000000
    style V4L2_Subdevs fill:#f3e5f5,color:#000000
    style Hardware fill:#ffeaa7,color:#000000
```

### Key Concepts:

1. **One I2C Address for all physical Sensors**: The D4xx device is accessed through I2C address 0x30 regardless of stream
2. **One Probe per D4xx device**: The driver's `ds5_probe()` is called once
3. **One V4L2 Subdevice per supported stream**: Upon probe, a V4L2 subdevice node is created for each supported stream
5. **Device Tree Configuration**: The I2C addresses and sensor types are defined in the device tree

### Example Multi-Sensor - Multi port - Single i2c bus Configuration:

```dts
i2c@0 {
    /* D4xx device at address 0x10 */
    d4m@1a {
        compatible = "intel,d4xx";
        def-addr = <0x10>;
        /* reg addr 0x1a */
        reg = <0x1a>;
        vcc-supply = <&vdd_1v8>;
    };
    ports {
        port@0 {
            reg = <0>;
            cam-type = "Depth";
            d4m0_depth_out: endpoint {
                port-index = <0>;
                bus-width = <2>;
                remote-endpoint = <&csi_in0>;
                vc-id = <0>;
            };
            gmsl-link {
                src-csi-port = "b";
                dst-csi-port = "a";
                serdes-csi-link = "a";
                csi-mode = "1x4";
                st-vc = <0>;
                vc-id = <0>;
                num-lanes = <2>;
            };
        };
        port@1 {
            reg = <0>;
            cam-type = "RGB";
            d4m0_depth_out: endpoint {
                port-index = <0>;
                bus-width = <2>;
                remote-endpoint = <&csi_in0>;
                vc-id = <0>;
            };
            gmsl-link {
                src-csi-port = "b";
                dst-csi-port = "a";
                serdes-csi-link = "a";
                csi-mode = "1x4";
                st-vc = <0>;
                vc-id = <1>;
                num-lanes = <2>;
            };
        };
        ...
    }
};
```

### Result in User Space:

```bash
# Each sensor appears as separate V4L2 subdevices
/dev/video0  # Depth sensor stream
/dev/video1  # Depth metadata stream
/dev/video2  # RGB sensor stream
/dev/video3  # RGB metadata stream
/dev/video4  # IR sensor stream
/dev/video5  # IR metadata stream
/dev/video6  # IMU sensor stream

# Media controller shows the complete topology
media-ctl -p -d /dev/media0
```

---

## Phase 1: Initial Probe and Power Setup
```mermaid
sequenceDiagram
    participant I2C as I2C Subsystem
    participant Probe as ds5_probe()
    participant DT as Device Tree
    participant Regmap as I2C Regmap
    I2C->>Probe: I2C device match (compatible="intel,d4xx")
    activate Probe
    Probe->>Probe: Allocate struct ds5 (devm_kzalloc)
    Probe->>Probe: Initialize mutex (state->lock)
    Probe->>Probe: Store I2C client reference
    Probe->>Probe: Select device variant from id->driver_data
    Probe->>DT: devm_regulator_get(&dev, "vcc")
    DT-->>Probe: VCC regulator handle
    Probe->>Probe: regulator_enable(state->vcc)
    Probe->>Regmap: devm_regmap_init_i2c(client, config)
    Note right of Regmap: Register: 16-bit<br/>Value: 8-bit
    Regmap-->>Probe: Regmap handle
```

### What is I2C Device Match?

The "I2C device match" is the process where the Linux kernel’s I2C subsystem detects a device on the I2C bus that matches a driver’s compatible string, and then calls the driver's `probe` function.

#### How it works:
1. **Device Tree/Board File**: The device is described in the device tree, for example:
    ```dts
        i2c@0 { //D4xx device
				status = "ok";
				reg = <0x3>; 	// line 18-19 of the I2C switch
				#address-cells = <0x1>;
				i2c-mux,deselect-on-exit;
				#size-cells = <0x0>;

				d4m0: d4m@10 {
					status = "ok";
					reg = <0x10>;
					compatible = "intel,d4xx";
					vcc-supply = <&p2822_vdd_1v8_cvb>;
					/*reset-gpios = <&tegra_main_gpio CAM0_RST_L GPIO_ACTIVE_HIG>;*/
					ports {
						#address-cells = <1>;
						#size-cells = <0>;

						port@0 {
							reg = <0>;
							d4m1_out: endpoint {
								port-index = <1>;
								bus-width = <2>;
								vc-id = <0>;
								remote-endpoint = <&csi_in1>;
							};
						};
						...
					};
    ```
2. **Driver Table**: The driver (in `d4xx.c`) provides an `i2c_device_id` table and/or an `of_match_table`:
    ```c
    static const struct of_device_id d4xx_of_match[] = {
        { .compatible = "intel,d4xx" },
        { }
    };
    MODULE_DEVICE_TABLE(of, d4xx_of_match);
    ```
3. **Match**: When the kernel boots, it matches the device tree node to the driver’s table. If matched, it calls the driver's `probe` function (here, `ds5_probe()`).

**Summary:**
The I2C device match is the automatic association of a physical I2C device (described in hardware and device tree) with its Linux driver, triggering the driver’s initialization code.
Current implementation will have 4 d4xx instances (depth, rgb, Y8, IMU)

## Phase 2: SerDes and Device Tree Parsing
```mermaid
sequenceDiagram
    participant Probe as ds5_probe()
    participant SerDes as SerDes Setup
    participant DT as Device Tree
    Probe->>SerDes: ds5_serdes_setup(state)
    activate SerDes
    SerDes->>DT: ds5_board_setup(state)
    activate DT
    loop For all ports
        DT->>DT: Parse device properties:<br/>- reg (I2C address)<br/>- def-addr (FW address)<br/>- cam-type (Depth/RGB/Y8/IMU)
        DT->>DT: Parse gmsl-link node:<br/>- dst-csi-port<br/>- src-csi-port<br/>- csi-mode<br/>- vc-id, st-vc<br/>- num-lanes
        DT->>DT: Find serializer/deserializer nodes
    end
    DT-->>SerDes: Board configuration
    deactivate DT
    SerDes->>SerDes: max9295_sdev_pair(sensor, serializer)
    SerDes->>SerDes: max9296_sdev_register(sensor, deserializer)
    SerDes->>SerDes: ds5_gmsl_serdes_setup()
    SerDes->>SerDes: max9295_init_settings()
    SerDes->>SerDes: max9296_init_settings()
    SerDes-->>Probe: SerDes configured
    deactivate SerDes
```

### What is SerDes Setup?

SerDes (Serializer/Deserializer) setup configures the GMSL (Gigabit Multimedia Serial Link) pipeline for transmitting camera data over long distances using differential signaling. This is essential for automotive and embedded applications where cameras are physically separated from the processing unit.

#### How it works:
1. **Device Tree Parsing**: The driver reads GMSL configuration from the device tree's `gmsl-link` node
2. **Serializer Pairing**: Associates the D4xx camera sensor with a MAX9295 serializer chip
3. **Deserializer Registration**: Registers the sensor with a MAX9296 deserializer on the host side
4. **Pipeline Configuration**: Sets up the complete data path including CSI lanes, virtual channels, and data rates

#### Example Device Tree Configuration:
```dts
d4m@10 {
    compatible = "intel,d4xx";
    reg = <0x10>;

    ports{
        port@0{ // Depth
            cam-type = "Depth";
            gmsl-link {
                dst-csi-port = "a";           // CSI port on deserializer side
                src-csi-port = "0";           // CSI port on serializer side  
                csi-mode = "1x4";             // 1 camera using 4 lanes
                serdes-csi-link = "a";        // GMSL link identifier
                st-vc = <0>;                  // Starting virtual channel
                vc-id = <0>;                  // Virtual channel ID for this sensor
                num-lanes = <2>;              // Number of MIPI CSI-2 lanes
            };
        }
    }
};
```

#### Key Functions:
- **`max9295_sdev_pair()`**: Links sensor to serializer (camera side)
- **`max9296_sdev_register()`**: Registers with deserializer (host side)
- **`ds5_gmsl_serdes_setup()`**: Configures complete GMSL pipeline
- **`max9295_init_settings()`**: Initializes serializer registers
- **`max9296_init_settings()`**: Initializes deserializer registers

**Summary:**
SerDes setup enables the RealSense camera to transmit high-bandwidth video data over long cable lengths using GMSL technology, typically used in automotive and industrial applications.

## Phase 3: Hardware Communication Verification
```mermaid
sequenceDiagram
    participant Probe as ds5_probe()
    participant HW as Hardware Registers
    participant DT as Device Tree
    Probe->>HW: Read register 0x5020 (retry up to 5 times)
    HW-->>Probe: Device response
    loop For all ports
        Probe->>DT: Read "cam-type" property
        DT-->>Probe: Camera type string
        Probe->>Probe: Set flags:<br/>state->is_depth<br/>state->is_rgb<br/>state->is_y8<br/>state->is_imu
    end
```

### What is Hardware Communication Verification?

This phase ensures the D4xx camera hardware is accessible via I2C and properly responding to register reads. It also determines the camera type from the device tree to configure the appropriate sensor functionality.

#### How it works:
1. **Initial I2C Test**: Reads a known register (0x5020) to verify I2C communication
2. **Retry Mechanism**: Attempts up to 5 times to handle timing issues during power-up
3. **Camera Type Detection**: Reads the `cam-type` property from device tree
4. **State Flags**: Sets internal flags based on camera type for later initialization

#### Example Code Flow:
```c
// Read register 0x5020 to verify device is responding
for (i = 0; i < 5; i++) {
    ret = regmap_read(state->regmap, 0x5020, &val);
    if (ret == 0)
        break;
    msleep(100);  // Wait 100ms before retry
}

// Read cam-type for each port from device tree
ports = of_get_child_by_name(node, "ports");

struct device_node *child = NULL;
struct device_node *port = NULL;
do
{
    child = of_get_next_child(ports, child);
    if (child != NULL)
    {
        err = of_property_read_string(port, "cam-type", &str_value);
        // Set appropriate flags
        ...
    }
} while (child != NULL);

```

#### Register 0x5020:
This is a status/identification register that:
- Returns a valid value when device is operational
- Returns 0x201 when device is in DFU (firmware update) recovery mode
- Used as the first hardware handshake test

**Summary:**
This phase validates I2C connectivity to the camera hardware and iterates over the sensor types, ensuring the driver initializes the correct functionalities (depth processing, RGB capture, IR streaming, or IMU data).

## Phase 4: DFU Character Device (Depth only)
```mermaid
sequenceDiagram
    participant Probe as ds5_probe()
    alt is_depth == true
        Probe->>Probe: ds5_chrdev_init(client, state)
        Probe->>Probe: Allocate char device region
        Probe->>Probe: Create device class "d4xx"
        Probe->>Probe: Initialize cdev with ds5_device_file_ops
        Note right of Probe: Creates /dev/d4xx-dfu-X-XXXX
    end
```

## Phase 5: Recovery Mode Check
```mermaid
sequenceDiagram
    participant I2C as I2C Bus
    participant Probe as ds5_probe()
    participant HW as Hardware Registers
    Probe->>HW: Read register 0x5020
    HW-->>Probe: Device status
    alt status == 0x201 (DFU Recovery)
        Probe->>Probe: Set dfu_state_flag = DS5_DFU_RECOVERY
        Probe-->>I2C: Return success (wait for FW update)
        Note right of Probe: Exit probe early
    end
```

## Phase 6: Firmware Version Detection
```mermaid
sequenceDiagram
    participant Probe as ds5_probe()
    participant HW as Hardware Registers
    Probe->>HW: Read DS5_FW_VERSION (0x030C)
    HW-->>Probe: Firmware version
    Probe->>HW: Read DS5_FW_BUILD (0x030E)
    HW-->>Probe: Build number
    Probe->>Probe: Log sensor name and FW version
```

## Phase 7: V4L2 Subsystem Initialization
```mermaid
sequenceDiagram
    participant Probe as ds5_probe()
    participant V4L2 as V4L2 Subsystem
    participant HW as Hardware Registers
    Probe->>V4L2: ds5_v4l_init(client, state)
    activate V4L2
    V4L2->>V4L2: ds5_parse_cam(client, state)
    V4L2->>HW: ds5_fixed_configuration()
    HW->>HW: Read DS5_DEVICE_TYPE (0x0310)
    HW-->>V4L2: Device type (D41X/D43X/D45X/D46X)
    V4L2->>V4L2: Set format arrays based on device type
    V4L2->>V4L2: ds5_sensor_format_init(depth)
    V4L2->>V4L2: ds5_sensor_format_init(ir)
    V4L2->>V4L2: ds5_sensor_format_init(rgb)
    V4L2->>V4L2: ds5_sensor_format_init(imu)
    deactivate V4L2
```

### What is V4L2 Subsystem Initialization?

V4L2 (Video for Linux 2) subsystem initialization prepares the camera driver to integrate with the Linux video capture framework. This phase reads the hardware configuration and sets up the supported video formats for each sensor type.

#### How it works:
1. **Entry Point**: `ds5_v4l_init()` is called from the probe function
2. **Hardware Detection**: Reads the device type register (0x0310) to identify the specific D4xx model
3. **Format Configuration**: Based on the detected model, assigns the appropriate format arrays to each sensor
4. **Sensor Format Init**: Initializes format lists for depth, IR, RGB, and IMU sensors

#### Device Type Detection:
```c
// Read DS5_DEVICE_TYPE register (0x0310)
ret = regmap_read(state->regmap, DS5_DEVICE_TYPE, &device_type);

switch (device_type) {
    case DS5_DEVICE_TYPE_D41X:  // D415/D410
        // Configure for D41X formats
        break;
    case DS5_DEVICE_TYPE_D43X:  // D435/D435i
        // Configure for D43X formats
        break;
    case DS5_DEVICE_TYPE_D45X:  // D455/D457
        // Configure for D45X formats
        break;
    case DS5_DEVICE_TYPE_D46X:  // D465
        // Configure for D46X formats
        break;
}
```

#### Format Arrays by Sensor Type:

**Depth Sensor Formats (example):**
```c
static const struct ds5_format ds5_depth_formats[] = {
    { .pixelformat = V4L2_PIX_FMT_Z16,   .depth = 16 },  // 16-bit depth
    { .pixelformat = V4L2_PIX_FMT_UYVY,  .depth = 16 },  // YUV format
};
```

**RGB Sensor Formats (example):**
```c
static const struct ds5_format ds5_rgb_formats[] = {
    { .pixelformat = V4L2_PIX_FMT_YUYV,  .depth = 16 },  // YUV 4:2:2
    { .pixelformat = V4L2_PIX_FMT_UYVY,  .depth = 16 },  // YUV 4:2:2
    { .pixelformat = V4L2_PIX_FMT_RGB565, .depth = 16 },  // RGB 5:6:5
};
```

**IR Sensor Formats (example):**
```c
static const struct ds5_format ds5_ir_formats[] = {
    { .pixelformat = V4L2_PIX_FMT_GREY,  .depth = 8 },   // Y8 grayscale
    { .pixelformat = V4L2_PIX_FMT_Y10,   .depth = 10 },  // Y10 format
    { .pixelformat = V4L2_PIX_FMT_Y12,   .depth = 12 },  // Y12 format
    { .pixelformat = V4L2_PIX_FMT_Y16,   .depth = 16 },  // Y16 format
};
```

#### Resolution Configuration:
```c
static const struct ds5_resolution ds5_sizes[] = {
    { .width = 1280, .height = 720,  .fps = 30 },
    { .width = 848,  .height = 480,  .fps = 60 },
    { .width = 640,  .height = 480,  .fps = 90 },
    { .width = 640,  .height = 360,  .fps = 90 },
    { .width = 480,  .height = 270,  .fps = 90 },
    { .width = 424,  .height = 240,  .fps = 90 },
};
```

**Summary:**
This phase detects the specific D4xx camera model and configures the supported video formats, resolutions, and frame rates for each sensor type (depth, RGB, IR, IMU), preparing the driver for V4L2 video capture operations.

## Phase 8: Individual Sensor Subdevice Initialization
```mermaid
sequenceDiagram
    participant V4L2 as V4L2 Subsystem
    participant Sensors as Sensor Subdevices
    participant Media as Media Controller
    V4L2->>Sensors: ds5_depth_init(client, state)
    activate Sensors
    Sensors->>Sensors: ds5_sensor_init(..., "depth")
    Sensors->>Sensors: v4l2_i2c_subdev_init(&sensor->sd)
    Sensors->>Sensors: Set mux_pad = DS5_MUX_PAD_DEPTH
    Sensors->>Sensors: Set subdev ops (pad, video)
    Sensors->>Media: media_entity_pads_init(1 pad, SOURCE)
    Sensors-->>V4L2: Depth sensor initialized
    deactivate Sensors
    V4L2->>Sensors: ds5_ir_init(client, state)
    activate Sensors
    Sensors->>Sensors: ds5_sensor_init(..., "ir")
    Sensors->>Sensors: Set mux_pad = DS5_MUX_PAD_IR
    Sensors->>Media: media_entity_pads_init(1 pad, SOURCE)
    Sensors-->>V4L2: IR sensor initialized
    deactivate Sensors
    V4L2->>Sensors: ds5_rgb_init(client, state)
    activate Sensors
    Sensors->>Sensors: ds5_sensor_init(..., "rgb")
    Sensors->>Sensors: Set mux_pad = DS5_MUX_PAD_RGB
    Sensors->>Media: media_entity_pads_init(1 pad, SOURCE)
    Sensors-->>V4L2: RGB sensor initialized
    deactivate Sensors
    V4L2->>Sensors: ds5_imu_init(client, state)
    activate Sensors
    Sensors->>Sensors: ds5_sensor_init(..., "imu")
    Sensors->>Sensors: Set mux_pad = DS5_MUX_PAD_IMU
    Sensors->>Media: media_entity_pads_init(1 pad, SOURCE)
    Sensors-->>V4L2: IMU sensor initialized
    deactivate Sensors
```

### What is Individual Sensor Subdevice Initialization?

Each sensor (depth, IR, RGB, IMU) is initialized as a separate V4L2 subdevice. This creates independent video sources that can be configured and controlled separately. The V4L2 subsystem doesn't call these subdevices during initialization—it registers them for later use during video streaming operations.

#### How it works:
1. **Subdevice Creation**: Each sensor gets its own `v4l2_subdev` structure
2. **Operations Registration**: Subdevice operations (callbacks) are registered for later use
3. **Media Pad Setup**: Each sensor gets one SOURCE pad for the media controller framework
4. **MUX Pad Assignment**: Each sensor is assigned a corresponding pad on the MUX subdevice

#### When does V4L2 call the sensor subdevice?

The V4L2 subsystem calls sensor subdevice operations in response to **user space requests** and **streaming events**, NOT during initialization:

**1. Format Negotiation (User opens video device):**
```c
// User space calls: ioctl(fd, VIDIOC_S_FMT, &fmt)
// V4L2 framework calls:
static int ds5_sensor_set_fmt(struct v4l2_subdev *sd,
                             struct v4l2_subdev_pad_config *cfg,
                             struct v4l2_subdev_format *fmt)
{
    // Validate and set format (resolution, pixel format)
    // Called when application sets video format
}
```

**2. Streaming Start/Stop (User starts capture):**
```c
// User space calls: ioctl(fd, VIDIOC_STREAMON, &type)
// V4L2 framework propagates through media pipeline:
// Platform driver (Tegra VI) -> MUX -> Active Sensor

static int ds5_sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
    if (enable) {
        // Configure hardware registers
        // Start camera streaming
        ds5_configure(state);
        // Write to DS5_START_STOP_STREAM register
    } else {
        // Stop camera streaming
    }
}
```

**3. Control Changes (User adjusts camera settings):**
```c
// User space calls: ioctl(fd, VIDIOC_S_CTRL, &control)
// V4L2 framework calls:
static int ds5_s_ctrl(struct v4l2_ctrl *ctrl)
{
    switch (ctrl->id) {
        case V4L2_CID_EXPOSURE_ABSOLUTE:
            // Set exposure time
            break;
        case V4L2_CID_ANALOGUE_GAIN:
            // Set sensor gain
            break;
        // ... other controls
    }
}
```

**4. Frame Interval Configuration:**
```c
// User space calls: ioctl(fd, VIDIOC_S_PARM, &parm)
// V4L2 framework calls:
static int ds5_sensor_s_frame_interval(struct v4l2_subdev *sd,
                                       struct v4l2_subdev_frame_interval *fi)
{
    // Set frame rate (fps)
}
```

#### Subdevice Operations Structure:
```c
static const struct v4l2_subdev_video_ops ds5_sensor_video_ops = {
    .s_stream = ds5_sensor_s_stream,           // Called on stream start/stop
    .g_frame_interval = ds5_sensor_g_frame_interval,  // Get frame rate
    .s_frame_interval = ds5_sensor_s_frame_interval,  // Set frame rate
};

static const struct v4l2_subdev_pad_ops ds5_sensor_pad_ops = {
    .enum_mbus_code = ds5_sensor_enum_mbus_code,      // Enumerate formats
    .enum_frame_size = ds5_sensor_enum_frame_size,    // Enumerate resolutions
    .enum_frame_interval = ds5_sensor_enum_frame_interval, // Enumerate fps
    .get_fmt = ds5_sensor_get_fmt,                    // Get current format
    .set_fmt = ds5_sensor_set_fmt,                    // Set format
};

static const struct v4l2_subdev_ops ds5_sensor_subdev_ops = {
    .video = &ds5_sensor_video_ops,
    .pad = &ds5_sensor_pad_ops,
};
```

#### Example Timeline - When Operations Are Called:

```
Initialization (Phase 8):
    └─> ds5_depth_init()
        └─> ds5_sensor_init()
            └─> v4l2_i2c_subdev_init(&sensor->sd, &ds5_sensor_subdev_ops)
                └─> Registers operations, but DOESN'T call them

User Space Application Flow:
    1. open("/dev/video0")               → No sensor callbacks yet
    2. ioctl(VIDIOC_ENUM_FMT)            → .enum_mbus_code() called
    3. ioctl(VIDIOC_ENUM_FRAMESIZES)     → .enum_frame_size() called
    4. ioctl(VIDIOC_S_FMT)               → .set_fmt() called
    5. ioctl(VIDIOC_S_PARM)              → .s_frame_interval() called
    6. ioctl(VIDIOC_S_CTRL) [exposure]   → ds5_s_ctrl() called
    7. ioctl(VIDIOC_STREAMON)            → .s_stream(1) called ★ HARDWARE STARTS
    8. [Video frames flowing...]
    9. ioctl(VIDIOC_STREAMOFF)           → .s_stream(0) called ★ HARDWARE STOPS
```

#### Each Sensor's Role:

**Depth Sensor Subdevice:**
- Provides Z16 (16-bit depth) data
- Connected to MUX pad `DS5_MUX_PAD_DEPTH`
- Controlled via `/dev/videoX`

**IR Sensor Subdevice:**
- Provides Y8/Y16 infrared data
- Connected to MUX pad `DS5_MUX_PAD_IR`
- Shares laser projector with depth sensor

**RGB Sensor Subdevice:**
- Provides color video (YUYV, RGB formats)
- Connected to MUX pad `DS5_MUX_PAD_RGB`
- Independent color camera sensor

**IMU Sensor Subdevice:**
- Provides accelerometer and gyroscope data
- Connected to MUX pad `DS5_MUX_PAD_IMU`
- Different data path (metadata, not video frames)

**Summary:**
Phase 8 creates and registers four independent V4L2 subdevices but doesn't invoke their operations. The V4L2 framework calls these subdevice operations later when user space applications open video devices, configure formats, adjust controls, or start/stop streaming. The actual hardware configuration and streaming happens during these runtime callbacks, not during initialization.

## Phase 9: MUX Subdevice Initialization
```mermaid
sequenceDiagram
    participant V4L2 as V4L2 Subsystem
    participant Mux as MUX Subdevice
    participant Media as Media Controller
    V4L2->>Mux: ds5_mux_init(client, state)
    activate Mux
    Mux->>Mux: v4l2_subdev_init(&mux->sd, &ds5_mux_subdev_ops)
    Mux->>Mux: Set name: "DS5 mux X-XXXX"
    Mux->>Mux: Set flags: V4L2_SUBDEV_FL_HAS_DEVNODE
    Mux->>Mux: Set entity function: MEDIA_ENT_F_CAM_SENSOR
    Mux->>Mux: Initialize 4 pads:<br/>- DS5_MUX_PAD_DEPTH (SINK)<br/>- DS5_MUX_PAD_RGB (SINK)<br/>- DS5_MUX_PAD_IR (SINK)<br/>- DS5_MUX_PAD_IMU (SINK)
    Mux->>Media: media_entity_pads_init(4 pads)
    Media-->>Mux: Pads initialized
    Mux-->>V4L2: MUX initialized
    deactivate Mux
```

### What is MUX Subdevice Initialization?

The MUX (multiplexer) subdevice acts as a central routing hub in the media controller topology, managing the four sensor streams (depth, RGB, IR, IMU) as separate data paths. During initialization, the MUX is configured as a V4L2 subdevice with four SOURCE pads—one for each sensor type—allowing the media framework to route video data from individual sensors to the platform's video capture hardware. The MUX subdevice is named using the I2C adapter ID and address (e.g., "DS5 mux 0-001a") and is marked with `V4L2_SUBDEV_FL_HAS_DEVNODE` to expose it as a device node in `/dev/v4l-subdevX`. All four pads are initialized as SOURCE pads using `media_entity_pads_init()`, establishing the foundation for later linking individual sensor subdevices to their corresponding MUX pads. This architecture enables the media controller to manage each sensor independently while presenting a unified camera device to user space.

## Phase 10: Control Handler Initialization
```mermaid
sequenceDiagram
    participant Mux as MUX Subdevice
    Mux->>Mux: ds5_ctrl_init(state, DS5_MUX_PAD_EXTERNAL)
    Note right of Mux: MUX controls
    Mux->>Mux: ds5_ctrl_init(state, DS5_MUX_PAD_DEPTH)
    Note right of Mux: Depth controls:<br/>- Laser power<br/>- Exposure<br/>- Auto-exposure<br/>- Gain
    Mux->>Mux: ds5_ctrl_init(state, DS5_MUX_PAD_RGB)
    Note right of Mux: RGB controls:<br/>- Exposure<br/>- Auto-exposure<br/>- Gain<br/>- White balance
    Mux->>Mux: ds5_ctrl_init(state, DS5_MUX_PAD_IR)
    Note right of Mux: IR controls:<br/>- Laser power<br/>- Exposure<br/>- Auto-exposure
    Mux->>Mux: ds5_ctrl_init(state, DS5_MUX_PAD_IMU)
    Note right of Mux: IMU controls
    alt CONFIG_TEGRA_CAMERA_PLATFORM
        Mux->>Mux: camera_common_initialize(&mux->sd, "d4xx")
        Note right of Mux: Tegra camera<br/>framework integration
    end
```

### What is Control Handler Initialization?

Control handlers provide the V4L2 control interface that allows user space applications to adjust camera parameters like exposure, gain, and laser power through standard V4L2 ioctls. The driver creates separate control handlers for each sensor type (MUX, depth, RGB, IR, IMU) using `ds5_ctrl_init()`, with each handler managing sensor-specific controls—for example, depth and IR sensors get laser power controls, while RGB gets white balance controls. Each control handler is initialized with `v4l2_ctrl_handler_init()` and populated with both standard V4L2 controls (like `V4L2_CID_EXPOSURE_ABSOLUTE` and `V4L2_CID_ANALOGUE_GAIN`) and custom RealSense-specific controls (like calibration data retrieval, hardware monitor commands, and firmware version queries). The controls are marked with flags like `V4L2_CTRL_FLAG_VOLATILE` (value can change without driver intervention) and `V4L2_CTRL_FLAG_EXECUTE_ON_WRITE` (control takes effect immediately), ensuring proper synchronization between hardware state and user space. Finally, each sensor's subdevice has its `ctrl_handler` pointer set to its corresponding handler, enabling the V4L2 framework to route control operations to the correct hardware registers via the `ds5_ctrl_ops` callback functions.

## Phase 11: Hardware Initialization
```mermaid
sequenceDiagram
    participant V4L2 as V4L2 Subsystem
    participant HW as Hardware Registers
    V4L2->>HW: ds5_hw_init(client, state)
    activate HW
    HW->>HW: Read MIPI capabilities:<br/>- Number of lanes<br/>- PHY config<br/>- Data rate min/max
    HW->>HW: Write DS5_MIPI_LANE_NUMS (0x0400)<br/>Value: num_lanes - 1
    HW->>HW: Write DS5_MIPI_LANE_DATARATE (0x0402)<br/>Value: 1000 Mbps (default)
    HW->>HW: Read DS5_MIPI_CONF_STATUS (0x0500)
    HW-->>V4L2: Hardware configured
    deactivate HW
```

### What is Hardware Initialization?

Hardware initialization configures the D4xx camera's MIPI CSI-2 interface to match the platform's video capture capabilities, ensuring proper data transmission between the camera and the host processor. The driver first reads the camera's MIPI capabilities from hardware registers (0x0300-0x030A) to determine supported lane counts, PHY configuration, and data rate ranges (typically 100-1500 Mbps). Based on the platform configuration—either from the Tegra camera platform (which provides lane count via `state->mux.sd.numlanes`) or a default of 2 lanes for other platforms—the driver writes the lane configuration to register `DS5_MIPI_LANE_NUMS` (0x0400) with the value decremented by one (e.g., 4 lanes = 0x03). The MIPI data rate is set to a default of 1000 Mbps by writing to register `DS5_MIPI_LANE_DATARATE` (0x0402), which can be adjusted based on platform requirements or SerDes configuration. Finally, the driver reads the MIPI configuration status register (0x0500) to verify that the hardware has successfully applied the settings, completing the low-level interface setup required for video streaming.

## Phase 12: MUX Registration (Async)
```mermaid
sequenceDiagram
    participant I2C as I2C Bus
    participant Probe as ds5_probe()
    participant V4L2 as V4L2 Subsystem
    participant Media as Media Controller
    V4L2->>V4L2: ds5_mux_register(client, state)
    V4L2->>Media: v4l2_async_register_subdev(&mux->sd)
    Note right of Media: Registers MUX as<br/>async subdevice
    Media-->>V4L2: Registration queued
    V4L2-->>Probe: V4L2 initialization complete
    Probe->>Probe: Create sysfs attributes:<br/>- ds5_fw_ver<br/>- ds5_read_reg<br/>- ds5_write_reg
    Probe-->>I2C: Probe complete (success)
```

### What is MUX Registration (Async)?

Asynchronous subdevice registration is a kernel mechanism that allows camera drivers to register with the V4L2 subsystem before the platform's video capture driver is ready, avoiding probe ordering dependencies. The `ds5_mux_register()` function calls `v4l2_async_register_subdev()` to add the MUX subdevice to the async notifier framework, which queues it for later binding when the platform video driver (e.g., Tegra VI or Intel IPU6) becomes available. This registration is non-blocking—the probe function returns successfully immediately after registration, allowing the kernel to continue booting without waiting for the complete video pipeline to be established. After registration, the probe function creates sysfs attributes under `/sys/bus/i2c/devices/X-XXXX/` for debugging purposes, including `ds5_fw_ver` (firmware version), `ds5_read_reg` (register read interface), and `ds5_write_reg` (register write interface), providing diagnostic access to the camera hardware. The actual connection of the MUX to the video capture pipeline happens asynchronously in Phase 13 when the V4L2 async framework triggers the `ds5_mux_registered()` callback.

## Phase 13: Async Callback - Sensor Registration and Linking
```mermaid
sequenceDiagram
    participant Media as Media Controller
    participant Mux as MUX Subdevice
    participant Sensors as Sensor Subdevices
    Media->>Media: Async framework triggers callback
    Media->>Mux: ds5_mux_registered(v4l2_subdev, cfg)
    activate Mux
    Mux->>Sensors: ds5_sensor_register(state, &depth.sensor)
    activate Sensors
    Sensors->>Media: v4l2_device_register_subdev(&depth->sd)
    Media-->>Sensors: Depth subdev registered
    Note right of Media: Creates /dev/videoX
    Sensors->>Media: media_create_pad_link(<br/>  depth.pad[0],<br/>  mux.pad[DS5_MUX_PAD_DEPTH],<br/>  IMMUTABLE | ENABLED)
    Media-->>Sensors: Link created
    Sensors-->>Mux: Depth sensor registered and linked
    deactivate Sensors
    Mux->>Sensors: ds5_sensor_register(state, &ir.sensor)
    activate Sensors
    Sensors->>Media: v4l2_device_register_subdev(&ir->sd)
    Media-->>Sensors: IR subdev registered
    Sensors->>Media: media_create_pad_link(<br/>  ir.pad[0],<br/>  mux.pad[DS5_MUX_PAD_IR],<br/>  IMMUTABLE | ENABLED)
    Media-->>Sensors: Link created
    Sensors-->>Mux: IR sensor registered and linked
    deactivate Sensors
    Mux->>Sensors: ds5_sensor_register(state, &rgb.sensor)
    activate Sensors
    Sensors->>Media: v4l2_device_register_subdev(&rgb->sd)
    Media-->>Sensors: RGB subdev registered
    Sensors->>Media: media_create_pad_link(<br/>  rgb.pad[0],<br/>  mux.pad[DS5_MUX_PAD_RGB],<br/>  IMMUTABLE | ENABLED)
    Media-->>Sensors: Link created
    Sensors-->>Mux: RGB sensor registered and linked
    deactivate Sensors
    Mux->>Sensors: ds5_sensor_register(state, &imu.sensor)
    activate Sensors
    Sensors->>Media: v4l2_device_register_subdev(&imu->sd)
    Media-->>Sensors: IMU subdev registered
    Sensors->>Media: media_create_pad_link(<br/>  imu.pad[0],<br/>  mux.pad[DS5_MUX_PAD_IMU],<br/>  IMMUTABLE | ENABLED)
    Media-->>Sensors: Link created
    Sensors-->>Mux: IMU sensor registered and linked
    deactivate Sensors
    Mux-->>Media: All sensors registered
    deactivate Mux
    Note over Media: Video device nodes created by<br/>platform driver (Tegra VI / Intel IPU6)
```

### What is Async Callback - Sensor Registration and Linking?

The async callback phase completes the video pipeline topology when the platform video driver binds to the D4xx camera, triggering the `ds5_mux_registered()` callback registered in Phase 12. This callback sequentially registers all four sensor subdevices (depth, IR, RGB, IMU) with the V4L2 device using `v4l2_device_register_subdev()`, which adds each sensor to the media controller graph and makes them visible to user space. For each sensor, the driver creates an immutable media link using `media_create_pad_link()` with flags `MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED`, connecting the sensor's SOURCE pad (pad 0) to its corresponding MUX SINK pad (e.g., depth sensor → `DS5_MUX_PAD_DEPTH`), establishing the data flow path. These links are marked immutable and always enabled because the hardware topology is fixed—each sensor can only output to its designated MUX pad, and the routing cannot be changed at runtime. After all sensors are registered and linked, the platform video driver (Tegra VI or Intel IPU6) creates the final video device nodes (`/dev/videoX`) that user space applications use to capture video, completing the integration of the RealSense camera into the Linux V4L2/media controller framework.

## Device Tree Integration

The driver parses critical device tree properties during initialization:

### Device Node Properties
```dts
d4xx@1a {
    compatible = "intel,d4xx";
    reg = <0x1a>;              // I2C slave address
    def-addr = <0x10>;         // Default firmware address
    vcc-supply = <&battery_reg>;
};
```

### GMSL Link Configuration (SerDes Mode)
```dts
gmsl-link {
    dst-csi-port = "a";        // Destination CSI port
    src-csi-port = "0";        // Source CSI port
    csi-mode = "1x4";          // CSI mode: 1x4, 2x4, 4x2, 2x2
    serdes-csi-link = "a";     // SerDes CSI link
    st-vc = <0>;               // Start virtual channel
    vc-id = <0>;               // Virtual channel ID
    num-lanes = <4>;           // Number of MIPI lanes
};
```

## Media Controller Graph

The final media controller topology:

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│ Depth Sensor│────▶│             │     │             │
│   subdev    │     │             │     │   CSI-2 RX  │
└─────────────┘     │             │     │   (Platform │
                    │             │     │    Driver)  │
┌─────────────┐     │             │     │             │
│  IR Sensor  │────▶│  DS5 MUX    │────▶│             │
│   subdev    │     │   subdev    │     └─────────────┘
└─────────────┘     │             │            │
                    │  (4 pads)   │            ▼
┌─────────────┐     │             │     ┌─────────────┐
│ RGB Sensor  │────▶│             │     │  Video Input│
│   subdev    │     │             │     │  Processor  │
└─────────────┘     │             │     │  (VI/IPU6)  │
                    │             │     └─────────────┘
┌─────────────┐     │             │            │
│ IMU Sensor  │────▶│             │            ▼
│   subdev    │     │             │     ┌─────────────┐
└─────────────┘     └─────────────┘     │ /dev/videoX │
                                        └─────────────┘
```

## Key Data Structures

### Main State Structure
```c
struct ds5 {
    struct {
        struct ds5_sensor sensor;
    } depth, ir, rgb, imu;
    
    struct {
        struct ds5_mux_subdev sd;
        struct media_pad pads[DS5_MUX_PAD_COUNT];  // 4 pads
        struct ds5_sensor *last_set;
    } mux;
    
    struct ds5_ctrls ctrls;
    struct ds5_dfu_dev dfu_dev;
    struct i2c_client *client;
    struct regmap *regmap;
    struct regulator *vcc;
    
    // SerDes members
    struct gmsl_link_ctx g_ctx;
    struct device *ser_dev;
    struct device *dser_dev;
    
    // Flags
    bool is_depth;
    bool is_rgb;
    bool is_y8;
    bool is_imu;
};
```

### Sensor Structure
```c
struct ds5_sensor {
    struct v4l2_subdev sd;
    struct media_pad pad;           // Single SOURCE pad
    struct v4l2_mbus_framefmt format;
    u16 mux_pad;                    // Corresponding mux pad
    
    struct {
        const struct ds5_format *format;
        const struct ds5_resolution *resolution;
        u16 framerate;
    } config;
    
    bool streaming;
    const struct ds5_format *formats;
    unsigned int n_formats;
    int pipe_id;                    // SerDes pipe ID
};
```

### MUX Pads
```c
enum {
    DS5_MUX_PAD_DEPTH,         // SINK - from depth sensor
    DS5_MUX_PAD_RGB,           // SINK - from RGB sensor
    DS5_MUX_PAD_IR,            // SINK - from IR sensor
    DS5_MUX_PAD_IMU,           // SINK - from IMU sensor
    DS5_MUX_PAD_COUNT
};
```

## Error Handling and Recovery

### Probe Failures
1. **Regulator not available** → Return error, probe fails
2. **I2C communication timeout** → Retry up to 5 times, then fail
3. **SerDes devices not ready** → Return `-EPROBE_DEFER`, kernel will retry
4. **Recovery mode detected (0x201)** → Exit probe successfully, create DFU device only

### Runtime Errors
1. **Streaming timeout** → Send stop command, return `-EAGAIN`
2. **Invalid format requested** → Return `-EINVAL`
3. **SerDes link failure** → Release resources, return error to user space

# Side-step design: i2c aliasing in SW

While the design above is more generic and removes unnecessary multpile probes for single camera,
it constitues a large refactor from current design.

An intermediate solution to allow for support of non-muxed boards while minimizing necessary changes is possible.
Aliasing the i2c bus addresses themselves cannot be done is SW, but it is possible to alias the devices addreses within a bus.

```mermaid
graph TB
    subgraph I2C_Addr["I2C MUX"]
        I2C_Bus_0x30["I2C Address 0x30"]
    end

    subgraph I2C_Out["I2C Output"]
        I2C_Out_0x30["I2C Address 0x30"]
        I2C_Out_0x33["I2C Address 0x33 (Unused)"]
        I2C_Out_0x34["I2C Address 0x34 (Unused)"]
        I2C_Out_0x35["I2C Address 0x35 (Unused)"]
    end

    subgraph I2C_Bus["I2C Bus"]
        I2C_Addr_0x30["I2C Address 0x30"]
    end

    subgraph DT["Device Tree"]
        DT_Depth["ds5_depth@0x30<br/>compatible='intel,d4xx'<br/>reg=0x1a<br/>cam-type='Depth'"]
        DT_RGB["ds5_rgb@0x30<br/>compatible='intel,d4xx'<br/>reg=0x1b<br/>cam-type='RGB'"]
        DT_IR["ds5_ir@0x30<br/>compatible='intel,d4xx'<br/>reg=0x1c<br/>cam-type='Y8'"]
        DT_IMU["ds5_imu@0x30<br/>compatible='intel,d4xx'<br/>reg=0x1d<br/>cam-type='IMU'"]
    end

    subgraph Driver["D4xx Driver (d4xx.c)"]
        Probe_Depth["ds5_probe()<br/>instance 1"]
        Probe_RGB["ds5_probe()<br/>instance 2"]
        Probe_IR["ds5_probe()<br/>instance 3"]
        Probe_IMU["ds5_probe()<br/>instance 4"]
    end

    subgraph V4L2_Subdevs["V4L2 Subdevices"]
        V4L2_Depth["Depth Sensor Subdev<br/>/dev/video0<br/>Format: Z16"]
        V4L2_RGB["RGB Sensor Subdev<br/>/dev/video2<br/>Format: YUYV/RGB"]
        V4L2_IR["IR Sensor Subdev<br/>/dev/video4<br/>Format: Y8/Y16"]
        V4L2_IMU["IMU Sensor Subdev<br/>/dev/video6<br/>Format: Metadata"]
    end

    subgraph Hardware["Physical Hardware"]
        HW_Depth["DS5 Depth Sensor"]
        HW_RGB["DS5 RGB Sensor"]
        HW_IR["DS5 IR Sensor"]
        HW_IMU["DS5 IMU Sensor"]
    end

    %% Device Tree to I2C Address
    DT_Depth -.->|defines| I2C_Addr_0x30
    DT_RGB -.->|defines| I2C_Addr_0x30
    DT_IR -.->|defines| I2C_Addr_0x30
    DT_IMU -.->|defines| I2C_Addr_0x30

    %% I2C to Driver Probe
    I2C_Addr_0x30 -->|I2C match| Probe_Depth
    I2C_Addr_0x30 -->|I2C match| Probe_RGB
    I2C_Addr_0x30 -->|I2C match| Probe_IR
    I2C_Addr_0x30 -->|I2C match| Probe_IMU

    %% Driver to V4L2 Subdevices
    Probe_Depth -->|creates| V4L2_Depth
    Probe_RGB -->|creates| V4L2_RGB
    Probe_IR -->|creates| V4L2_IR
    Probe_IMU -->|creates| V4L2_IMU

    %% Hardware connection
    HW_Depth <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_RGB <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_IR <-->|I2C Reg 0x1a| I2C_Bus_0x30
    HW_IMU <-->|I2C Reg 0x1a| I2C_Bus_0x30

    %% Mux Output
    I2C_Bus_0x30 <--> I2C_Out_0x30
    I2C_Bus_0x30 <--> I2C_Out_0x33
    I2C_Bus_0x30 <--> I2C_Out_0x34
    I2C_Bus_0x30 <--> I2C_Out_0x35
    
    style I2C_Out fill:#e1f5ff,color:#000000
    style DT fill:#fff4e1,color:#000000
    style Driver fill:#e8f5e9,color:#000000
    style V4L2_Subdevs fill:#f3e5f5,color:#000000
    style Hardware fill:#ffeaa7,color:#000000
```

### Example Multi-Sensor Multi-i2c-bus Configuration:

```dts
i2c@0 {
    reg = <0x0> /* All sensors at i2c 30 */
    d4m_depth@1a {
        compatible = "intel,d4xx";
        def-addr = <0x10>;
        /* reg addr 0x1a */
        reg = <0x1a>;
        cam-type = "Depth";
        vcc-supply = <&vdd_1v8>;
        ...
    };
    d4m_rgb@1b {
        compatible = "intel,d4xx";
        def-addr = <0x10>;
        /* reg addr (virtual) 0x1b */
        reg = <0x1b>;
        cam-type = "RGB";
        vcc-supply = <&vdd_1v8>;
        ...
    };
    d4m_ir@1c {
        compatible = "intel,d4xx";
        def-addr = <0x10>;
        /* reg addr (virtual) 0x1c */
        reg = <0x1c>;
        cam-type = "Y8";
        vcc-supply = <&vdd_1v8>;
        ...
    };
    d4m_imu@1d {
        compatible = "intel,d4xx";
        def-addr = <0x10>;
        /* reg addr (virtual) 0x1d */
        reg = <0x1d>;
        cam-type = "IMU";
        vcc-supply = <&vdd_1v8>;
        ...
    };
    ...
};
```

### Aliasing in D4xx driver

```
#define DS5_FIXED_REG_ADDR 0x1a
static int ds5_probe(struct i2c_client *c, const struct i2c_device_id *id)
{
    ...
    // Alias all addresses to 0x1a
	c->addr = DS5_FIXED_REG_ADDR;
}
```

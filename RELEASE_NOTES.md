# Release Notes - MIPI Platform Driver

## Version 1.0.6.2 (Pre-release) - 2026-08-17

### D5xx Camera Support & Enhancements
- Enable RGB calibration pixel mode for D585
- Set RGB gain range to 16-248 for D5xx cameras
- Expose RGB scalar controls for D585 GMSL
- Expose RGB controls and telemetry for D58x
- Fix D585 GMSL laser power range (RSDEV-13457)
- Address D58x laser configuration review feedback
- Align D585 GMSL advertised modes with USB
- Update D585 default I2C address to 0x10
- Add D401 dual RGB POC support: RAW8 dual-RGB CSI passthrough (OV9782)

### Video Format & Streaming Support
- Enable Y16I calibration pixel mode (RSDSO-21787)
- Fix Y16I hunk context compatibility with GNU patch
- Fix D5xx video node enumeration order
- Disable NVCSI deskew for D58x Y8 tunnel mode
- Remove vi5_fops Y12I error ignore (RSDEV-13252)

### MAX96712/MAX96724/MAX96717 SerDes Support
- Port D5xx MAX96724 tunnel mode to unified driver
- Minimize unified driver tunnel integration
- Unify MAX96717 pixel and tunnel paths
- Refine MAX96724 driver structure
- Align MAX96717 tunnel initialization with pixel mode
- Address MAX96724 review feedback
- Update max96712 4-lane rate to 1300Mbps
- Align all 96712 overlays to 4 MIPI lanes (RSDSO-21762)
- Use D5xx compatible string for MAX96712 overlays

### Hardware Platform Support
- Add 4-camera support for FangZhu FG12-4CH on Orin Nano/NX
- Add FG12 4-channel D5xx overlay
- Add JP7 4-camera FG12-16CH configuration + fix 1-camera name
- Add Seeed 3-camera (links 0-1-2) configuration

### V4L2 Controls & Metadata
- Expose depth AE mode as single read/write V4L2 control (RSDSO-21763)
- Extend metadata payload to 255 bytes for HKR usage (RSDEV-9250)
- Enlarge GVD control size (RSDEV-13118)

### Jetpack & Kernel Support
- Fix JetPack 5.1.6 streaming: G_PARM EINVAL + D457 color-path panic/recovery
- Install/deploy scripts now handle JP 5.1.2 module copy
- Fix Jetson VI5 recovery teardown resource lifetime

### D58x Pixel Mode & IMU
- Add D58x pixel mode flag and padded IMU line support (RSDSO 21757 + RSDSO 21758)

### GMSL Link & Timing Fixes
- Move GMSL one-shot flush from set_pipe to HW-reset recovery (RSDEV-12608)
- Flush the GMSL link line buffer on cold bring-up (RSDSO-21786)
- Fix BACKEND_TIMESTAMP > TIME_OF_ARRIVAL on GMSL (RSDSO-20101)

---

## Previous Releases

### Version 1.0.4.9 - 2026-07-20
Release 1.0.4.9: merge r/58.3 into master


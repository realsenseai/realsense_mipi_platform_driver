---
name: gmsl-overlay-fix
description: Make a GMSL camera DT overlay (Orin, JP6) that matches the cameras actually wired to a rig, prove it on the rig and roll back cleanly. Covers scan → pick or derive the overlay → build the dtbo → install as a NEW extlinux label → boot → verify (probe, scan, concurrent streams, dmesg) → roll back. Use when the cameras found on GMSL links don't match the overlay (missing /dev/video nodes, a camera that never probes, "-22" on a link property), when a carrier is moved to another CSI connector, or when asked to "fix the overlay", "make an overlay for rig X" or "enable cams 0-1 on <board>".
---

# Skill: fix a GMSL camera overlay on a rig

Worked example: an Orin Nano with a FangZhu FG12-4CH on the **CAM0** connector, D45X on link A + D401 on
link B → `hardware/realsense/tegra234-camera-d4xx-overlay-fg12-4ch-csi-b-cams-0-1.dts`.

Related skills, used here rather than copied:
- **gmsl-camera-scan** (the camera FW repo's `.claude/skills/gmsl-camera-scan/`): `gmsl_cam_scan.sh`
  finds the locked links and the serializer and camera on each, independent of the overlay. Copy it to the
  rig and run it there.
- **build** Step 3: overlay-only rebuild and how a new dtbo joins the `0001` patch's `dtbo-y` list.
- **deploy**: boot-label install conventions (new label, before/after camera list, rollback).
- **mipi-bandwidth-check**: when the overlay is right but concurrent streams starve.

Rig rules: use a rig only when it's allocated to you. Put a `[HH:MM:SS]` timestamp and an ETA in every
status line. Never write a rig hostname or password into the repo, a skill or a PR.

## 1. Scan what is really connected

```bash
sudo ./gmsl_cam_scan.sh --no-move      # read-only; restores link-enable reg 0x06
```
Record per link: lock, serializer address, camera model + FW, alias, the deserializer bus/address, and the
`overlay link-mask … at <DT path>` line. That path shows the **mux channel** the active overlay uses
(`cam_i2cmux/i2c@0` vs `i2c@1`). Map the nodes the active overlay already gives:
`for v in /sys/class/video4linux/video*; do echo "$v $(cat $v/name)"; done`.

## 2. Pick or derive the overlay

1. **Pick:** look in `hardware/realsense/` for an overlay with the same carrier, Jetson connector,
   deserializer and links. If one exists, go to step 3.
2. **Derive:** usually a working single-camera overlay for this carrier exists (often only as a dtbo in
   the rig's `/boot`), plus a multi-camera overlay for the same carrier on another connector. Take the
   multi-camera `.dts` as the base:
   - Trim links you don't need: VI `port@N`, `moduleN`, nvcsi `channel@N`, `ds5_N`, the `ser_X` node and
     its `maxim,linkN-vc-remap`. Set `num-channels` and `link-mask` to match.
   - Apply the **connection deltas** from the known-good single-camera dtbo. Find them with a
     property-level diff that ignores phandle plumbing:
     ```bash
     ./dtbo_diff.py rig-single-cam.dtbo derived.dtbo
     ```
     Typical deltas: mux `i2c@N`, nvcsi `port-index` + `tegra_sinterface`, `lane_polarity`, `bus-width`,
     `num_lanes`, deser `lane_cnt`, `serdes_pix_clk_hz`. When the diff shows only the intended link/VC
     changes, the derivation is done.
   - Keep `serdes_pix_clk_hz` equal to `lanes × Mbps/lane / 16`: 2 × 1500 / 16 → `187500000`;
     4 × 1300 / 16 → `325000000`.
   - **Never** add a DT property that depends on the camera model (CLAUDE.md, `hardware/realsense/`).
3. Do it with a script (regex rules that fail when they match nothing), not by hand. Rewrite the header
   comment to describe the real wiring.

## 3. Build the dtbo

```bash
./build_overlay_dtbo.sh hardware/realsense/<overlay>.dts <out>.dtbo [sources_6.2.x]
```
Sanity check: build an existing repo overlay with this script and compare it with the dtbo `build_all.sh`
produced; the sizes should match. Then test-apply offline on the rig's **own** base DTB (the `FDT` line of
the label):
```bash
fdtoverlay -i base.dtb -o merged.dtb <out>.dtbo
fdtget -p merged.dtb /bus@0/cam_i2cmux/i2c@0/max96712@29
```
Add the dtbo to `hardware/nvidia/t23x/nv-public/6.x/0001-…patch` (`dtbo-y`; bump the hunk count and the
diffstat) and to the README overlay tables. See build Step 3.

## 4. Install as a new boot label

1. Back up: `sudo cp -p /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.bak-before-<ticket>`.
2. Copy the dtbo to `/boot/<folder>/` and **add** a new `LABEL` that copies the known-good label's
   LINUX/INITRD/APPEND/FDT and changes only `OVERLAYS`. Never edit the known-good label.
3. Flipping `DEFAULT` needs explicit approval (no serial console, so U-Boot menu selection isn't
   possible). Flip and reboot as **separate**, timestamped steps.
4. Watch SSH from the background (sleep 45 s, then poll every 10 s, ~5 min budget). Never use a
   foreground sleep.
5. If SSH doesn't come back in ~5 min, or no camera probes: set `DEFAULT` back to the known-good label
   and reboot.

## 5. Verify

- **Probe:** one `D4XX Sensor: DEPTH, firmware build:` line and `subdev DS5 mux <bus>-<addr> bound` for
  each camera's depth alias (link A `0x1a`, B `0x2a`, C `0x3a`, D `0x4a`).
  `sudo dmesg | grep -E "d4xx|max96712|max9295|DS5 mux"`.
- **Scan** again with `--no-move`. It must list every camera and show the new overlay's link-mask/path.
- **Node map** from `/sys/class/video4linux/*/name`. Per camera: depth, depth-md, color, color-md, IR,
  IMU (odd numbers are metadata).
- **Concurrent streams:** use `stream_concurrent.sh` (runs on the Jetson). It refuses to start if
  a `v4l2-ctl` is still running and prints the dmesg from its own start time, so a wrapped ring buffer
  doesn't matter:
  ```bash
  STAGGER=1 ./stream_concurrent.sh 900 "0:848x480:Z16 :30" "2:1280x720:YUYV:30" "4:848x480:GREY:30" \
      "7:1280x720:Z16 :30" "9:1280x720:YUYV:30" "11:1280x720:GREY:30"
  ```
  Pass = every stream N/N frames and no new `uncorr_err`/timeouts. Expected and harmless:
  `link N line-buffer flush`, `stream N toggle ok`, and a single `corr_err: discarding
  frame 0` when a link starts.
- **Stability:** a second clean reboot must give the same result before the new label stays `DEFAULT`.

## 6. Roll back / clean up

- Rollback = set `DEFAULT` to the known-good label (or restore the backup) and reboot.
- Report the final state: `DEFAULT`, the labels present, the backup file names, and what you removed.
- Delete only your own temp files. Removing other people's labels/dtbos needs their explicit OK.

## Gotchas

- **Per-link properties for a link outside `link-mask` fail probe with -22.** Trim `linkN-vc-remap`
  together with the link.
- **Moving a carrier between Jetson connectors changes the mux channel, CSI port, lane count and
  polarity**, not just `port-index`. FG12-4CH on Orin Nano CAM0: mux `i2c@0`, deser CSI **port B**,
  2 lanes at **1500 Mbps**, nvcsi `port-index 1`/`serial_b`, `lane_polarity "6"`. The other connector:
  `i2c@1`, port C, 4 lanes at 1300 Mbps.
- **`maxim,csi-out-port` and `lane_rate_mbps` are parsed only by an out-of-tree max96712**
  (the module on the worked-example rig, built 2026-09-30 17:37). The repo driver hard-codes deser port A and 1300/2500 Mbps. Check
  that the running `max96712.ko` honours these properties (read `0x092D`, `0x0415`) before trusting an
  overlay that sets them.
- **D401: a second concurrent stream with a different frame height gets 0 frames** (VI `request timed
  out after 2500 ms`). This is a camera limit, not an overlay bug. Use the same height on all of a D401's
  concurrent streams when validating.
- Starting four or more streams at the same instant gives recoverable `failed to set pipe` /
  `max9295_write_reg` retries. Use `STAGGER=1`.
- Max96712 hybrid-lane rules for other connectors: see the repo CLAUDE.md "MIPI lane configuration".

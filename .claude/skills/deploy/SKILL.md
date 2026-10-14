---
name: deploy
description: Deploy the RealSense MIPI platform driver to a NVIDIA Jetson device. Use when the user wants to deploy, flash, install kernel/modules/DTBs to a Jetson, or verify a deployment. Triggers on requests mentioning deploy, flash, install kernel, push to jetson, or update jetson.
---

# Deploy Skill

## Supported JetPack Versions

Valid versions: `4.6.1`, `5.0.2`, `5.1.2`, `6.0`, `6.1`, `6.2`, `6.2.1`

Always ask the user which JetPack version to target if not specified.

## Deploy Workflow

### Step 1: Deploy to Jetson

To deploy use this bash command:

```bash
./scripts/deploy_kernel.sh $VERSION <TARGET_IP> [USERNAME] [REMOTE_PATH] [REMOTE_BOOT_FOLDER]
```

Defaults: USERNAME=`administrator`, REMOTE_PATH='git.USER.NAME', REMOTE_BOOT_FOLDER=`dev`

`REMOTE_BOOT_FOLDER` selects which `/boot/<folder>` receives the kernel `Image`; rigs whose `extlinux.conf` boots a non-default folder need it set explicitly. Known-good example:

```bash
./scripts/deploy_kernel.sh 6.2 <RIG_HOST> nvidia drgb_poc dev
```

This script is the **only** supported way to deploy a driver build — it replaces the whole `/lib/modules/$(uname -r)` tree, so every module stays ABI-consistent. Never hand-copy individual `.ko` files: `tegra-camera.ko` exports `camera_common_*` to `d4xx.ko` and `tegra_csi_*`/`csi5_fops` to `nvhost-nvcsi*`, so a partial copy strands the other consumers ("disagrees about version of symbol"), NVCSI fails to load, and no `/dev/video*` are created at all.

The command must have all 3 arguments to perform the full deploy.
Ask the user to provide username and remote path if not provided.
Save in memory for the next deploy command.
Ask the user to confirm the TARGET_IP before proceeding.
Save in memory the last used TARGET_IP for the next deploy command.

Deploy packs build artifacts into `kernel_mod/$VERSION/`, SCPs to the Jetson, runs the on-device install script, then reboots.

Without a TARGET argument, deploy only packages locally (no SCP/reboot).

Reboot of the Jetson will take about 2-5 minutes. After reboot, the new kernel/modules should be active.

### Step 2: Verify deployment

After deploy and reboot, SSH into the Jetson and run:

```bash
sudo dmesg | grep d4xx          # Check driver probe — expect "d4xx" probe messages with no errors
ls -l /dev/video*                # Should show 6 video devices per camera (video0–video5)
v4l2-ctl -d0 --stream-mmap      # Verify streaming works
```

If `dmesg` shows no d4xx messages or `/dev/video*` devices are missing, the driver did not load — check for patch/build version mismatch or missing DTB overlay.

### Alternate deploy — carriers that boot via extlinux OVERLAYS (overlay-only)

Some GMSL carriers boot a **custom base FDT + boot-time device-tree overlays** chosen in `/boot/extlinux/extlinux.conf`, not the standard NVIDIA kernel/DTB layout. `deploy_kernel.sh` does not model this. For an overlay-only change on such a rig:

**An overlay only brings up the GMSL links it declares. A camera on a link the active overlay does not enable is invisible to every check below (`rs-fw-update -l`, `/dev/video-rs-*`, `dmesg`, `i2cdetect`), so "not listed" does not mean "not connected". Never conclude a rig has fewer cameras than it has from one overlay's view, and never switch overlays in a way that can hide one.**

0. **Record what is connected before touching anything.** Save the output as the "before" list:
   ```bash
   rs-fw-update -l                       # type + serial of every camera the current overlay brings up
   ls /dev/video-rs-*                    # one set per camera
   sudo dmesg | grep -E "d4xx 9-00|device type|PID"   # per-subdevice probe lines
   ```
   Also read which links the current and the target overlay enable (decompile with `dtc -I dtb -O dts <overlay>.dtbo` and look at the link mask / camera nodes), and compare with what is physically cabled. If the rig owner or the rig notes say more cameras are cabled than the "before" list shows, the active overlay is hiding some: pick an overlay that enables those links.
1. Copy the built `.dtbo` to the Jetson `/boot` (two hops if build host and Jetson aren't directly reachable):
   ```bash
   scp <build-host>:.../generic-dts/dtbs/<overlay>.dtbo <local>
   scp <local> <user>@<jetson>:/tmp/
   ssh <user>@<jetson> "sudo cp /tmp/<overlay>.dtbo /boot/"
   ```
2. **Add a new boot label; do not edit the existing `DEFAULT` label in place.** Back up first (`sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.bak-<date>`). Mirror an existing camera label, change only `OVERLAYS` and the menu text, then set `DEFAULT <new label>`. The previous label stays intact as the fallback. The target overlay must enable every link that shows a camera in the "before" list (a superset); if you cannot tell which links are populated, start with the widest overlay (all links).
3. Reboot — GMSL is boot-probe only, no hotplug:
   ```bash
   ssh <user>@<jetson> "sudo reboot"
   ```
4. After reboot verify (per verify-deploy) and **compare with the "before" list: every camera serial in it must still be present.** If one is missing, set `DEFAULT` back to the previous label, reboot, and stop; do not burn firmware or run tests on a rig that lost a camera. Also confirm the links: i2c groups `2-00N[abcd]` (link N), `/dev/video-rs-*` symlinks, and **no `-121`** (serdes no-response) in `dmesg` for links that should be empty.
5. Burn or flash only the camera you mean: select it by serial number, and check that the device listed under that serial is the model the firmware is for (a D585 and a D401 on neighbouring links take different firmware).
6. When the work is done, restore the previous `DEFAULT` label unless the rig's owner asks to keep the new one, and note the overlay and the camera list in the rig's own notes.

Notes:
- Non-interactive sudo with a payload file: `echo $PW | sudo -S bash -c 'cmd < /tmp/file'`. Do NOT pipe file content into `sudo -S tee` — sudo eats the first line as the password.
- The base FDT name and camera-count labels are rig-specific — keep them in the rig's own notes, not in this skill.

## Deploy Details

### What gets deployed

Deploy packs the following from `images/$VERSION/`:

**JP 6.x (Orin):**
- Kernel image
- NVIDIA OOT modules (nvidia-oot, nvgpu, etc.)
- Device tree overlays (`tegra234-camera-d4xx-overlay*.dtbo`)
- Device tree blobs (`tegra234-*.dtb`)

**JP 5.x / 4.6.1 (Xavier):**
- Kernel image
- Kernel modules
- Device tree blobs

### Deploy scripts

- `./scripts/deploy_kernel.sh` — Works for all JetPack versions

## Common Issues

- **SSH connection refused**: Ensure the Jetson is powered on and reachable at the provided IP.
- **Permission denied**: Ensure the user has sudo access on the Jetson.
- **Build not found**: Run the build first (`./build_all.sh $VERSION`) before deploying.
- **Driver not loading after deploy**: Check `dmesg` for errors. Ensure the correct JetPack version was used for both build and deploy.
- **Jetson not rebooting**: SSH into the Jetson and manually run `sudo reboot`.

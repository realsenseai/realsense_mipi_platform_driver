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
./scripts/deploy_kernel.sh $VERSION <TARGET_IP> [USERNAME] [REMOTE_PATH]
```

Defaults: USERNAME=`administrator`, REMOTE_PATH='git.USER.NAME'

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

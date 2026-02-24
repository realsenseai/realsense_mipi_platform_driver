# Upstreaming Plan: RealSense MIPI Platform Driver Patches

## Context

The repository contains ~160 patches applied on top of NVIDIA JetPack kernel sources and nvidia-oot
modules. These patches have accumulated over years of D4XX RealSense development. The goal is to:
1. **Remove** patches already present in upstream sources (reducing maintenance burden)
2. **Improve** patches to meet upstream quality standards (code style, commit messages, architecture)
3. **Submit** viable patches to their appropriate upstream targets

**Two upstream targets:**
- **Linux mainline** (`linux-media@vger.kernel.org`, `linux-i2c@vger.kernel.org`)
- **NVIDIA JetPack** (`nv-tegra.nvidia.com` Gerrit or `nvidia-oot` GitHub PRs)

**Focus:** JP 6.x (`nvidia-oot/6.x/` + `kernel/kernel-jammy-src/6.x/`) as the actively maintained
versions. Older JP versions (4.6.1, 5.0.2, 5.1.2) are lower priority and only addressed when the
same patch also exists in JP 6.x.

---

## Phase 1 — Remove Already-Upstreamed Patches (Quick Wins)

### 1.1 Power Line Frequency "Auto" — REMOVE across all JP versions

**Finding:** Ricardo Ribalda's series ("media: uvcvideo: Probe the PLF characteristics", merged
June 2024) supersedes this patch with a *better* dynamic approach. The hardcoded `{ 3, "Auto" }`
addition is now redundant.

**Action:** Verify the JetPack 6.x kernel (linux-jammy) contains this merged change, then delete:
- `kernel/kernel-jammy-src/6.0/0004-realsense-powerlinefrequency-control-fix-jammy.patch`
- `kernel/kernel-jammy-src/6.2/0004-realsense-powerlinefrequency-control-fix-jammy.patch`
- `kernel/kernel-jammy-src/6.2.1/0004-realsense-powerlinefrequency-control-fix-jammy.patch`
- The corresponding section in `kernel/kernel-5.10/5.0.2/0001-Porting-...patch` (partial removal
  requires re-generating the porting patch)
- `kernel/kernel-4.9/4.6.1/0006-realsense-powerlinefrequency-control-fix.patch`

**Verify command** (on the JetPack kernel source):
```bash
git log --oneline drivers/media/usb/uvc/uvc_ctrl.c | grep -i "power line\|PLF"
```

### 1.2 UVC Buffer Error on Overflow — VERIFY then remove

**Finding:** The patch (author: Baoyou Xie/Linaro, 2017) was posted to
`linux-media@vger.kernel.org` and tracked in Ubuntu bug #1849871.
Lore: https://lore.kernel.org/patchwork/patch/822563/

**Action:** Check if `uvc_video.c` in the JetPack kernel already contains `buf->error = 1` on
overflow, then delete:
- `kernel/kernel-4.9/4.6.1/0005-media-uvcvideo-mark-buffer-error-where-overflow.patch`

---

## Phase 2 — Patches for Linux Mainline Submission

### 2.1 RealSense Camera Formats (UVC)

**Files:** `kernel/kernel-jammy-src/6.x/0002-realsense-camera-formats-jammy-master.patch`

**What it does:** Adds RealSense-specific pixel format GUIDs (Z16H, Y16I, FG, INZC, PAIR, RAW8,
CONFIDENCE_MAP etc.) to the UVC driver.

**Issues to fix before submission:**
- Missing `Signed-off-by` from original authors (Yu Meng)
- Split into two patches: (a) generic format improvements, (b) RealSense device-specific entries
- Write proper commit messages explaining each format (what it encodes, use case)
- Target: `linux-media@vger.kernel.org`

### 2.2 Dynamic I2C Bus Clock Rate

**Files:** `kernel/kernel-jammy-src/6.x/0005-support-for-dynamic-change-of-i2c-bus-clk-rate.patch`

**What it does:** Adds `bus_clk_rate` field to `struct i2c_adapter`, sysfs r/w interface, and a
Tegra-specific clock change implementation triggered before each I2C transfer.

**Issues to fix before mainline submission:**
- `sprintf` → `sysfs_emit` in `show_bus_clk_rate()`
- Type inconsistency: struct field is `unsigned long` but `i2c_set_adapter_bus_clk_rate()` takes
  `int`; pick one type consistently
- Remove redundant `extern` keywords in `include/linux/i2c.h`
- Return value bug in `tegra_i2c_change_clock_rate()`: ends with `return err` but `err` is already
  known to be 0 at that point (a companion fix exists in the 5.1.2 patchset as a separate patch)
- **Architecture concern:** Adding Tegra-specific runtime state to the generic `struct i2c_adapter`
  requires upstream justification. Alternative: use a Tegra-specific sysfs entry that does not
  touch the core struct
- Send as RFC first to `linux-i2c@vger.kernel.org` to gauge community interest in the generic
  approach before investing in a full submission

### 2.3 RealSense Metadata (UVC device table)

**Files:** `kernel/kernel-jammy-src/6.x/0003-realsense-metadata-jammy-master.patch`

**What it does:** Adds 279+ RealSense D4XX camera USB IDs with `UVC_INFO_META` flag to enable
metadata capture.

**Issues to fix:**
- Device table is large but well-structured; acceptable for mainline
- Verify each USB ID represents a real shipping product
- Add `Co-developed-by:` attribution for Yu Meng and Evgeni Raikhel
- Target: `linux-media@vger.kernel.org`

---

## Phase 3 — Patches for NVIDIA nvidia-oot Submission

Target: `git://nv-tegra.nvidia.com/nvidia/nvidia-oot.git`
(Public mirror: https://github.com/OE4T/linux-nv-oot)

### 3.1 Stream Restart on Capture Timeout — HIGH PRIORITY (Novel Contribution)

**File:** `nvidia-oot/6.x/0002-Adding-stream-restart-upon-capture-timeout.patch`

**Why valuable:** NVIDIA's developer forums show capture timeout recovery is an unresolved pain
point for many Jetson camera developers. A generic mechanism benefits the entire ecosystem.

**Critical issues to fix before submission:**

1. **D4XX-specific string match** must be replaced with a generic mechanism:

   ```c
   // Current (NOT acceptable for upstream):
   if (strncmp(chan->subdev[1]->name, "DS5 mux", 7) == 0)

   // Proposed approach: add optional restart_stream callback to tegra_vi_fops
   // so any camera driver can register its own restart handler.
   // D4XX populates it; VI capture code calls it generically on timeout.
   ```

2. **Missing `Signed-off-by`** — Ehud J. Goldik's email appears in the commit body, not as an
   SOB line. Fix: add `Signed-off-by: Ehud J. Goldik <ehud.joseph.goldik@realsenseai.com>`

3. **Commit message** must explain the broader problem (capture timeout recovery for all cameras)
   rather than referencing D4XX by name

### 3.2 VI Channel s/g Frame Interval Callbacks

**File:** `kernel/nvidia/5.1.2/0003-vi-channel-verify-s-g-callbacks.patch`

**What it does:** Changes a direct `sd->ops->video->g_frame_interval()` call to first try
`v4l2_subdev_call()` and fall back to the direct call if it fails.

**Issues:** The fallback to a direct call after `v4l2_subdev_call` fails is architecturally
questionable — the two paths produce the same result if the underlying op returns an error. The
root cause (why does `v4l2_subdev_call` return an error for the D4XX subdevice?) must be
investigated.

**Action before submission:**
- Identify why `v4l2_subdev_call` fails for the D4XX subdevice
- If the fix belongs in the D4XX driver (e.g., registering the op correctly), move it there
- If the VI channel genuinely needs null-safety guards, submit that alone (drop the fallback)

### 3.3 GPIO Tunneling for External Sync

**File:** `nvidia-oot/6.x/0004-Add-GPIO-tunneling-for-the-external-sync-support.patch`

**What it does:** Configures MAX9295 and MAX9296 SerDes registers to enable GPIO tunneling for
ESYNC (external sync) support.

**Issues to fix:**
- **Missing `Signed-off-by`** — Nikolai Lyskov is the author but there is no SOB line
- **Magic register values** — `0x77`, `0x04`, `0x57`, `0xAA`, `0x17`, `0x47` are undocumented.
  Add `#define` names with comments referencing the MAX9295/MAX9296 datasheet section
- **Commit message** must explain: what external sync does for D4XX cameras, why GPIO tunneling
  through SerDes is required, and what the register writes configure

### 3.4 Fix Y12I Calibration Stream Workaround

**File:** `nvidia-oot/6.x/0003-Fix-y12i-calibration-stream.patch`

**Current state:** Marked `/* TODO: Remove Y12I frame_err workaround once Y12I is fixed */`.
Suppresses `VB2_BUF_STATE_ERROR` for Y12I frames to work around a "black-bar at the bottom of
the frame" artifact.

**Action:** Do NOT submit upstream as-is. Options:
1. **Preferred:** Fix the root cause (investigate whether the black bar is a firmware issue, a
   MIPI timing issue, or a format parsing issue in the driver)
2. **If firmware-only fix:** Update the TODO to reference the firmware version that resolves it;
   add a firmware version guard in the driver
3. **If workaround must stay temporarily:** Improve the comment to explain what `frame_err` means
   for Y12I and why it is expected behavior (is it a known sensor artifact?)

### 3.5 SerDes API Additions (max9295/max9296)

**File:** `nvidia-oot/6.x/0001-Porting-nvidia-driver-patches-to-jetpack-6.0.patch`

The large monolithic porting patch (902 insertions) includes well-designed
`max9295_set_pipe()` / `max9296_set_pipe()` / `max9296_get_available_pipe_id()` APIs along with
D4XX driver registration in the OOT build system.

**Action:** Split into logical per-feature patches for NVIDIA submission:
- **Patch A:** Add SerDes pipe configuration APIs (max9295/max9296)
- **Patch B:** Add VI channel metadata device init/cleanup
- **Patch C:** Add D4XX driver to the NVIDIA OOT build system
- Each patch is self-contained, has a proper commit message, and its own Signed-off-by

---

## Phase 4 — Patch Quality Standards (Apply to All)

Before submitting any patch, verify it passes all of the following:

| Check | Tool / Command |
|-------|----------------|
| Coding style | `scripts/checkpatch.pl --strict <patch>` |
| No debug code | Manual review — no stray `printk` / `dev_dbg` debugging aids |
| No TODO workarounds | Manual review |
| `Signed-off-by` present | Every patch must have one |
| Commit message quality | Subject ≤ 72 chars; body explains *why*, not just *what* |
| No commented-out code | Manual review |

---

## Priority Order

| Priority | Patch | Target | Effort |
|----------|-------|--------|--------|
| P1 | Remove PLF "Auto" (already upstream) | N/A — delete | Low |
| P1 | Remove UVC overflow fix (verify first) | N/A — delete | Low |
| P2 | Stream restart on timeout (refactor) | nvidia-oot | High |
| P2 | GPIO tunneling (add SOB + register docs) | nvidia-oot | Low |
| P3 | RealSense UVC metadata device table | linux-media | Medium |
| P3 | Split monolithic JP6.0 porting patch | nvidia-oot | High |
| P4 | I2C dynamic clock rate (RFC) | linux-i2c | Medium |
| P4 | Y12I calibration (fix root cause) | nvidia-oot | High |
| P5 | vi-channel s/g fix (investigate root cause) | nvidia-oot | Medium |

---

## Critical Files

| File | Action |
|------|--------|
| `nvidia-oot/6.x/0002-Adding-stream-restart-upon-capture-timeout.patch` | Refactor — remove D4XX string check, add generic callback |
| `nvidia-oot/6.x/0004-Add-GPIO-tunneling-for-the-external-sync-support.patch` | Add SOB, document register values |
| `nvidia-oot/6.0/0001-Porting-nvidia-driver-patches-to-jetpack-6.0.patch` | Split into logical patches |
| `kernel/kernel-jammy-src/6.x/0004-realsense-powerlinefrequency-control-fix-jammy.patch` | DELETE after verifying it's in upstream |
| `kernel/kernel-jammy-src/6.x/0005-support-for-dynamic-change-of-i2c-bus-clk-rate.patch` | Fix code quality issues |
| `kernel/kernel-4.9/4.6.1/0005-media-uvcvideo-mark-buffer-error-where-overflow.patch` | DELETE after verifying it's in upstream |

---

## Verification

For each **removed** patch:
1. Grep the JetPack kernel source for the key change:
   ```bash
   grep -n "Auto\|PLF" drivers/media/usb/uvc/uvc_ctrl.c
   ```
2. Build the driver without the patch and confirm no regressions.

For each **submitted** patch:
1. Apply to the base source: `git apply --check <patch>`
2. Run `scripts/checkpatch.pl --strict <patch>`
3. Build the affected module
4. Run streaming tests on Jetson to verify no regressions

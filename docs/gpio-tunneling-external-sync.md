# GPIO Tunneling for External Sync — Design Document

## Overview

External sync (ESYNC) allows multiple D4XX cameras to synchronize frame capture via a shared GPIO signal routed through the GMSL link. The GPIO tunneling patches configure ESYNC registers on both the MAX9295 serializer and the MAX96712 deserializer to establish this signal path. MAX96712 only.

## Signal Path

```
External sync source (GPIO)
    ↓
MAX96712 deserializer (ESYNC registers 0x306–0x308, 0x313–0x315)
    ↓ GMSL link
MAX9295 serializer (ESYNC registers 0x2BE–0x2C3)
    ↓
D4XX camera module
```

Both ends must be configured for external sync to function.

---

# Old Design (Always-On Tunneling)

## How It Worked

Tunneling was configured **unconditionally at probe time** for all MAX96712 setups. There was no way to enable or disable it at runtime.

### Call Sites

| Path | Serializer tunneling | Deserializer ESYNC |
|------|---------------------|--------------------|
| **Probe** (`ds5_board_setup`) | Called (gated on `dser_ops == &max96712_interface`) | Called (via `max96712_setup_control()`) |
| **SERDES recovery** (`ds5_hw_reset_serdes_recovery`) | **NOT called** | Called (via `dser_ops->setup_control()`) |
| **Stream-start recovery** (retry in `ds5_mux_s_stream`) | **NOT called** | Via `dser_ops->setup_control()` |

### Probe-Time Tunneling Setup

```c
/* ds5_board_setup(), line ~4269 */
if (state->dser_ops == &max96712_interface) {
    int tun_err = max9295_setup_gpio_tunneling(state->ser_dev);
    if (tun_err)
        dev_err(dev, "gmsl serializer GPIO tunneling setup failed\n");
}
```

Only the **primary** instance per camera (`serdes_primary == true`) enters `ds5_board_setup()`. Peer instances skip SERDES setup entirely.

ESYNC registers were also written unconditionally inside `max96712_setup_control()`.

### Recovery Path (missing tunneling)

```c
/* ds5_hw_reset_serdes_recovery(), line ~2641 */
ret = max9295_setup_control(state->ser_dev);
/* ... */
ret = state->dser_ops->setup_control(state->dser_dev, &primary->client->dev);
/* max9295_setup_gpio_tunneling() is NOT called here */
```

## Old Design — Multi-Camera Behavior

### Single Camera on MAX96712

**Topology:** 1× D457 → 1× MAX9295 → 1× MAX96712 → Jetson

Probe configured both ends. **External sync worked.**

After HW reset: camera FW returned to default sync mode, but serializer tunneling stayed programmed. **Camera and serializer became inconsistent.**

### Dual Cameras on MAX96712

**Topology:** 2× D457 → 2× MAX9295 → 1× MAX96712 → Jetson

Each camera probed as primary, each configured its serializer and the shared deserializer. **Sync worked initially.**

After HW reset of one camera: that camera returned to default sync mode, but its serializer stayed tunneled. **That camera became inconsistent; the other still worked.**

After deserializer power cycle: all ESYNC registers wiped. Recovery restored deserializer ESYNC but neither serializer. **Both cameras lost sync.**

### Four Cameras on MAX96712 (fg12-16ch)

**Topology:** 4× D457 → 4× MAX9295 → 1× MAX96712 → Jetson

Same pattern. All configured at probe. After HW reset of any camera, that camera returned to default sync mode while its serializer remained tunneled. Others remained operational.

### Multiple Deserializers

Each serializer–deserializer pair was configured independently at probe. Same recovery gap applied per camera.

## Old Design — Known Issues

1. **HW reset left camera and serializer out of sync** — HW reset returned the D4XX FW to default sync mode, but the serializer tunneling registers remained programmed. Recovery did not drive the serializer back to default.
2. **No runtime enable/disable** — tunneling was always on, no V4L2 control to toggle it.
3. **Redundant deserializer writes** — `max96712_setup_control()` wrote ESYNC registers on every call, including once per camera in multi-camera setups.
4. **Tunneling active even when not needed** — cameras using internal sync (modes 0/1/4/5) still had ESYNC registers configured.

---

# New Design (Split Tunneling: Deserializer at Probe, Serializer On-Demand)

## Key Changes

| Aspect | Old Design | New Design |
|--------|-----------|------------|
| Deserializer ESYNC | Always at probe | At probe, fixed — no runtime changes |
| Serializer tunneling | Always at probe | On demand, only for the camera changing sync mode |
| Default state | Tunneling ON (both) | Deserializer ON at probe; serializer OFF until mode 2/3 |
| Runtime control | None | Per-camera `DS5_CAMERA_CID_SYNC_MODE` controls its serializer |
| After HW reset | Camera/serializer could diverge | Recovering camera serializer is reset to default to match reset camera FW |
| Scope | All MAX96712 setups | MAX96712 only; serializer side per-camera |

## Assumptions

- The deserializer ESYNC configuration is set once at probe and never modified at runtime. It is a shared, permanent configuration for the deserializer.
- Each camera's serializer ESYNC is independent. Enabling ESYNC on camera A does not affect camera B's serializer.
- Mixed sync modes on the same deserializer are supported: camera A can be in mode 2 (slave) while camera B is in mode 0 (default). Each camera controls only its own serializer.

## Trigger

| Sync Mode | Name | Serializer Action (this camera only) |
|-----------|------|--------------------------------------|
| 0 | Default | Disable (if currently on) |
| 1 | Master | Disable (if currently on) |
| 2 | Slave | **Enable** (if currently off) |
| 3 | Full Slave | **Enable** (if currently off) |
| 4 | Sub Pre-Master | Disable (if currently on) |
| 5 | Full Master | Disable (if currently on) |

Deserializer ESYNC is **not toggled** at runtime.

## Requirements

**R1 — Deserializer ESYNC at probe.** Keep ESYNC register writes in `max96712_setup_control()`. The deserializer is configured once at probe and never changed afterward.

**R2 — Serializer update policy.** On mode 2/3, program this camera's serializer to ESYNC-enabled. On mode 0/1/4/5, program this camera's serializer to default (ESYNC-disabled).

**R3 — HW reset returns serializer to default.** HW reset restores the camera FW to default sync mode, but does not reset serializer registers. Recovery must therefore disable tunneling on the recovered camera's serializer. Deserializer is not re-configured.

**R4 — No saved sync mode across HW reset.** The current driver does not store the requested sync mode in recovery-owned state. Recovery must therefore align the serializer to the reset camera's default state, not try to restore slave mode automatically.

**R5 — FW command unchanged.** `ds5_write(state, base | DS5_CAMERA_SYNC_MODE, ctrl->val)` is always sent to FW regardless of tunneling logic. Tunneling is a SerDes-level overlay.

**R6 — Locking.** Keep existing locking rules for serializer programming paths.

**R7 — MAX96712 only.** All serializer tunneling logic gated on `dser_ops == &max96712_interface`. No effect on MAX9296 setups.

**R8 — Per-camera independence.** Enabling ESYNC on one camera has no side-effect on sibling cameras. No iteration over `ds5_inited[]` on mode change.

**R9 — New SerDes API pairs.** Enable/disable functions for the serializer; deserializer uses existing `setup_control` path:
- `max9295_enable_gpio_tunneling()` / `max9295_disable_gpio_tunneling()`
- Deserializer: no new API needed (ESYNC remains in `max96712_setup_control()`)

## New Design — Call Sites

| Path | Serializer tunneling | Deserializer ESYNC |
|------|---------------------|--------------------|
| **Probe** | None | At probe via `max96712_setup_control()` — permanent |
| **`s_ctrl` SYNC_MODE → mode 2/3** | Enable **this camera's** serializer | No change |
| **`s_ctrl` SYNC_MODE → mode 0/1/4/5** | Disable **this camera's** serializer | No change |
| **HW reset recovery** | Disable **this camera's** serializer | No change |
| **Stream-start recovery** | No serializer change; previous HW reset already forced default state | No change |

## New Design — Multi-Camera Behavior

### Single Camera on MAX96712

**Topology:** 1× D457 → 1× MAX9295 → 1× MAX96712 → Jetson

1. Probe: deserializer ESYNC configured. Serializer tunneling OFF.
2. User sets sync mode 2 → this camera's serializer ESYNC is configured. **Sync works.**
3. User sets sync mode 0 → this camera's serializer is restored. **Sync off.**
4. After HW reset: camera FW returns to default mode, recovery disables this camera's serializer tunneling. Deserializer unchanged. **System returns to coherent default state.**

### Dual Cameras on MAX96712

**Topology:** 2× D457 → 2× MAX9295 → 1× MAX96712 → Jetson

1. Probe: deserializer ESYNC configured. Both serializers OFF.
2. Camera A sets mode 2 → Only Camera A's serializer configured. Camera B's serializer unchanged.
3. Camera B sets mode 3 → Only Camera B's serializer configured.
4. Camera B sets mode 0 → Only Camera B's serializer restored. Camera A unaffected.
5. After HW reset of Camera A: recovery disables only Camera A's serializer. Camera B unchanged. **Independent per-camera recovery.**

### Four Cameras on MAX96712 (fg12-16ch)

**Topology:** 4× D457 → 4× MAX9295 → 1× MAX96712 → Jetson

Each camera controls only its own serializer. The deserializer is configured once at probe. Any camera can independently enable/disable its serializer tunneling. HW reset recovery is per-camera and returns only the affected serializer to default.

### Multiple Deserializers

**Topology:** N× D457 → N× MAX9295 → 2× MAX96712 → Jetson

Each deserializer is configured at probe and unchanged thereafter. Each camera independently manages its own serializer tunneling. Cameras on deserializer A are fully independent from cameras on deserializer B.

If a camera undergoes HW reset, only that camera's serializer is driven back to default. Other cameras remain unchanged.

## Implementation Phases

### Phase 1 — New SerDes API functions

**MAX9295 serializer** (separate patch per JetPack version):
- Rename `max9295_setup_gpio_tunneling()` → `max9295_enable_gpio_tunneling()`
- Add `max9295_disable_gpio_tunneling()` — restores `MAX9295_PWDN_GPIO` (0x90), `MAX9295_RESET_SRC` (0x60), zeroes `0x2C0`–`0x2C3`
- Export both, declare in `max9295.h`

**MAX96712 deserializer** — no API change. ESYNC register writes remain in `max96712_setup_control()` as before. No `enable_esync`/`disable_esync` functions needed.

### Phase 2 — `dser_interface` — no change

No new function pointers needed. Deserializer ESYNC is permanent; only the serializer needs runtime enable/disable.

### Phase 3 — Tunneling helper (`d4xx.c`)

New `ds5_set_ser_esync_tunneling(struct ds5 *state, bool enable)`:
1. Gate on `dser_ops == &max96712_interface`
2. Enable: call `max9295_enable_gpio_tunneling(state->ser_dev)`
3. Disable: call `max9295_disable_gpio_tunneling(state->ser_dev)`

Only this camera's serializer is touched. No sibling iteration.

### Phase 4 — Hook into `s_ctrl` (`d4xx.c`)

In `ds5_s_ctrl`, case `DS5_CAMERA_CID_SYNC_MODE`, after the FW write:

```c
if (state->is_depth && state->dser_ops == &max96712_interface) {
    bool need_esync = (ctrl->val == 2 || ctrl->val == 3);
    ds5_set_ser_esync_tunneling(state, need_esync);
}
```

### Phase 5 — Remove probe-time serializer tunneling (`d4xx.c`)

- Delete `max9295_setup_gpio_tunneling()` call from `ds5_board_setup()`
- Deserializer ESYNC writes in `max96712_setup_control()` are kept — deserializer is configured at probe and stays configured

### Phase 6 — Recovery path (`d4xx.c`)

In `ds5_hw_reset_with_recovery()`, after HW reset completes, return the recovered camera's serializer to default:

```c
if (state->dser_ops == &max96712_interface) {
    max9295_disable_gpio_tunneling(state->ser_dev);
    /* Deserializer ESYNC is not re-written */
}
```

## Files to Modify

| File | Change |
|------|--------|
| `kernel/realsense/d4xx.c` | helper, `s_ctrl` hook, probe cleanup (remove serializer tunneling call), recovery |
| `nvidia-oot/*/0004-*max9295*tunneling*.patch` | Rename enable, add disable, update header |
| `.github/copilot-instructions.md` | (no change needed) |

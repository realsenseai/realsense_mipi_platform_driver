---
name: add-xu-command
description: Add a new XU command/control for the D4XX driver in this repository. Use when the user asks to add a new XU command, add RO XU control, add read-only XU, add extension unit selector, add new V4L2 private control, or wire FW command through ds5 controls.
---

# Add XU Command Skill

Add a new XU-style control in the D4XX stack with consistent driver wiring, test updates, and command-line validation.

## Scope

Use this skill when implementing a new XU/extended control path that maps to D4XX firmware command/register behavior and is exposed as a V4L2 control.

- Primary implementation file: `kernel/realsense/d4xx.c`
- Test constants mirror: `test/v4l2_test/d4xx/constants.py`
- Control tests: `test/v4l2_test/tests/test_controls.py`
- Optional app-side selector mapping: `utilities/streamApp/camera_sub_system/include/CSSTypes.h`

## Inputs To Collect First

Before coding, gather these requirements from the user/spec:

1. Control type: GET only, SET only, or GET + SET.
2. Data shape: integer, menu, bool, byte array, or structured payload.
3. Firmware interface: the exact register address (offset from the control base) for direct register access. This must come from firmware documentation or the firmware team — never infer it from the XU selector, unit ID, or GUID.
4. Sensor scope: depth-only, RGB-only, IR-only, IMU-only, or all.
5. Range/enum details: min, max, step, default, and unsupported values.
6. Runtime constraints: allowed while streaming, reset behavior, SerDes side effects.

If any of these are missing, ask concise clarifying questions before implementation.

## Implementation Workflow

### 1. Allocate a control ID

In `kernel/realsense/d4xx.c`:

1. Add a new `DS5_CAMERA_CID_*` define near the existing `DS5_CAMERA_CID_BASE` list.
2. Keep offsets unique and compact.
3. If the control has menu values, add an enum and menu table close to existing control definitions.

### 2. Add control config

Add a `static struct v4l2_ctrl_config ds5_ctrl_*` entry. Choose flags based on access type:

| Access | `.flags` |
|--------|----------|
| GET only | `V4L2_CTRL_FLAG_VOLATILE \| V4L2_CTRL_FLAG_READ_ONLY` |
| SET only | `V4L2_CTRL_FLAG_EXECUTE_ON_WRITE` |
| GET + SET | `V4L2_CTRL_FLAG_VOLATILE \| V4L2_CTRL_FLAG_EXECUTE_ON_WRITE` |

Always set `.ops = &ds5_ctrl_ops` and fill `.type`, `.min`, `.max`, `.step`, `.def` (and `.dims`/`.elem_size` for array types).

### 3. Wire control handlers

#### GET path — `ds5_g_volatile_ctrl()` (when GET is supported)

Add a `case DS5_CAMERA_CID_*:` block that reads the value from firmware and writes it to the appropriate `ctrl->p_new.*` pointer:

- Scalar: `ds5_read(state, base | REGISTER_OFFSET, ctrl->p_new.p_u16)`
- Array payload: `ds5_raw_read()` into `ctrl->p_new.p_u8` or `ctrl->p_new.p`

#### SET path — `ds5_s_ctrl()` (when SET is supported)

Add a `case DS5_CAMERA_CID_*:` block that writes the value to firmware:

- Scalar: `ds5_write(state, base | REGISTER_OFFSET, ctrl->val)`
- Array: `ds5_raw_write(state, base | REGISTER_OFFSET, ctrl->p_new.p, size)`

For both handlers:
- Reuse existing helpers (`ds5_read`, `ds5_write`, `ds5_hwmc_send`, `ds5_hwmc_wait`, `ds5_get_hwmc`).
- Locking differs by handler: `ds5_s_ctrl()` acquires `state->lock` itself before the switch; `ds5_g_volatile_ctrl()` does NOT hold `state->lock` — do not assume it is held in GET paths.
- Follow existing logging patterns (include `__func__`).
- Guard depth-only controls with `if (state->is_depth)`, RGB-only with `if (state->is_rgb)`, etc.

### 4. Register the control in init path

In `ds5_ctrl_init()`:

1. Add `v4l2_ctrl_new_custom(hdl, &ds5_ctrl_*, sensor)` in the appropriate SID block:
   - All sensors (depth+RGB+IR): `if (sid >= DEPTH_SID && sid < IMU_SID)`
   - Depth-only: `if (sid == DEPTH_SID)`
   - RGB-only: `if (sid == RGB_SID)`
   - IMU-only: `if (sid == IMU_SID)`
2. Store the returned pointer in `ctrls->*` only if a write path needs to access it later.
3. Preserve existing control registration order/style.

### 5. Mirror test constants

Update `test/v4l2_test/d4xx/constants.py`:

1. Add `DS5_CAMERA_CID_* = DS5_CAMERA_CID_BASE + <offset>` with the same offset as `d4xx.c`.
2. Keep numbering synchronized with the driver file to avoid test drift.

### 6. Write tests (mandatory)

Tests are **always required** for every new XU control. Add a new `class Test<ControlName>` in `test/v4l2_test/tests/test_controls.py`, decorated with `@pytest.mark.d457`.

Write **every applicable test** from the list below based on which operations the control supports:

| Test | When to write |
|------|--------------|
| `test_<name>_enumerated` | Always — verify control appears in `enumerate_controls()` |
| `test_<name>_read` | When GET is supported — verify value is readable and within `[min, max]` |
| `test_<name>_write_legal` | When SET is supported — verify legal values (min, mid, max) are accepted |
| `test_<name>_write_rejected` | When GET-only — verify write raises `OSError` |
| `test_<name>_set_illegal` | When SET is supported — verify out-of-range values are clamped to `[min, max]` |
| `test_<name>_default` | When GET + SET — verify writing default and reading back returns default |
| `test_<name>_roundtrip` | When GET + SET — verify set/get roundtrip for min, mid, max; restore original in `finally` |

**Clamping note:** `VIDIOC_S_CTRL` silently clamps out-of-range writes to `[min, max]`. Verify clamped value with a subsequent read. If hard rejection is required, use `VIDIOC_S_EXT_CTRLS` (returns `ERANGE`) and assert with `pytest.raises(OSError)`.

Always restore the original value in a `finally` block when a test changes a persistent control value.

**No magic numbers:** Declare `MIN`, `MAX`, `MID`, and `DEFAULT` as class-level constants at the top of the test class. Use these names everywhere in the test methods — including boundary arithmetic (`self.MAX + 1`, `self.MIN - 1`) and assertion messages. Never write raw numeric literals in test logic.

### 7. Optional selector/app mapping

If userspace selector mapping is required, update:

- `utilities/streamApp/camera_sub_system/include/CSSTypes.h`

Add selector/CID mapping only if it is part of the task requirements.

## Validation Checklist

Run these checks after coding:

1. Build/compile target driver path for the requested JetPack version.
2. Verify control presence:
   - `v4l2-ctl -d /dev/video0 --list-ctrls`
3. Verify control read (when GET is supported):
   - `v4l2-ctl -d /dev/video0 --get-ctrl=<control_name>`
4. Verify control write (when SET is supported):
   - `v4l2-ctl -d /dev/video0 --set-ctrl=<control_name>=<value>`
5. Run the new test class:
   - `cd test && python3 run_ci.py -r test_<name>`

## Code Style and Safety Rules

1. Follow Linux kernel style in `d4xx.c`.
2. Keep changes minimal; avoid broad refactors for a single control addition.
3. Reuse existing helper and error-handling paths.
4. Avoid adding duplicate readiness/recovery logic if an existing path already covers it.
5. If changes touch patch files, validate hunk headers and ask whether to apply fixes across all JetPack versions.
6. **Never suggest HWMC for XU controls.** The XU selector number (e.g. 0x13) and the HWMC opcode are independent — there is no mapping between them. The correct firmware register address for a XU control must be obtained from the firmware team or firmware documentation. Do not guess or derive the register address from the selector, unit ID, or GUID.

## Expected Output Format

When using this skill, provide:

1. Summary of requirements interpreted for the new XU control.
2. List of files changed.
3. Control ID, user-facing control name, and access type (GET only / SET only / GET + SET).
4. Names of tests written and which cases they cover.
5. Validation commands and observed outcomes.
6. Any assumptions or follow-up items.

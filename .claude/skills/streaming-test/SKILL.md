---
name: streaming-test
description: "Run the D4XX StreamToMetaData streaming stability test on a Jetson device over SSH. Executes StreamingTestRunner.py with fixed test profiles (DEPTH+IR+COLOR at 848x480@30fps), analyzes output for streaming errors, checks dmesg for D4XX/GMSL driver errors, and produces a summary report. Supports multiple iterations with a compiled end report. Use when the user wants to: run streaming test, run stability test, run StreamToMetaData, test streaming on jetson, run streaming iterations, stress test camera, start/stop test, cycle test, or check streaming reliability."
---

# Streaming Stability Test

Run StreamingTestRunner.py on a Jetson device, analyze results, and report.

## Workflow

1. Determine target device and iteration count
2. Clear dmesg on the Jetson before testing
3. Send camera hardware reset via v4l2-ctl
4. For each iteration, run the test and capture output
5. After all iterations, capture dmesg
6. Analyze all outputs and dmesg for errors
7. Present a summary report to the user

## Step 1: Determine Parameters

Ask the user for parameters using AskUserQuestion. Use defaults if the user has already specified values or asks to use defaults.

- **Target**: Default from you memory. Override if user specifies a different host.
- **Iterations**: Default 1. The user may request N iterations (e.g., "run 5 iterations").
- **Profile**: Default `848x480`. Available profiles:
  - `848x480` (default): `DEPTH_848_480_Z16_30_IR1_848_480_Y8_30_COLOR_848_480_YUYV_30`
  - `1280x720`: `DEPTH_1280_720_Z16_30_IR1_1280_720_Y8_30_COLOR_1280_720_YUYV_30`
  - `640x480`: `DEPTH_640_480_Z16_30_IR1_640_480_Y8_30_COLOR_640_480_YUYV_30`
- **Stream time**: Default `30` seconds. The user may specify a different duration (e.g., "stream for 60 seconds").

## Step 2: Clear dmesg

Before the first iteration, clear dmesg to get a clean baseline:

```bash
ssh nvidia@<host> "sudo dmesg -C"
```

## Step 3: Send Camera Hardware Reset

Send a hardware reset to the camera via the V4L2 custom control on the Depth video device (video0). This ensures the camera is in a clean state before streaming.

```bash
ssh nvidia@<host> "v4l2-ctl -d /dev/video0 -c hw_reset=1"
```

Wait 5 seconds after the reset for the camera to fully reinitialize before proceeding:

```bash
sleep 5
```

If the reset command fails (e.g., device not found), log the error but continue with the test.

## Step 4: Run the Test

For each iteration, run with the selected profile and stream time:

```bash
ssh nvidia@<host> "cd ~/librealsense/build/Release/StreamToMetaData && python StreamingTestRunner.py --profiles <PROFILE> --cycle 10 --time <TIME> --start-method 0 --no-metadata-tracking" 2>&1
```

Where:
- `<PROFILE>` is the selected profile string from Step 1 (e.g., `DEPTH_848_480_Z16_30_IR1_848_480_Y8_30_COLOR_848_480_YUYV_30`)
- `<TIME>` is the stream time in seconds from Step 1 (default: `30`)

Use a long timeout (at least `10 * <TIME> + 60` seconds) to account for all cycles plus overhead.

Label each iteration clearly (e.g., "Iteration 1/N") when reporting progress to the user.

## Step 5: Capture dmesg

After all iterations complete:

```bash
ssh nvidia@<host> "sudo dmesg --time-format=reltime" 2>&1
```

## Step 6: Analyze Results

### Test Output Errors

Scan each iteration's output for these patterns (case-insensitive):
- `error` or `ERROR`
- `fail` or `FAIL`
- `exception` or `Exception`
- `timeout` or `Timeout`
- `No frames received`
- `could not` or `Could not`
- `unable to` or `Unable to`
- Lines with non-zero exit codes or tracebacks

### dmesg Errors

Scan dmesg for these D4XX/GMSL-specific patterns:
- `d4xx` with `error`, `fail`, `timeout`, or `fault`
- `max9295` or `max9296` errors
- `tegra-capture-vi` errors or warnings
- `GMSL` errors
- `camera` with `error` or `fail`
- `i2c` transfer errors related to camera buses
- Kernel oops, panics, or BUG lines mentioning camera/vi/d4xx

## Step 7: Summary Report

Present the report in this format:

```
## Streaming Test Report

**Target:** nvidia@<host>
**Iterations:** N
**Profile:** <selected profile resolution>
**Stream time:** <TIME>s per cycle
**Test command:** StreamingTestRunner.py --profiles <PROFILE> --cycle 10 --time <TIME> --start-method 0 --no-metadata-tracking

### Results per Iteration

| Iteration | Status | Errors |
|-----------|--------|--------|
| 1         | PASS/FAIL | brief error description or "None" |
| ...       | ...    | ...    |

### Overall Result: PASS / FAIL (X/N iterations passed)

### Test Output Errors
[List each unique error found with iteration number and context, or "No errors found"]

### dmesg Analysis
[List each relevant error/warning found, or "No D4XX/GMSL errors in dmesg"]

### Notes
[Any additional observations: repeated patterns, degradation over iterations, etc.]
```

When multiple iterations are run, also note:
- Whether failures are consistent or intermittent
- Whether errors increase over iterations (suggesting resource leaks)
- First iteration where failure appeared

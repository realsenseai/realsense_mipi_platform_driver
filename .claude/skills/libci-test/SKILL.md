---
name: libci-test
description: "Run librealsense CI (libCI) unit tests on a remote Jetson device over SSH. Executes run-unit-tests.py with jetson/nightly context, collects test output, dmesg logs, and individual test logs, saves them locally, and produces a detailed analysis report. Use when the user wants to: run libCI, run librealsense tests, run unit tests on jetson, run live tests, run nightly tests, test camera with librealsense, run libCI regression, run libCI with repeat."
---

# libCI Test Runner

Run librealsense CI unit tests on a remote Jetson, collect all logs, and analyze results.

## Default Parameters

| Parameter | Default | Notes |
|-----------|---------|-------|
| SSH host | `fw-orin-1` | Override if user specifies different host |
| SSH user | `nvidia` | Override if user specifies different user |
| Test runner path | `~/dev/librealsense/unit-tests` | The `run-unit-tests.py` script location |
| Test log output dir | `~/dev/librealsense/build/Release/unit-tests` | Where individual `.log` files land on Jetson |
| Context | `jetson nightly` | Passed to `--context` |
| Local log dir | `test.logs/` | In workspace root |

## Workflow

### 1. Determine Parameters

Confirm or override defaults based on user request:

- **Host/user**: Use defaults (`nvidia@fw-orin-1`) unless user specifies otherwise
- **Test filter**: If user names specific tests, use `-r <regex>` (e.g., `-r live-metadata-alive`)
- **Repeat mode**: If user wants repeated runs, add `--repeat N --hw-reset-delay 1 --fail-fast`
- **Local log filename**: Generate from date: `libCI-DD-MM[-suffix].txt` where suffix is optional user label (branch name, commit hash, test filter, etc.)

### 2. Pre-Test Setup

Clear dmesg on the Jetson to get a clean baseline:

```bash
ssh nvidia@<host> "sudo dmesg -C"
```

If dmesg clear fails, log warning but continue.

### 3. Run the Tests

#### Full suite (default)

```bash
ssh nvidia@<host> "cd ~/dev/librealsense/unit-tests && python run-unit-tests.py -s --context 'jetson nightly' --live" 2>&1
```

#### Filtered by test name

```bash
ssh nvidia@<host> "cd ~/dev/librealsense/unit-tests && python run-unit-tests.py -s --context 'jetson nightly' --live -r <regex>" 2>&1
```

#### Repeated runs (stress/stability)

```bash
ssh nvidia@<host> "cd ~/dev/librealsense/unit-tests && python run-unit-tests.py --context 'jetson nightly' --live -r <regex> --repeat <N> --hw-reset-delay 1 --fail-fast" 2>&1
```

**Terminal strategy**: The full suite takes 30-60+ minutes. Use `isBackground=true` in `run_in_terminal` for full-suite or high-repeat runs; poll with `get_terminal_output` periodically to show progress. Use `isBackground=false` for short filtered runs (single test, low repeat count) so output streams in real-time. For repeat mode, scale timeout expectation by repeat count.

### 4. Capture dmesg

After the test completes, capture the full dmesg:

```bash
ssh nvidia@<host> "sudo dmesg --time-format=reltime" 2>&1
```

Save to local log dir as `dmesg.txt` (or with date prefix).

### 5. Collect Individual Test Logs

libCI writes per-test `.log` files to the build output directory. Fetch them:

```bash
scp nvidia@<host>:~/dev/librealsense/build/Release/unit-tests/*.log <local_log_dir>/
```

These logs contain detailed per-test output including frame data, timing, and stack traces not shown in the main output.

### 6. Save All Logs Locally

Save artifacts to `test.logs/` in the workspace root:

- **Test output**: `test.logs/libCI-DD-MM[-suffix].txt` — the full stdout/stderr from `run-unit-tests.py`
- **dmesg**: `test.logs/dmesg-DD-MM[-suffix].txt` — kernel log captured after test
- **Individual test logs**: `test.logs/DD-MM[-suffix]/` — directory with per-test `.log` files from Jetson

Use the same date/suffix for all artifacts from one run so they correlate.

### 7. Analyze Results

#### Parse test output

Scan the libCI output for these key patterns:

**Test execution lines:**
- `-I- Running <test-name>  [<config> -> <device>]` — test started
- `-I- Running <test-name>  [<config> -> <device>][rep N]` — repeated run
- `-I- Running <test-name>  [<config> -> <device>][retry N]` — retry after failure

**Result lines:**
- `All tests passed (N assertions in M test cases)` — test passed
- `-E- <test-name>: [<config>] exited with non-zero value (N)` — test failed
- `-W- <test-name>: no device matches configuration "<device>"` — test skipped (not a failure)
- `This test is dedicated to run with <model> only - stepping over test` — test skipped by design

**Summary line (at end of full suite):**
- `N of M test(s) failed!` — overall failure count
- If absent and tests ran, all passed

**Error detail lines:**
- `-E- Traceback` — Python exception in test
- `-E- Test failed` — assertion failure
- `-W- <test-name>: <error details>` — warning (may indicate flaky behavior)
- `left :` / `right :` — assertion comparison values

#### Parse dmesg

Scan dmesg for D4XX/GMSL-specific patterns. Load [references/libci_errors.md](./references/libci_errors.md) for the full error catalog.

Key patterns:
- `d4xx` with `error`, `fail`, `timeout`, `fault`
- `max9295` or `max9296` errors
- `tegra-capture-vi` errors
- `i2c` transfer errors on camera buses
- `GMSL` link errors
- Kernel oops, panics, BUG lines

### 8. Summary Report

Present results in this format:

```
## libCI Test Report

**Target:** nvidia@<host>
**Date:** <date>
**Command:** `python run-unit-tests.py <full args>`
**Branch/Commit:** <if known>

### Summary

| Metric | Value |
|--------|-------|
| Total tests | N |
| Passed | N |
| Failed | N |
| Skipped | N |
| Duration | ~Xm |

### Failed Tests

| # | Test Name | Error Summary |
|---|-----------|---------------|
| 1 | test-live-metadata-alive | Remote I/O error (errno=121) |
| 2 | test-live-frames-fps | FPS deviation >5% on Color stream |
| ... | ... | ... |

### Skipped Tests (not failures)
- test-X: no device matches "D555"
- test-Y: dedicated to D405 only

### Error Analysis

#### By Category
- **I2C/Communication**: N failures (errno=121, remote I/O)
- **Streaming/FPS**: N failures (deviation, timeout)
- **Firmware**: N failures (FW update, DFU)
- **Calibration**: N failures (OCC, low fill factor)
- **Framework**: N failures (pipeline, recording)

#### dmesg Correlation
<Match test failure times with dmesg errors. Note any driver errors around failure timestamps.>

### Comparison with Previous Run

**This section is mandatory.** Always auto-compare against the most recent previous libCI log in `test.logs/`.

Procedure:
1. Find the most recent `test.logs/libCI-*.txt` file (by modification time, excluding the current run's file).
2. Extract failed test names from both runs using the pattern: `-E- <test-name>: [<config>] exited with non-zero value`
3. Compute three sets:
   - **Regressions** (new failures): tests that fail now but passed (or were absent) in the previous run
   - **Fixed** (resolved failures): tests that failed previously but pass now
   - **Persistent**: tests that fail in both runs
4. Present as a table:

| Status | Test Name |
|--------|-----------|
| REGRESSION | test-live-frames-fps |
| FIXED | test-live-metadata-alive |
| PERSISTENT | test-fw-update |

If no previous log exists, state "No previous run found for comparison" and skip.

### Root Cause Analysis
1. **<Primary issue>**
   - Evidence: <supporting data from test output + dmesg>
   - Impact: <which tests affected>
   - Recommendation: <fix or investigate>

### Logs Saved
- Test output: `test.logs/libCI-DD-MM-suffix.txt`
- dmesg: `test.logs/dmesg-DD-MM-suffix.txt`
- Per-test logs: `test.logs/DD-MM-suffix/`
```

### Analysis Guidelines

- **Distinguish known-benign skips from real failures**: Tests skipped for "no device matches D555" or "dedicated to D405 only" are expected on D457 setups — do not count as failures.
- **Correlate dmesg timestamps with test failures**: Match `-E-` lines with kernel log timestamps for causal analysis.
- **Compare to previous runs**: Check `test.logs/libCI-*.txt` for historical results. Flag regressions (tests that previously passed but now fail) vs. known failures.
- **I2C errno=121 patterns**: `Remote I/O error` indicates I2C communication failure — check dmesg for retry counts and SerDes link status.
- **FPS deviations**: Check if the camera was still recovering from a previous test's HW reset.
- **Repeat-mode analysis**: Track which iteration first failed, whether failures are consistent or intermittent, whether they escalate (suggesting resource leaks).

## Edge Cases

**Test runner not found**: If `~/dev/librealsense/unit-tests/run-unit-tests.py` doesn't exist, try `~/librealsense/unit-tests/run-unit-tests.py` as fallback. Ask user for path if neither works.

**SSH connection failure**: Verify connectivity with `ssh nvidia@<host> 'echo OK'` before attempting the full run.

**Test hangs**: If no output for >5 minutes during a single test, the test may be hung. Consider killing via `Ctrl-C` or timeout, collect partial logs and dmesg.

**Partial output**: If SSH disconnects mid-run, save whatever output was captured and collect dmesg anyway — partial results are still valuable for diagnosis.

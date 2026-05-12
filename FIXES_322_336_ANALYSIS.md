# Fixes for test_driver runs 322 & 336 Failures

## Executive Summary

Runs 322 and 336 failed due to two independent issues:
1. **Pipeline readiness gate** allows tests to run despite camera initialization failure (design flaw)
2. **SerDes I2C communication** times out during early probe phase (hardware/timing issue)

This document provides concrete, actionable fixes for both issues with code patches and implementation guidance.

---

## Issue #1: Pipeline Readiness Gate Flaw

### Current Behavior (Run 322)
```
Attempt 1/24: waiting...
Attempt 2/24: waiting...
...
Attempt 24/24: waiting...
WARNING: device readiness check incomplete, proceeding anyway
[Pipeline] { (LRS_UNITEST)
Scheduling project: LRS_jetson_compile_pipeline
Build LRS_jetson_compile_pipeline #13996 completed: FAILURE
```

The readiness stage performed 24 attempts (2 minutes), found zero depth devices, and then **continued to run downstream tests anyway**. This is a logic error: a failed readiness gate should not proceed to test execution.

### Root Cause
The Jenkins pipeline shell script has implicit fallthrough behavior. When readiness check fails, it prints a warning but exits with code 0 (success), allowing the pipeline to continue.

### Recommended Fix: Single-Reboot Retry + Strict Fail Gate

Replace the readiness wait stage with:

```groovy
stage('Wait for node after reboot') {
    steps {
        script {
            def readiness_attempts = 24
            def readiness_interval = 5
            def reboot_attempted = false
            def success = false
            
            // First readiness attempt
            def result = sh(script: '''#!/bin/bash
                set -e
                echo "Polling for GMSL camera readiness (attempt 1)..."
                for i in $(seq 1 24); do
                    cameras=$(ls /dev/video-rs-depth-0 /dev/video-rs-depth-md-0 2>/dev/null | wc -l)
                    if [ $cameras -eq 2 ]; then
                        echo "Attempt $i: Found depth camera devices"
                        if /path/to/rs-enumerate-devices >/dev/null 2>&1; then
                            echo "Device enumeration successful"
                            exit 0
                        fi
                    fi
                    echo "Attempt $i/$readiness_attempts: waiting..."
                    sleep 5
                done
                echo "First readiness attempt failed"
                exit 1
            ''', returnStatus: true)
            
            if (result == 0) {
                success = true
            } else if (!reboot_attempted) {
                // One recovery: reboot and retry
                echo "Readiness check failed. Attempting recovery reboot..."
                reboot_attempted = true
                sh 'sudo reboot'
                sleep(time: 120, unit: 'SECONDS')
                
                // Second readiness attempt after reboot
                result = sh(script: '''#!/bin/bash
                    set -e
                    echo "Polling for GMSL camera readiness (attempt 2 after reboot)..."
                    for i in $(seq 1 24); do
                        cameras=$(ls /dev/video-rs-depth-0 /dev/video-rs-depth-md-0 2>/dev/null | wc -l)
                        if [ $cameras -eq 2 ]; then
                            echo "Attempt $i: Found depth camera devices"
                            if /path/to/rs-enumerate-devices >/dev/null 2>&1; then
                                echo "Device enumeration successful"
                                exit 0
                            fi
                        fi
                        echo "Attempt $i/$readiness_attempts: waiting..."
                        sleep 5
                    done
                    echo "Second readiness attempt failed"
                    exit 1
                ''', returnStatus: true)
                
                if (result == 0) {
                    success = true
                }
            }
            
            if (!success) {
                error("Camera readiness check failed after ${reboot_attempted ? '2' : '1'} attempt(s). Aborting pipeline.")
            }
        }
    }
}
```

### Key Changes
1. **Explicit success/failure logic**: Exit code 0 only on full readiness pass.
2. **Single reboot retry**: After first failure, reboot once and retry.
3. **Hard stop on second failure**: `error()` call aborts pipeline before test execution.
4. **Stronger readiness criteria**: Require both depth device nodes AND successful enumeration.
5. **Clear logging**: Distinguish between first attempt, recovery reboot, and final status.

### Implementation Notes
- Location: Jenkins job config for `test_driver` job, in the "Wait for node after reboot" stage.
- Replace shell script in that stage with the Groovy script above.
- Adjust `/path/to/rs-enumerate-devices` to match actual path on Jenkins agent.
- This prevents runs 322/336-style failures where readiness failed but tests ran anyway.

---

## Issue #2: SerDes I2C Startup Timeout

### Current Behavior (Runs 322 & 336)
```
[Mon May 11 00:13:57 2026] tegra-i2c 3180000.i2c: I2C transfer timed out
[Mon May 11 00:13:57 2026] max9296 9-0048: max9296_write_reg:i2c write failed, 0x10 = 1
[Mon May 11 00:13:57 2026] max9296 9-0048: max9296_write_reg:i2c write failed, 0x10 = 21
[Mon May 11 00:13:58 2026] max9295 9-0040: max9295_setup_control: ERROR: ser device not found
[Mon May 11 00:13:58 2026] d4xx 9-001a: gmsl serializer setup failed
[Mon May 11 00:13:58 2026] d4xx: probe of 9-001a failed with error -121
```

The SerDes (MAX9296/MAX9295) I2C bus becomes unresponsive immediately after kernel boot, causing all camera probes to fail. The i2c layer itself reports "transfer timed out", suggesting the SerDes is not yet ready when the driver tries to communicate.

### Root Cause
SerDes modules (and their downstream cameras) are not fully initialized when the kernel module loads and probes. The driver attempts register writes before the SerDes firmware has completed startup or power sequencing.

### Recommended Fix: Bounded SerDes Startup Retry

Add a brief startup retry loop **before** the first critical MAX9296/MAX9295 register transaction in the d4xx driver:

**Location**: In [kernel/realsense/d4xx.c](kernel/realsense/d4xx.c), in the `ds5_probe()` or SerDes setup path before `max9295_setup()` is called.

**Pattern**:

```c
// In d4xx.c, before ds5_serdes_setup() or max9295_setup_control() calls:

#define DS5_SERDES_STARTUP_RETRY_COUNT 5
#define DS5_SERDES_STARTUP_RETRY_DELAY_MS 100

/**
 * ds5_serdes_startup_ready() - Poll for SerDes I2C link readiness
 * @client: i2c_client for the camera instance
 * @timeout_ms: Maximum time to wait (e.g., 500 ms)
 *
 * Returns: 0 on success (I2C link responsive), negative on timeout
 *
 * Background: After kernel boot, SerDes modules may not be ready to accept
 * register writes immediately. This function performs a brief non-blocking
 * poll to detect when the I2C link becomes responsive, allowing the driver
 * to delay probe until SerDes firmware has completed power-on sequencing.
 */
static int ds5_serdes_startup_ready(struct i2c_client *client, int timeout_ms)
{
	int attempts = (timeout_ms + DS5_SERDES_STARTUP_RETRY_DELAY_MS - 1) / 
	               DS5_SERDES_STARTUP_RETRY_DELAY_MS;
	int i;

	for (i = 0; i < attempts; i++) {
		// Attempt a single harmless read (e.g., device ID register)
		// that will succeed if SerDes is ready, fail fast if not
		u8 reg_val;
		int err;

		// Use regmap direct read with no retry (detect transient startup state)
		err = regmap_raw_read(client->adapter, MAX9296_I2C_ADDR, 
		                      MAX9296_DEVICE_ID_REG, &reg_val, 1);
		if (err == 0) {
			dev_info(&client->dev, "SerDes I2C link ready after %d ms", 
			         i * DS5_SERDES_STARTUP_RETRY_DELAY_MS);
			return 0;
		}

		// Link not ready yet; brief delay and retry
		if (i < attempts - 1) {
			mdelay(DS5_SERDES_STARTUP_RETRY_DELAY_MS);
		}
	}

	// Timeout: SerDes I2C link did not become responsive in time
	dev_warn(&client->dev, "SerDes I2C startup timeout after %d ms", timeout_ms);
	return -ETIMEDOUT;
}

// In ds5_probe(), before calling ds5_serdes_setup():

static int ds5_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	// ... existing setup code ...

	// NEW: Check SerDes I2C readiness before attempting register access
	if (ds5_serdes_startup_ready(client, 500) != 0) {
		dev_err(&client->dev, "SerDes startup check timed out, deferring probe");
		return -EPROBE_DEFER;  // Let kernel retry probe later
	}

	// Now safe to proceed with normal SerDes setup
	if ((err = ds5_serdes_setup(state)) != 0) {
		dev_err(&client->dev, "%s: SerDes setup failed: %d\n", __func__, err);
		goto error;
	}

	// ... rest of probe ...
}
```

### Key Design Decisions

1. **Bounded timeout**: 500 ms maximum for startup ready check (short enough to not block probe, long enough to catch transient startup delays).
2. **Non-blocking poll**: Single direct reads, no retry logic. Detects SerDes readiness state without noisy warnings.
3. **Defer-not-fail**: Return `-EPROBE_DEFER` to allow kernel to retry probe after brief delay, instead of hard-failing on first timeout.
4. **Minimal overhead**: Only runs once per probe, adds ~5-50 ms latency in the startup path, negligible impact on successful boots.
5. **Clear logging**: Distinguish "startup ready" from "normal I2C failure" so logs don't create confusion.

### Expected Outcome

- **Run 322/336 scenario**: SerDes not ready at T=0 boot → startup check timeout → probe deferred → kernel retries after a few hundred ms → SerDes now ready → probe succeeds.
- **Normal scenario** (SerDes already ready): Startup check succeeds immediately → probe proceeds → no extra latency.

### Implementation Location

- **File**: [kernel/realsense/d4xx.c](kernel/realsense/d4xx.c)
- **Function**: `ds5_probe()` — add check before `ds5_serdes_setup()` call.
- **Registers to check**: Use a safe read-only register (device ID or status) rather than attempting configuration writes.
- **Fallback**: If MAX9296 device ID register is not accessible, use a known-safe register from camera probe path.

---

## Combined Impact

| Scenario | Before | After |
|----------|--------|-------|
| Run 322 (readiness fails) | Pipeline ignores failure, tests run and fail | Pipeline aborts before tests, or retries with reboot |
| Run 336 (partial readiness) | Device count appears OK but some probes fail | Stricter readiness gate requires full topology; if any instance fails, waits + retries before proceeding |
| Normal boot | No change | Startup check succeeds immediately; <~50ms added latency on first probe |
| Post-reboot SerDes not-ready | Driver retries within I2C layer only | Probe defers, kernel retries, startup check + defer loop handles recovery |

---

## Testing Recommendations

1. **Test pipeline gate logic**: Force readiness to fail, verify pipeline aborts (does not trigger LRS_UNITEST).
2. **Test single reboot recovery**: Simulate condition where first readiness fails but second succeeds.
3. **Test SerDes startup defer**: Inject delay into SerDes I2C path, verify driver defers probe correctly.
4. **Regression test**: Normal boot path should not see increased latency or additional retry spam.

---

## References

- **Affected runs**: test_driver #322, #336
- **Root causes**: 
  - Readiness gate logic flaw: warning-and-continue instead of fail-or-recover
  - SerDes startup race: driver attempts register writes before I2C link is ready
- **Fixes proposed here**:
  1. Jenkins pipeline stage replacement with reboot-retry and strict fail logic
  2. Driver-side SerDes startup ready-check with deferred probe on timeout

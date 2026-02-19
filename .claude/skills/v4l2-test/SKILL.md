---
name: v4l2-test
description: Run V4L2 pytest tests on a remote Jetson device over SSH. Use when the user wants to run v4l2 tests, test the V4L2 driver on Jetson, run pytest for v4l2_test, check V4L2 functionality, or validate camera driver changes. Collects test results, dmesg logs, and provides detailed analysis of failures including error classification and root cause diagnosis.
---

# V4L2 Test Runner

Execute V4L2 pytest tests on a remote Jetson device, collect comprehensive diagnostics, and provide detailed failure analysis.

## Workflow

### 1. Obtain SSH Credentials

**Check memory first:**
- Read `/home/administrator/.claude/projects/-home-administrator-realsense-mipi-platform-driver-tests/memory/MEMORY.md`
- Search for stored Jetson SSH credentials (hostname, username)

**If credentials not in memory:**
- Ask user for Jetson hostname/IP and SSH username
- Save to memory file for future use:
  ```markdown
  ## Jetson SSH Credentials
  - **Hostname**: <hostname>
  - **Username**: <username>
  ```

**SSH key requirement:**
- Passwordless SSH must be configured (script uses ssh without password prompt)
- If user reports connection issues, verify: `ssh <user>@<host> 'echo OK'`

**Sudo access requirement:**
- The script requires passwordless sudo access for: `rmmod`, `modprobe`, and `dmesg`
- If user doesn't have passwordless sudo, pre-flight checks may fail
- Test with: `ssh <user>@<host> 'sudo -n echo OK'`

### 2. Determine Test Scope

**Default**: Run all v4l2 tests

**If user specifies a test:**
- Use the `--test` parameter with pytest pattern matching
- Examples:
  - "streaming tests" → `--test streaming`
  - "control tests" → `--test control`
  - "specific test like test_depth_streaming" → `--test test_depth_streaming`

### 3. Run the Test Script

Execute the test runner script:

```bash
cd /home/administrator/realsense_mipi_platform_driver/tests/.claude/skills/v4l2-test
python3 scripts/run_v4l2_tests.py --host <hostname> --user <username> [--test <pattern>]
```

**The script will:**
1. Verify SSH connection to Jetson
2. Locate the v4l2_test directory on the remote device
3. Collect system information (JetPack version, kernel, video devices)
4. **Pre-flight checks:**
   - Reload D4XX driver: `sudo rmmod d4xx; sudo modprobe d4xx`
   - Send hw_reset to camera via V4L2 control
   - Run basic sanity check to verify camera is connected and responsive
   - **If sanity check fails, stop execution and report issue**
5. Run pytest with appropriate parameters (only if sanity check passed)
6. Collect dmesg logs (filtered for D4XX, GMSL, V4L2, NVCSI, VI drivers)
7. Save all outputs to `./v4l2_test_results/` directory

**Note**: The script shows real-time test output so the user can follow progress.

### 4. Analyze Test Results

After the script completes, analyze the collected data:

**Read the output files:**
- `system_info_*.json` - Jetson configuration
- `dmesg_*.log` - Kernel driver logs

**Parse pytest output** from the script output to identify:
- Total tests run
- Pass/fail counts
- Failed test names and error messages
- Test duration

**Consult references for analysis:**

Load [references/v4l2_errors.md](references/v4l2_errors.md) to:
- Interpret V4L2 ioctl errors (errno values, common causes)
- Understand dmesg error patterns
- Map errors to subsystems (D4XX driver, NVCSI, VI, SerDes)

Load [references/test_structure.md](references/test_structure.md) to:
- Understand what each test validates
- Determine typical failure causes by test category
- Context for expected test behavior

**Error classification:**
- Categorize errors by type (streaming, format negotiation, controls, detection)
- Identify affected subsystem (driver, V4L2 core, hardware)
- Determine severity (critical, major, minor)

### 5. Generate Summary Report

Produce a comprehensive markdown report with the following structure:

```markdown
# V4L2 Test Results - <Timestamp>

## Summary
- **Jetson**: <hostname> (JetPack <version>, Kernel <version>)
- **Total Tests**: <count>
- **Passed**: <count> (<percentage>%)
- **Failed**: <count> (<percentage>%)
- **Duration**: <time>

## Test Pass/Fail Breakdown
<table showing pass/fail by test category>

## Failed Tests

### <Test Name>
- **Error**: <error message>
- **Category**: <streaming/controls/detection/metadata/error_handling>
- **Root Cause**: <analysis of cause>
- **Dmesg Correlation**: <relevant dmesg errors if any>

<repeat for each failed test>

## Error Analysis

### Error Type Distribution
<count of each error type: STREAMON failures, I/O errors, invalid argument, etc.>

### Affected Subsystems
<which components had errors: D4XX driver, NVCSI, VI, SerDes, etc.>

### Error Patterns
<any recurring patterns, e.g., "All streaming tests fail with I/O error">

## Dmesg Highlights
<critical errors from dmesg, especially around test failure times>

## Root Cause Analysis
<your conclusions about the underlying issues>

### Primary Issues
1. <main issue identified>
   - Evidence: <supporting data>
   - Impact: <what tests/functionality affected>
   - Recommendation: <how to fix or investigate further>

<repeat for additional issues>

## Recommendations
1. <action item 1>
2. <action item 2>
<prioritized list of next steps>

## Appendix
- System info: <path to system_info file>
- Dmesg log: <path to dmesg log file>
```

**Analysis guidelines:**
- Be specific about root causes (don't just repeat error messages)
- Correlate test failures with dmesg entries
- Provide actionable recommendations
- Distinguish between code issues, configuration issues, and hardware issues
- Note if failures are consistent or intermittent

## Memory Management

**After each test run**, update memory with:
- SSH credentials (if not already stored)
- Any persistent issues discovered
- Useful diagnostic commands or workarounds

## Edge Cases

**If sanity check fails:**
- The script will stop before running full tests (saving time on known-broken setups)
- Check the pre-flight output to see which check failed:
  - **Video devices not found**: Driver not loaded or camera not connected
  - **Cannot query capabilities**: I2C communication failure (check SerDes configuration)
  - **Cannot stream**: Critical hardware or driver issue (check dmesg for errors)
- Diagnostic logs (system info, dmesg) will still be collected
- Investigate root cause before attempting full test suite

**If driver reload fails:**
- Module may not exist: check if d4xx.ko is installed
- Module may be in use: other processes accessing video devices
- Run manually: `ssh <user>@<host> 'sudo rmmod d4xx && sudo modprobe d4xx'`

**If test directory not found:**
- Script checks common paths, but may fail on non-standard installations
- Ask user for the path and update script invocation

**If all tests are skipped:**
- Usually means "No D4XX cameras detected"
- Check: driver loaded (`lsmod | grep d4xx`), video devices (`ls /dev/video*`)
- Collect dmesg to diagnose why camera not detected

**If script hangs:**
- Tests have 600s timeout, but individual tests might hang
- Check SSH connection stability
- May need to manually kill pytest on Jetson

**If dmesg collection fails:**
- Requires passwordless sudo access on Jetson
- If user doesn't have sudo, skip dmesg analysis and note in report

## Reference Files

- **[v4l2_errors.md](references/v4l2_errors.md)**: Comprehensive V4L2 error reference for interpreting test failures and dmesg logs
- **[test_structure.md](references/test_structure.md)**: Detailed test organization, categories, and expected behavior

These references should be loaded on-demand during analysis to understand specific errors and test purposes.

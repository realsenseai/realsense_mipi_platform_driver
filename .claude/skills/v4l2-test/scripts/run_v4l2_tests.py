#!/usr/bin/env python3
"""
Run V4L2 pytest tests on a remote Jetson device over SSH.

Usage:
    python3 run_v4l2_tests.py --host <hostname> --user <username> [--test <test_pattern>] [--output <dir>]

Example:
    python3 run_v4l2_tests.py --host jetson.local --user nvidia
    python3 run_v4l2_tests.py --host 192.168.1.100 --user nvidia --test test_streaming
"""

import argparse
import subprocess
import sys
import json
from pathlib import Path
from datetime import datetime


class V4L2TestRunner:
    def __init__(self, host, user, test_pattern=None, output_dir=None):
        self.host = host
        self.user = user
        self.test_pattern = test_pattern
        self.output_dir = Path(output_dir) if output_dir else Path.cwd()
        self.ssh_base = f"{user}@{host}"

    def run_ssh_command(self, command, capture_output=True):
        """Execute command on remote host via SSH."""
        ssh_cmd = ["ssh", self.ssh_base, command]
        try:
            result = subprocess.run(
                ssh_cmd,
                capture_output=capture_output,
                text=True,
                timeout=600  # 10 minute timeout
            )
            return result
        except subprocess.TimeoutExpired:
            print(f"ERROR: Command timed out after 600 seconds", file=sys.stderr)
            return None
        except Exception as e:
            print(f"ERROR: SSH command failed: {e}", file=sys.stderr)
            return None

    def check_connection(self):
        """Verify SSH connection to Jetson."""
        print(f"Checking SSH connection to {self.ssh_base}...")
        result = self.run_ssh_command("echo 'Connection OK'")
        if result and result.returncode == 0:
            print("✓ SSH connection successful")
            return True
        else:
            print("✗ SSH connection failed")
            return False

    def find_test_directory(self):
        """Locate the v4l2_test directory on remote device."""
        print("Locating v4l2_test directory...")

        # Common paths to check
        paths_to_check = [
            "~/realsense_mipi_platform_driver/tests/v4l2_test",
            "~/realsense_tests/test/v4l2_test",
            "~/tests/v4l2_test",
            "/home/*/realsense_mipi_platform_driver/tests/v4l2_test",
        ]

        for path in paths_to_check:
            result = self.run_ssh_command(f"test -d {path} && echo 'FOUND' || echo 'NOT_FOUND'")
            if result and "FOUND" in result.stdout:
                # Get absolute path
                result = self.run_ssh_command(f"cd {path} && pwd")
                if result and result.returncode == 0:
                    test_dir = result.stdout.strip()
                    print(f"✓ Found test directory: {test_dir}")
                    return test_dir

        print("✗ Could not find v4l2_test directory")
        return None

    def reload_driver(self):
        """Reload the D4XX driver module."""
        print("\n" + "="*60)
        print("Pre-flight Check: Reloading D4XX driver...")
        print("="*60 + "\n")

        # Remove module
        print("Removing d4xx module...")
        result = self.run_ssh_command("sudo rmmod d4xx")
        if result and result.returncode == 0:
            print("✓ Module removed")
        else:
            print("⚠ Warning: Module removal failed or module not loaded")

        # Wait a moment
        import time
        time.sleep(1)

        # Load module
        print("Loading d4xx module...")
        result = self.run_ssh_command("sudo modprobe d4xx")
        if result and result.returncode == 0:
            print("✓ Module loaded successfully")
            time.sleep(2)  # Give driver time to initialize
            return True
        else:
            print("✗ Failed to load d4xx module")
            return False

    def send_hw_reset(self):
        """Send hardware reset to the camera via V4L2 control."""
        print("\nSending hardware reset to camera...")

        # Find first video device (typically video0 for depth)
        result = self.run_ssh_command("ls /dev/video0 2>/dev/null")
        if not result or result.returncode != 0:
            print("✗ No video devices found")
            return False

        # Send hw_reset control using v4l2-ctl
        # hw_reset is typically control ID 0x009a2064
        reset_cmd = "v4l2-ctl -d /dev/video0 --set-ctrl=hw_reset=1"
        result = self.run_ssh_command(reset_cmd)
        if result and result.returncode == 0:
            print("✓ Hardware reset sent successfully")
            import time
            time.sleep(2)  # Wait for reset to complete
            return True
        else:
            print("⚠ Warning: Hardware reset command failed (may not be supported)")
            # Don't fail here as hw_reset might not be available on all setups
            return True

    def run_sanity_check(self):
        """Run basic sanity check to verify camera is connected and responsive."""
        print("\n" + "="*60)
        print("Pre-flight Check: Camera Sanity Check")
        print("="*60 + "\n")

        # Check if video devices exist
        print("Checking for video devices...")
        result = self.run_ssh_command("ls /dev/video* 2>/dev/null | head -6")
        if not result or result.returncode != 0 or not result.stdout.strip():
            print("✗ FAIL: No video devices found")
            print("   Camera is not detected by the system")
            return False

        video_devices = result.stdout.strip().split('\n')
        print(f"✓ Found {len(video_devices)} video devices:")
        for dev in video_devices:
            print(f"  - {dev}")

        # Check if D4XX module is loaded
        print("\nChecking D4XX driver...")
        result = self.run_ssh_command("lsmod | grep d4xx")
        if not result or result.returncode != 0 or not result.stdout.strip():
            print("✗ FAIL: D4XX driver not loaded")
            return False
        print("✓ D4XX driver loaded")

        # Try to query capabilities on video0
        print("\nQuerying camera capabilities...")
        result = self.run_ssh_command("v4l2-ctl -d /dev/video0 --info")
        if not result or result.returncode != 0:
            print("✗ FAIL: Cannot query camera capabilities")
            print("   Camera is not responding to V4L2 commands")
            return False

        # Check for D4XX in the output
        if "d4xx" not in result.stdout.lower() and "realsense" not in result.stdout.lower():
            print("⚠ Warning: Device does not appear to be a D4XX camera")
            print(f"Device info:\n{result.stdout}")

        print("✓ Camera responds to V4L2 queries")

        # Try to list formats
        print("\nChecking supported formats...")
        result = self.run_ssh_command("v4l2-ctl -d /dev/video0 --list-formats-ext 2>&1 | head -20")
        if not result or result.returncode != 0:
            print("✗ FAIL: Cannot list camera formats")
            print("   Camera may not be properly initialized")
            return False

        if "Z16" not in result.stdout and "GREY" not in result.stdout:
            print("⚠ Warning: Expected depth formats not found")
            print(f"Available formats:\n{result.stdout}")
        else:
            print("✓ Camera formats available")

        # Try a quick streaming test (capture 1 frame)
        print("\nTesting basic streaming...")
        stream_test_cmd = (
            "timeout 10 v4l2-ctl -d /dev/video0 "
            "--set-fmt-video=width=640,height=480 "
            "--stream-mmap --stream-count=1 2>&1"
        )
        result = self.run_ssh_command(stream_test_cmd)
        if not result or result.returncode != 0:
            print("✗ FAIL: Cannot stream from camera")
            if result and result.stdout:
                print(f"   Error: {result.stdout.strip()}")
            print("\n   This indicates a critical issue with camera communication")
            print("   Full test suite will likely fail - stopping execution")
            return False

        if "captured 1" in result.stdout.lower() or "<<" in result.stdout:
            print("✓ Successfully captured test frame")
        else:
            print("⚠ Warning: Streaming output unexpected")
            print(f"Output: {result.stdout}")

        print("\n" + "="*60)
        print("✓ SANITY CHECK PASSED - Camera is operational")
        print("="*60 + "\n")
        return True

    def run_tests(self, test_dir):
        """Run pytest on remote device."""
        print("\n" + "="*60)
        print("Running V4L2 tests...")
        print("="*60 + "\n")

        # Build pytest command
        if self.test_pattern:
            pytest_cmd = f"cd {test_dir} && python3 -m pytest -vs -k '{self.test_pattern}' --tb=short"
        else:
            pytest_cmd = f"cd {test_dir} && python3 -m pytest -vs --tb=short"

        print(f"Command: {pytest_cmd}\n")

        # Run tests (don't capture output so user sees real-time progress)
        result = self.run_ssh_command(pytest_cmd, capture_output=False)

        print("\n" + "="*60)
        if result and result.returncode == 0:
            print("✓ All tests passed")
        else:
            print("✗ Some tests failed or error occurred")
        print("="*60 + "\n")

        return result

    def collect_dmesg_logs(self):
        """Collect dmesg logs from remote device."""
        print("Collecting dmesg logs...")

        # Collect dmesg with timestamp, filter for relevant drivers
        dmesg_cmd = (
            "sudo dmesg -T | grep -E '(d4xx|D4XX|GMSL|V4L2|video|media|tegra-camrtc|nvcsi|vi5)' | tail -500"
        )

        result = self.run_ssh_command(dmesg_cmd)
        if result and result.returncode == 0:
            dmesg_output = result.stdout

            # Save to file
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            dmesg_file = self.output_dir / f"dmesg_{timestamp}.log"
            dmesg_file.write_text(dmesg_output)

            print(f"✓ Dmesg logs saved to: {dmesg_file}")
            return dmesg_file
        else:
            print("✗ Failed to collect dmesg logs")
            return None

    def collect_system_info(self):
        """Collect system information from Jetson."""
        print("Collecting system information...")

        info = {}

        # JetPack version
        result = self.run_ssh_command("cat /etc/nv_tegra_release 2>/dev/null || echo 'N/A'")
        if result:
            info['jetpack'] = result.stdout.strip()

        # Kernel version
        result = self.run_ssh_command("uname -r")
        if result:
            info['kernel'] = result.stdout.strip()

        # Video devices
        result = self.run_ssh_command("ls -la /dev/video* 2>/dev/null || echo 'No video devices'")
        if result:
            info['video_devices'] = result.stdout.strip()

        # D4XX module
        result = self.run_ssh_command("lsmod | grep d4xx || echo 'Module not loaded'")
        if result:
            info['d4xx_module'] = result.stdout.strip()

        # Save to file
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        info_file = self.output_dir / f"system_info_{timestamp}.json"
        info_file.write_text(json.dumps(info, indent=2))

        print(f"✓ System info saved to: {info_file}")
        return info_file

    def run(self):
        """Execute the full test workflow."""
        print("\n" + "="*60)
        print("V4L2 Test Runner")
        print("="*60 + "\n")

        # Ensure output directory exists
        self.output_dir.mkdir(parents=True, exist_ok=True)

        # Check SSH connection
        if not self.check_connection():
            return 1

        # Find test directory
        test_dir = self.find_test_directory()
        if not test_dir:
            return 1

        # Collect system info
        self.collect_system_info()

        # PRE-FLIGHT CHECKS
        print("\n" + "="*60)
        print("Running pre-flight checks...")
        print("="*60 + "\n")

        # 1. Reload driver
        if not self.reload_driver():
            print("\n✗ CRITICAL: Driver reload failed")
            print("Cannot proceed with tests")
            return 1

        # 2. Send hardware reset
        self.send_hw_reset()

        # 3. Run sanity check
        if not self.run_sanity_check():
            print("\n✗ CRITICAL: Sanity check failed")
            print("Camera is not operational - skipping full test suite")
            print("\nCollecting diagnostic logs...")
            self.collect_dmesg_logs()
            print(f"\nDiagnostic data saved to: {self.output_dir}")
            return 1

        # Run tests (only if sanity check passed)
        test_result = self.run_tests(test_dir)

        # Collect dmesg logs
        self.collect_dmesg_logs()

        print("\n" + "="*60)
        print("Test run complete!")
        print(f"Results saved to: {self.output_dir}")
        print("="*60 + "\n")

        return 0 if (test_result and test_result.returncode == 0) else 1


def main():
    parser = argparse.ArgumentParser(
        description="Run V4L2 pytest tests on remote Jetson device"
    )
    parser.add_argument(
        "--host",
        required=True,
        help="Hostname or IP address of Jetson device"
    )
    parser.add_argument(
        "--user",
        required=True,
        help="SSH username for Jetson device"
    )
    parser.add_argument(
        "--test",
        help="Test pattern to run (pytest -k argument). If not specified, runs all tests."
    )
    parser.add_argument(
        "--output",
        default="./v4l2_test_results",
        help="Output directory for test results and logs (default: ./v4l2_test_results)"
    )

    args = parser.parse_args()

    runner = V4L2TestRunner(
        host=args.host,
        user=args.user,
        test_pattern=args.test,
        output_dir=args.output
    )

    sys.exit(runner.run())


if __name__ == "__main__":
    main()

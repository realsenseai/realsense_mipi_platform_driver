#!/usr/bin/env python3
"""
Parallel streaming test for any number of V4L2 video devices.

Starts streaming on all video devices in parallel, verifies frames arrive
on all after a defined period, stops streams, and repeats for N iterations.

Usage:
    python3 parallel_stream_test.py /dev/video0 /dev/video2 /dev/video7 [--iterations N] [--duration SECS]
"""

import argparse
import subprocess
import time
import signal
import sys
import os
import re
from typing import Tuple, Optional, List


def start_stream(video_dev: str) -> subprocess.Popen:
    """Start v4l2-ctl streaming on a video device."""
    proc = subprocess.Popen(
        ["v4l2-ctl", "-d", video_dev, "--stream-mmap", "--stream-count=0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True
    )
    return proc


def stop_stream(proc: subprocess.Popen, timeout: float = 2.0) -> Tuple[bool, str]:
    """
    Stop a streaming process and return frame count info.
    
    Returns:
        Tuple of (success, output_text)
    """
    if proc.poll() is not None:
        # Process already terminated
        stdout, _ = proc.communicate()
        return proc.returncode == 0, stdout or ""
    
    # Send SIGINT to gracefully stop streaming
    proc.send_signal(signal.SIGINT)
    
    try:
        stdout, _ = proc.communicate(timeout=timeout)
        return True, stdout or ""
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        return False, stdout or ""


def parse_frame_count(output: str) -> int:
    """
    Parse frame count from v4l2-ctl output.
    
    v4l2-ctl prints lines like:
    '<' or '>' for each frame, or summary at end showing frame count
    """
    # Count frame markers (< for capture)
    frame_markers = output.count('<')
    if frame_markers > 0:
        return frame_markers
    
    # Try to parse "frames captured" from summary
    match = re.search(r'(\d+)\s+frames?\s+captured', output, re.IGNORECASE)
    if match:
        return int(match.group(1))
    
    # Count lines that look like frame output
    lines = output.strip().split('\n')
    frame_lines = sum(1 for line in lines if line.strip().startswith('<'))
    
    return frame_lines


def verify_device_exists(video_dev: str) -> bool:
    """Check if a video device exists."""
    return os.path.exists(video_dev)


def run_iteration(devices: List[str], duration: float) -> Tuple[bool, dict]:
    """
    Run one iteration of parallel streaming test on any number of devices.

    Returns:
        Tuple of (success, results_dict)
    """
    results = {
        "devices": devices,
        "frames": {dev: 0 for dev in devices},
        "success": {dev: False for dev in devices},
        "error": None,
    }

    # Start all streams in parallel
    try:
        procs = {dev: start_stream(dev) for dev in devices}
    except Exception as e:
        results["error"] = f"Failed to start streams: {e}"
        return False, results

    time.sleep(duration)

    # Stop all streams and collect results
    for dev, proc in procs.items():
        ok, output = stop_stream(proc)
        frames = parse_frame_count(output)
        results["frames"][dev] = frames
        results["success"][dev] = ok and frames > 0

    overall_success = all(results["success"].values())
    return overall_success, results


def main():
    parser = argparse.ArgumentParser(
        description="Test parallel streaming on any number of V4L2 video devices"
    )
    parser.add_argument(
        "devices",
        nargs="+",
        help="Video devices to stream in parallel (e.g., /dev/video0 /dev/video2 /dev/video7)"
    )
    parser.add_argument(
        "-i", "--iterations",
        type=int,
        default=100,
        help="Number of test iterations (default: 100)"
    )
    parser.add_argument(
        "-d", "--duration",
        type=float,
        default=2.0,
        help="Duration to stream per iteration in seconds (default: 2.0)"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output"
    )

    args = parser.parse_args()

    for dev in args.devices:
        if not verify_device_exists(dev):
            print(f"ERROR: Device {dev} does not exist")
            sys.exit(1)

    print(f"Parallel Streaming Test")
    print(f"========================")
    for i, dev in enumerate(args.devices, 1):
        print(f"Device {i}: {dev}")
    print(f"Iterations: {args.iterations}")
    print(f"Duration per iteration: {args.duration}s")
    print()

    passed = 0
    failed = 0

    for i in range(1, args.iterations + 1):
        print(f"Iteration {i}/{args.iterations}: ", end="", flush=True)

        success, results = run_iteration(args.devices, args.duration)

        if success:
            passed += 1
            frame_summary = ", ".join(str(results["frames"][d]) for d in args.devices)
            print(f"PASS (frames: {frame_summary})")
        else:
            failed += 1
            print(f"FAIL")
            if args.verbose or results["error"]:
                if results["error"]:
                    print(f"  Error: {results['error']}")
                for dev in args.devices:
                    print(f"  {dev}: {results['frames'][dev]} frames, success={results['success'][dev]}")

        if i < args.iterations:
            time.sleep(0.5)

    print()
    print(f"Results: {passed}/{args.iterations} passed, {failed} failed")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()

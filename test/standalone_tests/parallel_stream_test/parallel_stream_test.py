#!/usr/bin/env python3
"""
Parallel streaming test for two V4L2 video devices.

Starts streaming on two video devices in parallel, verifies frames arrive
on both after a defined period, stops streams, and repeats for N iterations.

Usage:
    python3 parallel_stream_test.py /dev/video0 /dev/video7 [--iterations N] [--duration SECS]
"""

import argparse
import subprocess
import time
import signal
import sys
import os
import re
from typing import Tuple, Optional


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


def run_iteration(dev1: str, dev2: str, duration: float) -> Tuple[bool, dict]:
    """
    Run one iteration of parallel streaming test.
    
    Returns:
        Tuple of (success, results_dict)
    """
    results = {
        "dev1": dev1,
        "dev2": dev2,
        "dev1_frames": 0,
        "dev2_frames": 0,
        "dev1_success": False,
        "dev2_success": False,
        "error": None
    }
    
    # Start both streams in parallel
    try:
        proc1 = start_stream(dev1)
        proc2 = start_stream(dev2)
    except Exception as e:
        results["error"] = f"Failed to start streams: {e}"
        return False, results
    
    # Wait for the specified duration
    time.sleep(duration)
    
    # Stop both streams
    success1, output1 = stop_stream(proc1)
    success2, output2 = stop_stream(proc2)
    
    # Parse frame counts
    frames1 = parse_frame_count(output1)
    frames2 = parse_frame_count(output2)
    
    results["dev1_frames"] = frames1
    results["dev2_frames"] = frames2
    results["dev1_success"] = success1 and frames1 > 0
    results["dev2_success"] = success2 and frames2 > 0
    
    overall_success = results["dev1_success"] and results["dev2_success"]
    
    return overall_success, results


def main():
    parser = argparse.ArgumentParser(
        description="Test parallel streaming on two V4L2 video devices"
    )
    parser.add_argument(
        "device1",
        help="First video device (e.g., /dev/video0)"
    )
    parser.add_argument(
        "device2",
        help="Second video device (e.g., /dev/video7)"
    )
    parser.add_argument(
        "-i", "--iterations",
        type=int,
        default=100,
        help="Number of test iterations (default: 10)"
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
    
    # Verify devices exist
    if not verify_device_exists(args.device1):
        print(f"ERROR: Device {args.device1} does not exist")
        sys.exit(1)
    if not verify_device_exists(args.device2):
        print(f"ERROR: Device {args.device2} does not exist")
        sys.exit(1)
    
    print(f"Parallel Streaming Test")
    print(f"========================")
    print(f"Device 1: {args.device1}")
    print(f"Device 2: {args.device2}")
    print(f"Iterations: {args.iterations}")
    print(f"Duration per iteration: {args.duration}s")
    print()
    
    passed = 0
    failed = 0
    
    for i in range(1, args.iterations + 1):
        print(f"Iteration {i}/{args.iterations}: ", end="", flush=True)
        
        success, results = run_iteration(
            args.device1,
            args.device2,
            args.duration
        )
        
        if success:
            passed += 1
            print(f"PASS (frames: {results['dev1_frames']}, {results['dev2_frames']})")
        else:
            failed += 1
            print(f"FAIL")
            if args.verbose or results["error"]:
                if results["error"]:
                    print(f"  Error: {results['error']}")
                print(f"  {args.device1}: {results['dev1_frames']} frames, success={results['dev1_success']}")
                print(f"  {args.device2}: {results['dev2_frames']} frames, success={results['dev2_success']}")
        
        # Small delay between iterations
        if i < args.iterations:
            time.sleep(0.5)
    
    print()
    print(f"Results: {passed}/{args.iterations} passed, {failed} failed")
    
    # Exit with error code if any failures
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()

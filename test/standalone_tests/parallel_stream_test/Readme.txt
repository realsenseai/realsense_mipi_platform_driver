This test starts any number of video devices in parallel using v4l2-ctl and verifies that frames arrived to all of them.

Usage:
    python parallel_stream_test.py <device1> [device2 ...] [-i ITERATIONS] [-d DURATION] [-v]

Examples:

python parallel_stream_test.py /dev/video0 /dev/video2 -i 10 -d 5
    Run video0 and video2 (depth and RGB of the same camera) for 10 iterations, 5 seconds each.

python parallel_stream_test.py /dev/video0 /dev/video7
    Run video0 and video7 (depth of camera 0 and depth of camera 1) with default iterations (100) and duration (2s).

python parallel_stream_test.py /dev/video0 /dev/video2 /dev/video7
    Run three devices in parallel with default settings.

Options:
    -i, --iterations N      Number of start/stop iterations (default: 100)
    -d, --duration SECS     Stream duration per iteration in seconds (default: 2.0)
    -v, --verbose           Print per-device frame counts and success status on failure
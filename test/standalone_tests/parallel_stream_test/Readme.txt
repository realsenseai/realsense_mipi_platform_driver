This test starts two given video devices in parallel using v4l2-ctl and verify that frames arrived to both
e.g:
python parallel_stream_test.py /dev/video0 /dev/video2 -i 10 -d 5
This will run video0 and video2 (generally depth and RGB of the same camera) for 10 iterations, 5 seconds per iterations

python parallel_stream_test.py /dev/video0 /dev/video7
This will run video0 and video7 (generally depth of camera 0 and depth of camera 1) with default iterations and duration
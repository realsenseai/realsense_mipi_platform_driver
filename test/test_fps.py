import subprocess
import pytest
import re

@pytest.mark.d457
@pytest.mark.parametrize("device", {'0', '2'})
@pytest.mark.parametrize("frames", {'100'})
@pytest.mark.parametrize("timeout", {10})
def test_fps(device, frames, timeout):
	try:
		cmd = [ "v4l2-ctl",
		 "-d" + device,
		 "--stream-mmap",
		 "--stream-count",
		 frames,
		 "--verbose"]
		output = subprocess.run(cmd,
						  check=True,
						  text=True,
						  capture_output=True,
						  timeout=timeout).stderr.splitlines()
		last = None
		for line in output:
			m = re.search(r"cap dqbuf:.*seq:\s*(\d*)\s*bytesused:.*fps:\s*(\d+\.\d+)\s*.*", line)
			if m:
				frame = int(m.group(1))
				fps = float(m.group(2))
				if last:
					print(f"frame: {frame}/{last}, fps: {fps}")
					assert frame != last, f"Repeated frame: {frame}"
					assert frame == last+1, f"Frame droped between: {last} and {frame}"
				last = frame

	except Exception as e:
		assert False, "Exception caught during test: {}".format(e)

import subprocess
import pytest
import re

@pytest.mark.d457
@pytest.mark.parametrize("frames", {20})
@pytest.mark.parametrize("device", {'0', '2'})
def test_fps(device, frames):
    try:
        print(f"\nDevice: {device}")
        formats = get_formats(device)
        for w, h in formats:
            print(f"Format: {w}x{h}")
            for FPS in formats[(w, h)]:
                cmd = [ "v4l2-ctl",
                       f"-d{device}",
                       f"--set-fmt-video=width={w},height={h}",
                       ]
                subprocess.check_call(cmd)
                print(f"FPS/{FPS}:", end=' ')
                cmd = [ "v4l2-ctl",
                       f"-d{device}",
                       "-p",
                       f"{FPS}",
                       ]
                subprocess.check_call(cmd, stdout=subprocess.DEVNULL)
                cmd = [ "v4l2-ctl",
                       f"-d{device}",
                       "--stream-mmap",
                       "--stream-count",
                       f"{frames}",
                       "--verbose",
                       ]
                timeout = 5.0 * frames / FPS
                output = subprocess.run(cmd,
                                        check=True,
                                        text=True,
                                        capture_output=True,
                                        timeout=timeout).stderr.splitlines()
                last = None
                kpi = 5 # [%]
                for line in output:
                    m = re.search(r"cap dqbuf:.*seq:\s*(\d*)\s*bytesused:.*", line)
                    if m:
                        frame = int(m.group(1))
                        print(f"{frame}", end=',')
                        if last:
                            assert frame > last, f"Repeated frame: {frame}"
                            assert (frame - last) < 3 , f"Frames dropped between: {last} and {frame}"
                        m = re.search(r"cap dqbuf:.*bytesused:.*fps:\s*(\d+\.\d+)\s*.*", line)
                        if m:
                            fps = float(m.group(1))
                            if last:
                                print(f"\b/{fps}", end=',')
                                assert fps > FPS * (1 - kpi/100), f"FPS too low: {fps}/{FPS}"
                                assert fps < FPS * (1 + kpi/100), f"FPS too high: {fps}/{FPS}"
                        last = frame
                print()
                assert last, "No frames received"
    except subprocess.TimeoutExpired:
        assert False, "No frames received"

def get_formats(device):
    cmd = [ "v4l2-ctl",
           "-d" + device,
           "--list-formats-ext",
           ]
    output = subprocess.run(cmd,
                            check=True,
                            text=True,
                            capture_output=True
                            ).stdout.splitlines()
    formats = {}
    last = None
    for line in output:
        m = re.search(r"\s*Size: Discrete\s*(\d+)x(\d+)", line)
        if m:
            w = int(m.group(1))
            h = int(m.group(2))
            last = (w ,h)
            if not last in formats:
                formats[last] = set()
            continue
        m = re.search(r"\s*Interval: Discrete.*\((\d+\.\d+)\s+fps\)", line)
        if m:
            fps = float(m.group(1))
            if last:
                formats[last].add(fps)
    return formats

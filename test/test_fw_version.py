import subprocess
import pytest
import re

def validate_video_device(device):
    """Validate device is a safe video device path."""
    # Accept only video device numbers (0-99)
    if not re.match(r'^[0-9]{1,2}$', str(device)):
        raise ValueError(f"Invalid device: {device}. Expected video device number (0-99)")
    return str(device)

@pytest.mark.d457
@pytest.mark.parametrize("device", {'0'})
def test_fw_version(device):
    try:
        device = validate_video_device(device)
        key = "fw_version"
        result = subprocess.check_call(["v4l2-ctl", "-d", device, "-C", key])
        assert result == 0

        std_output = subprocess.check_output(["v4l2-ctl", "-d", device, "-C", key])
        key += ": "
        assert key in std_output.decode(), "Couldn't fetch FW version"

        # Remove the 'fw version: ' string from std output
        fw_version = int(std_output.decode().replace(key, ""))

        fw_version_str = str(fw_version>>24 & 0xFF) + "." + str(fw_version>>16 & 0xFF) + "." + str(fw_version>>8 & 0xFF)  + "." + str(fw_version & 0xFF)
        print ("fw_version:", fw_version_str)

        # Check if the FW version matching with 5.x.x.x
        assert fw_version == (fw_version & 0x05FFFFFF), "Expected FW version is 5.x.x.x, but received {}".format(fw_version_str)

        # Get DFU device name
        dfu_device = subprocess.check_output(["ls", "/sys/class/d4xx-class/"]).decode()
        assert "d4xx-dfu-" in dfu_device, "D4xx DFU device not found"

        # Validate DFU device name to prevent path traversal
        dfu_device_name = dfu_device.strip()
        if not re.match(r'^d4xx-dfu-[0-9]+$', dfu_device_name):
            raise ValueError(f"Invalid DFU device name: {dfu_device_name}")

        # Get FW version from DFU device info
        dfu_device_path = f"/dev/{dfu_device_name}"
        dfu_device_info = subprocess.check_output(["cat", dfu_device_path]).decode()

        # Check whether the DFU info also has same FW version
        assert fw_version_str in dfu_device_info, "FW versions read through v4l2-ctl utility and DFU device info doesn't match"

    except Exception as e:
        assert False, "Exception caught during test: {}".format(e)

import subprocess
import pytest

@pytest.mark.d457
@pytest.mark.parametrize("device", {'0'})
def test_fw_version(device):
    try:
        result = subprocess.check_call(["v4l2-ctl", "-d"+device, "-C", "fw_version"])
        assert result == 0

        std_output = subprocess.check_output(["v4l2-ctl", "-d"+device, "-C", "fw_version"])
        assert "fw version: " in std_output, "Couldn't fetch FW version"

        # Remove the 'fw version: ' string from std output
        fw_version = int(std_output.replace("fw version: ", ""))

        fw_version_str = str(fw_version>>24 & 0xFF) + "." + str(fw_version>>16 & 0xFF) + "." + str(fw_version>>8 & 0xFF)  + "." + str(fw_version & 0xFF)
        print ("fw_version:", fw_version_str)

        # Check if the FW version matching with 5.x.x.x
        assert fw_version == (fw_version & 0x05FFFFFF), "Expected FW version is 5.x.x.x, but received {}".format(fw_version_str)

        # Get DFU device name
        dfu_device = subprocess.check_output(["ls", "/sys/class/d4xx-class/"])
        assert "d4xx-dfu-" in dfu_device, "D4xx DFU device not found"

        # Get FW version from DFU device info
        dfu_device_info = subprocess.check_output(["cat", "/dev/"+dfu_device.strip()])

        # Check whether the DFU info also has same FW version
        assert fw_version_str in dfu_device_info, "FW versions read through v4l2-ctl utility and DFU device info doesn't match"

    except Exception as e:
        assert False, e

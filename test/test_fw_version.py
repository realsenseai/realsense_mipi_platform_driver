import subprocess
import pytest

@pytest.mark.parametrize("device", {'-d0'})
@pytest.mark.parametrize("options", {'-C'})
def test_fw_version(device, options):
    rc = subprocess.check_call(["v4l2-ctl", device, options ,"fw_version"])
    assert rc == 0

    std_output = subprocess.check_output(["v4l2-ctl", device, options ,"fw_version"])
    if "fw version: " in std_output:
        # Remove the 'fw version: ' string from std output
        fw_version = int(std_output.replace("fw version: ", ""))
        print ("fw_version: ", hex(fw_version))
        # Check if the FW version matching with 5.x.x.x
        assert fw_version == (fw_version & 0x05FFFFFF)
    else:
        assert false

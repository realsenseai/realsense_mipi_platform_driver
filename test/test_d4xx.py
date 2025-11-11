#!/usr/bin/env python3
"""
D4XX Module Unit Test Suite

This test suite provides comprehensive testing for the Intel RealSense D4XX camera driver module.
It verifies driver functionality, device enumeration, stream capabilities, and control operations.

Requirements:
- pytest
- pytest-html (for HTML reports)
- pytest-cov (for coverage reports)
- v4l2-ctl utility

Run with:
    pytest test_d4xx.py --html=report.html --self-contained-html --cov=d4xx
"""

import os
import re
import time
import subprocess
import pytest
import logging
from pathlib import Path
from typing import Dict, List, Optional, Union, Tuple

# Configure logging
logging.basicConfig(level=logging.INFO,
                   format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Constants
DEVICE_PATH_PREFIX = "/dev/video"
DFU_CLASS_PATH = "/sys/class/d4xx-class/"
MODULE_NAME = "d4xx"
DEFAULT_TIMEOUT = 10  # seconds

# Streams and formats supported by D4XX
STREAM_TYPES = ["depth", "rgb", "ir", "imu"]
SUPPORTED_FORMATS = {
    "depth": ["Z16", "Y8"],
    "rgb": ["RGB8", "YUYV"],
    "ir": ["Y8", "Y16"],
    "imu": ["IMU"]
}

class D4xxTestException(Exception):
    """Custom exception for D4XX test failures"""
    pass

class D4xxTester:
    """Helper class for testing D4XX driver functionality"""
    
    def __init__(self, depth_device_id: str = "0", rgb_device_id: str = "2"):
        """
        Initialize the tester with specific devices
        
        Args:
            depth_device_id: Depth Device ID (default: "0" for /dev/video0)
            rgb_device_id: RGB Device ID (default: "2" for /dev/video2)
        """
        self.depth_device_id = depth_device_id
        self.rgb_device_id = rgb_device_id
        self.depth_device_path = f"{DEVICE_PATH_PREFIX}{depth_device_id}"
        self.rgb_device_path = f"{DEVICE_PATH_PREFIX}{rgb_device_id}"
          # Verify devices exist
        if not os.path.exists(self.depth_device_path):
            raise D4xxTestException(f"Depth device {self.depth_device_path} does not exist")
            
        if not os.path.exists(self.rgb_device_path):
            raise D4xxTestException(f"RGB device {self.rgb_device_path} does not exist")
            
        logger.info(f"Initialized tester for depth device: {self.depth_device_path} and RGB device: {self.rgb_device_path}")
    
    def is_device_presense(self) -> bool:
        if not os.path.exists(self.depth_device_path):
            logger.error(f"Depth device {self.depth_device_path} does not exist")
            return False
            
        if not os.path.exists(self.rgb_device_path):
            logger.error(f"RGB device {self.depth_device_path} does not exist")
            return False
        return True

    def is_module_loaded(self) -> bool:
        """Check if d4xx module is loaded"""
        try:
            output = subprocess.check_output(["lsmod"], text=True)
            return MODULE_NAME in output
        except subprocess.SubprocessError as e:
            logger.error(f"Error checking module status: {e}")
            return False
        
    def get_device_info(self, device_id=None) -> Dict:
        """
        Get device information using v4l2-ctl
        
        Args:
            device_id: Device ID to query (if None, defaults to depth_device_id)
        """
        try:
            device_id = device_id if device_id is not None else self.depth_device_id
            output = subprocess.check_output(["v4l2-ctl", f"-d{device_id}", "--all"], 
                                            universal_newlines=True)
            
            info = {
                "driver": re.search(r"Driver name\s+:\s+(.+)", output).group(1) if re.search(r"Driver name\s+:\s+(.+)", output) else None,
                "card": re.search(r"Card type\s+:\s+(.+)", output).group(1) if re.search(r"Card type\s+:\s+(.+)", output) else None,
                "bus_info": re.search(r"Bus info\s+:\s+(.+)", output).group(1) if re.search(r"Bus info\s+:\s+(.+)", output) else None,
                "capabilities": re.findall(r"\[(.+?)\]", output) if re.findall(r"\[(.+?)\]", output) else []
            }
            
            return info
        except subprocess.SubprocessError as e:
            logger.error(f"Error getting device info: {e}")
            raise D4xxTestException(f"Failed to get device info: {e}")
            
    def get_fw_version(self, device_id=None) -> str:
        """
        Get firmware version
        
        Args:
            device_id: Device ID to query (if None, defaults to depth_device_id)
        """
        try:
            device_id = device_id if device_id is not None else self.depth_device_id
            output = subprocess.check_output(["v4l2-ctl", f"-d{device_id}", "-C", "fw_version"],
                                           universal_newlines=True)
            
            fw_version = int(output.replace("fw version: ", "").strip())
            fw_version_str = f"{fw_version>>24 & 0xFF}.{fw_version>>16 & 0xFF}.{fw_version>>8 & 0xFF}.{fw_version & 0xFF}"
            print(f"Firmware version: {fw_version_str}")
            return fw_version_str
        except subprocess.SubprocessError as e:
            logger.error(f"Error getting FW version: {e}")
            raise D4xxTestException(f"Failed to get FW version: {e}")
    def get_supported_formats(self, device_id: str = None) -> Dict:
        """
        Get supported formats for the device
        
        Args: device_id: Device ID to query (if None, defaults to depth_device_id)
        """
        try:
            device_id = device_id if device_id is not None else self.depth_device_id
            device_path = f"{DEVICE_PATH_PREFIX}{device_id}"
            
            logger.info(f"Querying formats from device {device_path}")
            output = subprocess.check_output(
                ["v4l2-ctl", f"-d{device_id}", "--list-formats-ext"],
                universal_newlines=True
            )
            
            # Initialize data structures
            formats = {}
            current_format = None
            current_size = None
            
            # Parse output line by line
            for line in output.splitlines():
                line = line.strip()
                
                # Format detection - matches '[0]: 'Z16 ' (16-bit Depth)'
                if "[" in line and "]:" in line and "'" in line:
                    current_format = self._parse_format_line(line, formats)
                    current_size = None
                
                # Size detection - matches 'Size: Discrete 1280x720'
                elif current_format and "Size: Discrete" in line:
                    current_size = self._parse_size_line(line, formats, current_format)
                
                # FPS detection - matches 'Interval: Discrete 0.033s (30.000 fps)'
                elif current_format and current_size and "Interval: Discrete" in line:
                    self._parse_fps_line(line, current_size)
            
            # Check if any formats were found
            if not formats:
                logger.warning("No formats were found in the device output")                
                logger.debug(f"Raw output:\n{output}")
            return formats
            
        except subprocess.SubprocessError as e:
            logger.error(f"Error getting supported formats: {e}")
            raise D4xxTestException(f"Failed to get supported formats: {e}")
        except Exception as e:
            logger.error(f"Unexpected error parsing formats: {e}")
            logger.debug(f"Raw output:\n{output if 'output' in locals() else 'No output available'}")
            raise D4xxTestException(f"Failed to parse format data: {e}")
        
    def get_controls(self, device_id=None) -> List[Dict]:
        """
        Get available controls for the device
        
        Args:
            device_id: Device ID to query (if None, defaults to depth_device_id)
        """
        try:
            device_id = device_id if device_id is not None else self.depth_device_id
            output = subprocess.check_output(["v4l2-ctl", f"-d{device_id}", "--list-ctrls"],
                                           universal_newlines=True)
            
            controls = []
            for line in output.splitlines():
                parts = line.split()
                if len(parts) >= 3:
                    control_name = parts[0]
                    # Extract control information
                    control_info = {
                        "name": control_name,
                        "value": None,
                        "default": None,
                        "min": None,
                        "max": None
                    }
                    for part in parts[1:]:
                        if '=' in part:
                            key, value = part.split('=', 1)
                            control_info[key] = value
                    
                    controls.append(control_info)
            
            return controls
        except subprocess.SubprocessError as e:
            logger.error(f"Error getting controls: {e}")
            raise D4xxTestException(f"Failed to get controls: {e}")
        
    def test_control_access(self, control_name: str, device_id: str = None) -> Tuple[bool, str]:
        """Test if a specific v4l2 control is accessible and can be read
        
        Args:
            control_name: Name of the control to test (e.g., 'laser_power', 'fw_version')
            device_id: Device ID to query (if None, defaults to depth_device_id)
            
        Returns:
            Tuple[bool, str]: (success, output/error message)
        """
        try:
            device_id = device_id if device_id is not None else self.depth_device_id
            device_path = f"{DEVICE_PATH_PREFIX}{device_id}"
            
            logger.info(f"Testing access to control '{control_name}' on device {device_path}")
            
            # Try to read the control value
            output = subprocess.check_output(
                ["v4l2-ctl", f"-d{device_id}", "-C", control_name],
                universal_newlines=True,
                stderr=subprocess.STDOUT
            )
            
            logger.info(f"Successfully read control '{control_name}': {output.strip()}")
            return True, output.strip()
            
        except subprocess.SubprocessError as e:
            error_msg = f"Failed to access control '{control_name}': {e}"
            logger.error(error_msg)
            return False, str(e)


    def test_stream_start_stop(self, format_name: str, width: int, height: int, fps: int = 30, duration: int = 5, device_id: str = None) -> Tuple[bool, float]:
        """Test starting and stopping a stream with specified format
        
        Args:
            format_name: The pixel format name (e.g., 'Z16 ', 'RGB8')
            width: The stream width in pixels
            height: The stream height in pixels
            fps: Frames per second to set
            duration: How many seconds the stream should run
            device_id: Specific device ID to use (if None, depth_device_id will be used)
            
        Returns:
            Tuple[bool, float]: (success status, measured FPS)
        """
        try:
            # Use provided device_id or default to depth_device_id
            device_id = device_id if device_id is not None else self.depth_device_id
            device_path = f"{DEVICE_PATH_PREFIX}{device_id}"
            
            # Format name needs to be exactly as expected by v4l2-ctl (including spaces if needed)
            logger.debug(f"Testing stream with format '{format_name}' at {width}x{height}, {fps} FPS for {duration} seconds on device {device_path}")
            
            # Calculate stream count based on fps and duration
            stream_count = fps * duration
            
            # Set the format
            set_format_process = subprocess.Popen(
                ["v4l2-ctl", f"-d{device_id}", f"--set-fmt-video=width={width},height={height},pixelformat={format_name}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            set_format_process.communicate()
            
            if set_format_process.returncode != 0:
                logger.error(f"Failed to set video format on device {device_path}")
                return False, 0.0
                
            # Set the framerate
            set_fps_process = subprocess.Popen(
                ["v4l2-ctl", f"-d{device_id}", f"--set-parm={fps}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            _, _ = set_fps_process.communicate()
            
            if set_fps_process.returncode != 0:
                logger.error(f"Failed to set framerate to {fps} on device {device_path}")
                return False, 0.0
            
            # Skip the first few frames for stability
            skip_frames = 10  
            
            # Stream with verbose mode to get FPS measurements from v4l2-ctl itself
            cmd = [
                "v4l2-ctl", 
                f"-d{device_id}", 
                "--stream-mmap", 
                f"--stream-count={stream_count}",
                "--stream-poll",
                "--verbose",
                f"--stream-skip={skip_frames}",
                "--stream-to=/dev/null"
            ]
            
            logger.debug(f"Running stream command: {' '.join(cmd)}")
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True
            )
            
            # Wait for process to complete (add 2 seconds to the timeout for buffer)
            stdout, stderr = process.communicate(timeout=duration + 5)
            
            # Check if the process succeeded
            if process.returncode != 0:
                logger.error(f"Stream test failed: {stderr}")
                return False, 0.0
            
            # Parse v4l2-ctl output to extract measured FPS
            fps_values = []
            for line in stdout.splitlines() + stderr.splitlines():
                if "fps:" in line:
                    match = re.search(r"fps:\s+(\d+\.\d+)", line)
                    if match:
                        fps_values.append(float(match.group(1)))
            
            # Calculate average FPS from the collected values
            if fps_values:
                # Remove outliers (first few frames might have inconsistent timing)
                if len(fps_values) > 5:
                    fps_values = fps_values[3:]
                
                measured_fps = sum(fps_values) / len(fps_values)
                logger.debug(f"Stream completed: Target FPS: {fps}, Measured FPS: {measured_fps:.2f} (from {len(fps_values)} samples)")
            else:
                logger.warning("No FPS measurements found in v4l2-ctl output")
                measured_fps = 0
                
            # Verify the measured FPS is within 5% of the requested FPS
            fps_tolerance = 0.05  # 5%
            fps_min = fps * (1 - fps_tolerance)
            fps_max = fps * (1 + fps_tolerance)
            
            if not fps_values or not (fps_min <= measured_fps <= fps_max):
                if fps_values:
                    logger.error(f"FPS mismatch: Target FPS: {fps}, Measured FPS: {measured_fps:.2f}, "
                               f"Expected range: {fps_min:.2f} - {fps_max:.2f}")
                else:
                    logger.error(f"Could not measure FPS for target rate {fps} FPS")
                    
                # We return False here to indicate the test failed due to FPS mismatch
                return False, measured_fps if fps_values else 0.0
                
            return True, measured_fps
        except subprocess.SubprocessError as e:
            logger.error(f"Error in stream test: {e}")
            # Include information about the target FPS in the error for troubleshooting
            logger.error(f"Target FPS was {fps} for format {format_name} at {width}x{height}")
            return False, 0.0
        
    def test_first_frame_delay(self, format_name: str, width: int, height: int, fps: int = 30, device_id: str = None) -> Tuple[bool, float]:
        """Measure the delay to get the first frame with specified format
        
        Args:
            format_name: The pixel format name (e.g., 'Z16 ', 'RGB8')
            width: The stream width in pixels
            height: The stream height in pixels
            fps: Frames per second to set
            device_id: Specific device ID to use (if None, depth_device_id will be used)
            
        Returns:
            Tuple[bool, float]: (success, delay in seconds)
        """
        try:
            # Use provided device_id or default to depth_device_id
            device_id = device_id if device_id is not None else self.depth_device_id
            device_path = f"{DEVICE_PATH_PREFIX}{device_id}"
            
            logger.debug(f"Testing first frame delay with format '{format_name}' at {width}x{height}, {fps} FPS on device {device_path}")
            
            # Set the format
            set_format_process = subprocess.Popen(
                ["v4l2-ctl", f"-d{device_id}", f"--set-fmt-video=width={width},height={height},pixelformat={format_name}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            set_format_process.communicate()
            
            if set_format_process.returncode != 0:
                logger.error(f"Failed to set video format on device {device_path}")
                return False, 0.0
                
            # Set the framerate
            set_fps_process = subprocess.Popen(
                ["v4l2-ctl", f"-d{device_id}", f"--set-parm={fps}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            _, _ = set_fps_process.communicate()
            
            if set_fps_process.returncode != 0:
                logger.error(f"Failed to set framerate to {fps} on device {device_path}")
                return False, 0.0
            
            # Stream with verbose mode, but only 1 frame
            cmd = [
                "v4l2-ctl", 
                f"-d{device_id}", 
                "--stream-mmap", 
                "--stream-count=1",
                "--stream-poll",
                "--verbose",
                "--stream-to=/dev/null"
            ]
            
            logger.debug(f"Running stream command for first frame: {' '.join(cmd)}")
            
            # Record the start time
            start_time = time.time()
            
            # Start the process
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True
            )
            
            # Wait for process to complete (add a reasonable timeout)
            stdout, stderr = process.communicate(timeout=DEFAULT_TIMEOUT)
            
            # Calculate the delay
            delay = time.time() - start_time
            
            # Check if the process succeeded
            if process.returncode != 0:
                logger.error(f"First frame test failed: {stderr}")
                return False, delay
            
            logger.debug(f"First frame received in {delay:.3f} seconds")
            return True, delay
            
        except subprocess.SubprocessError as e:
            logger.error(f"Error in first frame test: {e}")
            return False, 0.0

    def get_dfu_device_info(self) -> Optional[str]:
        """Get DFU device information"""
        try:
            # List d4xx-class devices
            dfu_device = subprocess.check_output(["ls", DFU_CLASS_PATH], 
                                               universal_newlines=True).strip()
            
            if not dfu_device or "d4xx-dfu-" not in dfu_device:
                logger.warning("D4XX DFU device not found")
                return None
                  # Read DFU device info
            dfu_info = subprocess.check_output(["cat", f"/dev/{dfu_device}"], 
                                             universal_newlines=True)
            
            return dfu_info
        except subprocess.SubprocessError as e:
            logger.error(f"Error getting DFU device info: {e}")
            return None
            
    def _parse_format_line(self, line, formats):
        """Parse a line containing format information"""
        format_match = re.search(r"\[\d+\]:\s+'([^']+)'\s+\((.+?)\)", line)
        if format_match:
            # Do NOT strip the format code - preserve exact whitespace as it appears
            # This is important for formats like 'Z16 ' where the space is significant
            format_code = format_match.group(1)
            format_desc = format_match.group(2).strip()
            formats[format_code] = {
                "description": format_desc,
                "sizes": []
            }
            logger.info(f"Found format: '{format_code}' ({format_desc})")
            return format_code
        return None
    
    def _parse_size_line(self, line, formats, current_format):
        """Parse a line containing resolution size information"""
        size_match = re.search(r"Size: Discrete (\d+)x(\d+)", line)
        if size_match and current_format:
            width, height = int(size_match.group(1)), int(size_match.group(2))
            size_info = {"width": width, "height": height, "intervals": []}
            formats[current_format]["sizes"].append(size_info)
            logger.info(f"  Resolution: {width}x{height}")
            return size_info
        return None
    
    def _parse_fps_line(self, line, current_size):
        """Parse a line containing FPS information"""
        if current_size:
            fps_match = re.search(r"Interval: Discrete .+?\((\d+\.\d+) fps\)", line)
            if fps_match:
                fps = float(fps_match.group(1))
                current_size["intervals"].append({"fps": fps})
                logger.debug(f"    FPS: {fps}")


# Fixtures
@pytest.fixture(scope="session")
def d4xx_device():
    """Create and return a D4xxTester instance"""
    # Define device IDs for depth and RGB
    depth_device_id = "0"  # Default to video0 for depth
    rgb_device_id = "2"    # Default to video2 for RGB
    
    # Create tester instance
    try:
        tester = D4xxTester(depth_device_id=depth_device_id, rgb_device_id=rgb_device_id)
        yield tester
    except D4xxTestException as e:
        pytest.skip(f"D4xx device not available: {e}")


# Test cases
@pytest.mark.driver
def test_module_loaded(d4xx_device):
    """Test if d4xx module is loaded"""
    assert d4xx_device.is_module_loaded(), "D4XX module is not loaded"


@pytest.mark.driver
def test_device_presence(d4xx_device):
    """Test if D4XX device is present"""
    assert d4xx_device.is_device_presense()


@pytest.mark.fw
def test_fw_version(d4xx_device):
    """Test firmware version is valid and follows expected format"""
    fw_version = d4xx_device.get_fw_version()
    
    # Check version format (major.minor.patch.build)
    match = re.match(r"^\d+\.\d+\.\d+\.\d+$", fw_version)
    assert match, f"Invalid firmware version format: {fw_version}"
    
    # Check major version (expected to be 5.x.x.x as per test_fw_version.py)
    major_version = int(fw_version.split('.')[0])
    assert major_version == 5, f"Expected major version 5, but got {major_version}"


@pytest.mark.fw
def test_dfu_device(d4xx_device):
    """Test if DFU device is available and has matching firmware version"""
    dfu_info = d4xx_device.get_dfu_device_info()
    assert dfu_info is not None, "DFU device info not available"
    
    fw_version = d4xx_device.get_fw_version()
    assert fw_version in dfu_info, "Firmware version mismatch between driver and DFU device"



@pytest.mark.controls
def test_basic_controls(d4xx_device):
    """Test basic control accessibility"""
    # List of essential controls that should be present
    essential_controls = [
        "fw_version",
        "laser_power_on_off",
        "manual_laser_power",
        "ae_setpoint_get",
        "ae_setpoint_set",
        "auto_exposure",
        "height_align"
    ]
    failed_controls = []
    for control in essential_controls:
        success, output = d4xx_device.test_control_access(control)
        if not success:
            failed_controls.append((control, output))
    
    # Log control test results
    if failed_controls:
        for control, error in failed_controls:
            logger.error(f"Control '{control}' failed: {error}")
        # Fail the test if any controls are not accessible
        pytest.fail(f"The following controls are not accessible: {', '.join(c for c, _ in failed_controls)}")
    else:
        logger.info("All essential controls are accessible")


@pytest.mark.controls
def test_laser_power_control(d4xx_device):
    """Test laser power control functionality"""
    success, output = d4xx_device.test_control_access("laser_power_on_off")
    assert success, "Cannot access laser_power control"
    
    # Extract current value
    try:
        current_value = int(output.split(": ")[-1].strip())
        
        # Test setting to 0 (off)
        subprocess.check_call(["v4l2-ctl", f"-d{d4xx_device.depth_device_id}", "-c", "laser_power_on_off=0"])
        
        # Verify the value was set
        output = subprocess.check_output(["v4l2-ctl", f"-d{d4xx_device.depth_device_id}", "-C", "laser_power_on_off"],
                                       universal_newlines=True)
        assert "laser_power_on_off: 0" in output, "Failed to set laser_power_on_off to 0"
        
        # Reset to original value
        subprocess.check_call(["v4l2-ctl", f"-d{d4xx_device.depth_device_id}", 
                             "-c", f"laser_power_on_off={current_value}"])
    except (ValueError, subprocess.SubprocessError) as e:
        pytest.fail(f"Error testing laser power control: {e}")


@pytest.mark.controls
def test_depth_control_access(d4xx_device):
    """Test access to all available depth controls and log their values to HTML report"""
    
    # Get all available controls from the depth device
    try:
        controls = d4xx_device.get_controls(device_id=d4xx_device.depth_device_id)
        logger.info(f"Found {len(controls)} controls on depth device")
    except D4xxTestException as e:
        pytest.fail(f"Failed to get controls list: {e}")
    
    if not controls:
        pytest.skip("No controls found on depth device")
    
    # Track control access results for summary
    accessible_controls = []
    failed_controls = []
    control_results = []
    
    logger.info("=" * 80)
    logger.info("DEPTH DEVICE CONTROL ACCESS TEST RESULTS")
    logger.info("=" * 80)
    
    # Test access to each control
    for control_info in controls:
        control_name = control_info.get("name", "unknown")
        
        try:
            # Use v4l2-ctl to read the control value
            output = subprocess.check_output(
                ["v4l2-ctl", f"-d{d4xx_device.depth_device_id}", "-C", control_name],
                universal_newlines=True,
                stderr=subprocess.STDOUT
            )
            
            # Parse the control value from output
            control_value = "N/A"
            if ":" in output:
                control_value = output.split(":", 1)[1].strip()
            
            # Log success
            logger.info(f"✓ CONTROL: {control_name[:25]} | VALUE: {control_value[:15]} | STATUS: ACCESSIBLE")
            
            accessible_controls.append(control_name)
            control_results.append({
                "name": control_name,
                "value": control_value,
                "accessible": True,
                "error": None
            })
            
        except subprocess.CalledProcessError as e:
            # Control access failed
            error_msg = str(e.output) if hasattr(e, 'output') and e.output else str(e)
            logger.warning(f"✗ CONTROL: {control_name:<25} | VALUE: {'N/A':<15} | STATUS: FAILED - {error_msg}")
            
            failed_controls.append((control_name, error_msg))
            control_results.append({
                "name": control_name,
                "value": "N/A",
                "accessible": False,
                "error": error_msg
            })
            
        except Exception as e:
            # Unexpected error
            error_msg = f"Unexpected error: {str(e)}"
            logger.error(f"✗ CONTROL: {control_name:<25} | VALUE: {'N/A':<15} | STATUS: ERROR - {error_msg}")
            
            failed_controls.append((control_name, error_msg))
            control_results.append({
                "name": control_name,
                "value": "N/A",
                "accessible": False,
                "error": error_msg
            })
    
    # Log summary statistics
    logger.info("=" * 80)
    logger.info("CONTROL ACCESS SUMMARY")
    logger.info("=" * 80)
    logger.info(f"Total controls found: {len(controls)}")
    logger.info(f"Successfully accessible: {len(accessible_controls)}")
    logger.info(f"Failed to access: {len(failed_controls)}")
    logger.info(f"Success rate: {(len(accessible_controls) / len(controls) * 100):.1f}%")
    
    # Log accessible controls list
    if accessible_controls:
        logger.info("ACCESSIBLE CONTROLS:")
        for control in accessible_controls:
            logger.info(f"  ✓ {control}")
    
    # Log failed controls with reasons
    if failed_controls:
        logger.info("FAILED CONTROLS:")
        for control, error in failed_controls:
            logger.info(f"  ✗ {control}: {error}")
    
    logger.info("=" * 80)
    
    # Ensure at least some controls are accessible (basic sanity check)
    if not accessible_controls:
        pytest.fail("No controls are accessible on the depth device")
    
    # Log detailed results for HTML report
    logger.info("DETAILED CONTROL ACCESS RESULTS:")
    for result in control_results:
        status = "SUCCESS" if result["accessible"] else "FAILED"
        error_info = f" ({result['error']})" if result["error"] else ""
        logger.info(f"Control '{result['name']}': {status} - Value: {result['value']}{error_info}")
    


@pytest.mark.controls
def test_rgb_control_access(d4xx_device):
    """Test access to all available RGB controls and log their values to HTML report"""
    
    # Get all available controls from the RGB device
    try:
        controls = d4xx_device.get_controls(device_id=d4xx_device.rgb_device_id)
        logger.info(f"Found {len(controls)} controls on RGB device")
    except D4xxTestException as e:
        pytest.fail(f"Failed to get RGB controls list: {e}")
    
    if not controls:
        pytest.skip("No controls found on RGB device")
    
    # Track control access results for summary
    accessible_controls = []
    failed_controls = []
    control_results = []
    
    logger.info("=" * 80)
    logger.info("RGB DEVICE CONTROL ACCESS TEST RESULTS")
    logger.info("=" * 80)
    
    # Test access to each control
    for control_info in controls:
        control_name = control_info.get("name", "unknown")
        
        try:
            # Use v4l2-ctl to read the control value
            output = subprocess.check_output(
                ["v4l2-ctl", f"-d{d4xx_device.rgb_device_id}", "-C", control_name],
                universal_newlines=True,
                stderr=subprocess.STDOUT
            )
            
            # Parse the control value from output
            control_value = "N/A"
            if ":" in output:
                control_value = output.split(":", 1)[1].strip()
            
            # Log success
            logger.info(f"✓ CONTROL: {control_name:<25.25]} | VALUE: {control_value:<15.15} | STATUS: ACCESSIBLE")
            
            accessible_controls.append(control_name)
            control_results.append({
                "name": control_name,
                "value": control_value,
                "accessible": True,
                "error": None
            })
            
        except subprocess.CalledProcessError as e:
            # Control access failed
            error_msg = str(e.output) if hasattr(e, 'output') and e.output else str(e)
            logger.warning(f"✗ CONTROL: {control_name:<25.25} | VALUE: {'N/A':<15.15} | STATUS: FAILED - {error_msg[:30]}")
            
            failed_controls.append((control_name, error_msg))
            control_results.append({
                "name": control_name,
                "value": "N/A",
                "accessible": False,
                "error": error_msg
            })
            
        except Exception as e:
            # Unexpected error
            error_msg = f"Unexpected error: {str(e)}"
            logger.error(f"✗ CONTROL: {control_name:<25.25} | VALUE: {'N/A':<15.15} | STATUS: ERROR - {error_msg[:30]}")
            
            failed_controls.append((control_name, error_msg))
            control_results.append({
                "name": control_name,
                "value": "N/A",
                "accessible": False,
                "error": error_msg
            })
    
    # Ensure at least some controls are accessible (basic sanity check)
    if not accessible_controls:
        pytest.fail("No controls are accessible on the RGB device")
    


@pytest.mark.formats
def test_supported_formats(d4xx_device):
    """Test if device supports expected formats"""
    formats = d4xx_device.get_supported_formats()
    
    # Check if at least one format is supported
    assert formats, "No formats supported by the device"
    
    # Log available formats
    for fmt, sizes in formats.items():
        logger.info(f"Format: {fmt}, Sizes: {sizes}")


@pytest.mark.stream
def test_depth_stream(d4xx_device):
    """Test depth streaming with Z16 format, all supported resolutions and fps"""
    # Get supported formats from the depth device
    formats = d4xx_device.get_supported_formats(device_id=d4xx_device.depth_device_id)
    
    # Check if formats were retrieved successfully
    assert formats, "No formats supported by the device"
    
    # The specific depth format with space included ('Z16 ')
    depth_format = 'Z16 '
    
    # Keep track of test results
    results = []
    
    # Check if our specific format is available
    if depth_format in formats:
        logger.info(f"Testing depth format: {depth_format}")
          # For each resolution
        for size_info in formats[depth_format]["sizes"]:
            width = size_info["width"]
            height = size_info["height"]
            
            # Get all supported FPS values
            fps_values = [30]  # Default if no FPS data available
            if "intervals" in size_info and size_info["intervals"]:
                fps_list = [interval["fps"] for interval in size_info["intervals"] if "fps" in interval]
                if fps_list:
                    fps_values = fps_list  # Use all available FPS values
            
            # Test stream for this resolution with each supported FPS on depth device
            for fps in fps_values:
                logger.info(f"Testing depth format {depth_format} at {width}x{height} with {fps} FPS on depth device")
                result, measured_fps = d4xx_device.test_stream_start_stop(depth_format, width, height, fps=int(fps), device_id=d4xx_device.depth_device_id)
                # Store the result
                result_info = {
                    "format": depth_format,
                    "resolution": f"{width}x{height}",
                    "fps": fps,
                    "measured_fps": measured_fps,
                    "result": result
                }
                results.append(result_info)
                
                # Log the result
                if result:
                    logger.info(f"✓ Depth stream with {depth_format} at {width}x{height}, target {fps} FPS, measured {measured_fps:.2f} FPS succeeded")
                else:
                    logger.warning(f"✗ Depth stream with {depth_format} at {width}x{height}, target {fps} FPS, measured {measured_fps:.2f} FPS failed")
    else:
        logger.warning(f"Depth format '{depth_format}' not found on the device")
        pytest.skip(f"Depth format '{depth_format}' not found on the device")
        return []
    
    # Check if any tests were successful
    if not any(r["result"] for r in results):
        pytest.fail("All depth streaming tests failed")
      # Log a summary
    logger.info("Depth streaming test results summary:")
    for r in results:
        status = "Passed" if r["result"] else "Failed"
        measured_fps_str = f", measured {r['measured_fps']:.2f} FPS" if "measured_fps" in r else ""
        logger.info(f"  {r['format']} at {r['resolution']}, target {r['fps']} FPS{measured_fps_str}: {status}")
    
    # Return successful formats/resolutions for reporting
    return [f"{r['format']} at {r['resolution']}, target {r['fps']} FPS, measured {r['measured_fps']:.2f} FPS" for r in results if r["result"]]


@pytest.mark.stream
def test_rgb_stream(d4xx_device):
    """Test RGB streaming with YUYV format, all supported resolutions and fps"""
    # Get supported formats from the RGB device
    formats = d4xx_device.get_supported_formats(device_id=d4xx_device.rgb_device_id)
    
    # Check if formats were retrieved successfully
    assert formats, "No formats supported by the device"
    
    # The specific RGB format we want to test
    rgb_format = 'YUYV'
    
    # Keep track of test results
    results = []
    
    # Check if our specific format is available
    if rgb_format in formats:
        logger.info(f"Testing RGB format: {rgb_format}")
        
        # For each resolution
        for size_info in formats[rgb_format]["sizes"]:
            width = size_info["width"]
            height = size_info["height"]
            
            # Get the highest supported FPS
            fps = 30  # Default if no FPS data available
            if "intervals" in size_info and size_info["intervals"]:
                fps_values = [interval["fps"] for interval in size_info["intervals"] if "fps" in interval]
                if fps_values:
                    fps = max(fps_values)  # Use the highest available FPS
                      # Test stream for this resolution with appropriate FPS on RGB device
            logger.info(f"Testing RGB format {rgb_format} at {width}x{height} with {fps} FPS on RGB device")
            result, measured_fps = d4xx_device.test_stream_start_stop(rgb_format, width, height, fps=int(fps), device_id=d4xx_device.rgb_device_id)
            
            # Store the result
            result_info = {
                "format": rgb_format,
                "resolution": f"{width}x{height}",
                "fps": fps,
                "measured_fps": measured_fps,
                "result": result
            }
            results.append(result_info)
            
            # Log the result
            if result:
                logger.info(f"✓ RGB stream with {rgb_format} at {width}x{height}, target {fps} FPS, measured {measured_fps:.2f} FPS succeeded")
            else:
                logger.warning(f"✗ RGB stream with {rgb_format} at {width}x{height}, target {fps} FPS, measured {measured_fps:.2f} FPS failed")
    else:
        logger.warning(f"RGB format '{rgb_format}' not found on the device")
        pytest.skip(f"RGB format '{rgb_format}' not found on the device")
        return []
      # Check if any tests were successful
    if not any(r["result"] for r in results):
        pytest.fail("All RGB streaming tests failed")
        
    # Log a summary
    logger.info("RGB streaming test results summary:")
    for r in results:
        status = "Passed" if r["result"] else "Failed"
        measured_fps_str = f", measured {r['measured_fps']:.2f} FPS" if "measured_fps" in r else ""
        logger.info(f"  {r['format']} at {r['resolution']}, target {r['fps']} FPS{measured_fps_str}: {status}")
      # Return successful formats/resolutions for reporting
    return [f"{r['format']} at {r['resolution']}, target {r['fps']} FPS, measured {r['measured_fps']:.2f} FPS" for r in results if r["result"]]

@pytest.mark.stream
def test_first_frame_delay(d4xx_device):
    """
    Test the time it takes to receive the first frame for all supported depth resolutions and FPS.
    
    Tests are considered successful if the first frame is received within 1.5 seconds.
    Similar to test_depth_stream, but measures first frame delay instead of sustained streaming.
    """
    # Get supported formats from the depth device
    formats = d4xx_device.get_supported_formats(device_id=d4xx_device.depth_device_id)
    
    # Check if formats were retrieved successfully
    assert formats, "No formats supported by the device"
    
    # The specific depth format with space included ('Z16 ')
    depth_format = 'Z16 '
    
    # Maximum acceptable delay in seconds
    max_acceptable_delay = 1.5
    
    # Keep track of test results
    results = []
    
    # Check if our specific format is available
    if depth_format in formats:
        logger.info(f"Testing first frame delay for format: {depth_format}")
        
        # For each resolution
        for size_info in formats[depth_format]["sizes"]:
            width = size_info["width"]
            height = size_info["height"]
            
            # Get all supported FPS values
            fps_values = [30]  # Default if no FPS data available
            if "intervals" in size_info and size_info["intervals"]:
                fps_list = [interval["fps"] for interval in size_info["intervals"] if "fps" in interval]
                if fps_list:
                    fps_values = fps_list  # Use all available FPS values
              # Test stream for this resolution with each supported FPS on depth device
            for fps in fps_values:
                # Test first frame delay for this resolution with appropriate FPS on depth device
                logger.info(f"Testing first frame delay with format {depth_format} at {width}x{height} with {fps} FPS on depth device")
                success, delay = d4xx_device.test_first_frame_delay(depth_format, width, height, fps=int(fps), device_id=d4xx_device.depth_device_id)
                # Store the result
                result_info = {
                    "format": depth_format,
                    "resolution": f"{width}x{height}",
                    "fps": fps,
                    "delay": delay,
                    "result": success and (delay <= max_acceptable_delay)
                }
                results.append(result_info)        
                # Log the result
                if success:
                    if delay <= max_acceptable_delay:
                        logger.info(f"✓ First frame delay: {delay:.3f}s for {depth_format} at {width}x{height}, {fps} FPS - Within limit")
                    else:
                        logger.warning(f"⚠ First frame delay: {delay:.3f}s for {depth_format} at {width}x{height}, {fps} FPS - Exceeds limit of {max_acceptable_delay}s")
                else:
                    logger.warning(f"✗ Failed to get first frame for {depth_format} at {width}x{height}, {fps} FPS")
    else:
        logger.warning(f"Depth format '{depth_format}' not found on the device")
        pytest.skip(f"Depth format '{depth_format}' not found on the device")
        return []
    
    # Check if any tests were successful
    if not any(r["result"] for r in results):
        pytest.fail("All first frame delay tests failed")
    
    # Log successful formats/resolutions for reporting in HTML
    successful_tests = [f"{r['format']} at {r['resolution']}, {r['fps']} FPS, delay: {r['delay']:.3f}s" for r in results if r["result"]]
    if successful_tests:
        logger.info("SUCCESSFUL FIRST FRAME DELAY TESTS:")
        for test in successful_tests:
            logger.info(f"  ✓ {test}")
    else:
        logger.warning("No first frame delay tests passed")
    
    # Return successful formats/resolutions for reporting
    return successful_tests


# Main function for standalone execution
if __name__ == "__main__":
    # This enables running this file directly (not through pytest)
    # It will execute tests and generate report
    import sys
    sys.exit(pytest.main(["-v", 
                          "--html=d4xx_test_report.html", 
                          "--self-contained-html",
                          "--cov=d4xx", 
                          __file__]))

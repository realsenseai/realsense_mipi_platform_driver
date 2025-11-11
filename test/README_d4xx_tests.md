# D4XX Module Test Suite

A comprehensive pytest-based test suite for the Intel RealSense D4XX camera driver module.

## Overview

This test suite verifies the functionality of the D4XX kernel driver by testing:
- Driver module presence
- Device enumeration
- Firmware version
- Control interfaces
- Streaming capabilities
- DFU device functionality

## Requirements

- Linux system with D4XX camera connected
- Python 3.6 or higher
- v4l2-ctl utility (from v4l-utils package)
- pytest and related packages (see requirements.txt)

## Installation

1. Install required system packages:
   ```bash
   sudo apt-get update
   sudo apt-get install -y v4l-utils
   ```

2. Install Python dependencies:
   ```bash
   pip install -r requirements.txt
   ```

## Running Tests

### Basic Usage

Run all tests and generate an HTML report:

```bash
python run_d4xx_tests.py --open-report
```

### Advanced Usage

Run specific test categories:

```bash
python run_d4xx_tests.py --markers driver,fw
```

Available test markers:
- `driver`: Basic driver functionality tests
- `fw`: Firmware-related tests
- `controls`: Control interface tests
- `formats`: Format support tests
- `stream`: Streaming functionality tests

Run tests in parallel:

```bash
python run_d4xx_tests.py --parallel
```

### Running with pytest directly

```bash
cd test
pytest test_d4xx.py -v --html=report.html --self-contained-html
```

## Test Report

The test report includes:
- Summary statistics
- Test results with pass/fail status
- Detailed error information for failed tests
- Code coverage metrics
- Environment information

Reports are saved in the `reports` directory by default.

## Customization

- Edit `test_d4xx.py` to add or modify tests
- Adjust report appearance by modifying `report_template.py`
- Configure test parameters in `run_d4xx_tests.py`

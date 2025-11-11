#!/usr/bin/env python3
"""
D4XX Test Runner

This utility script runs the D4XX module tests and generates a comprehensive HTML report.
"""

import os
import sys
import argparse
import subprocess
import datetime
import logging
import webbrowser
from pathlib import Path

# Configure logging
logging.basicConfig(level=logging.INFO,
                   format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Constants
DEFAULT_REPORT_DIR = "reports"
DEFAULT_TEST_FILE = "test_d4xx.py"
PYTEST_HTML_REQUIRED = "pytest-html"
PYTEST_COV_REQUIRED = "pytest-cov"
PYTEST_XDIST_REQUIRED = "pytest-xdist"  # For parallel testing

def check_prerequisites():
    """Check if all prerequisites are installed"""
    try:
        # Check pytest
        import pytest
        logger.info(f"Found pytest version: {pytest.__version__}")
        
        # Check pytest-html
        try:
            import pytest_html
            logger.info(f"Found pytest-html version: {pytest_html.__version__}")
        except ImportError:
            logger.error(f"Missing {PYTEST_HTML_REQUIRED}. Installing...")
            subprocess.check_call([sys.executable, "-m", "pip", "install", PYTEST_HTML_REQUIRED])
        
        # Check pytest-cov
        try:
            import pytest_cov
            logger.info(f"Found pytest-cov")
        except ImportError:
            logger.error(f"Missing {PYTEST_COV_REQUIRED}. Installing...")
            subprocess.check_call([sys.executable, "-m", "pip", "install", PYTEST_COV_REQUIRED])
            
        # Check pytest-xdist
        try:
            import xdist
            logger.info(f"Found pytest-xdist")
        except ImportError:
            logger.error(f"Missing {PYTEST_XDIST_REQUIRED}. Installing...")
            subprocess.check_call([sys.executable, "-m", "pip", "install", PYTEST_XDIST_REQUIRED])
            
        # Check v4l2-ctl
        try:
            subprocess.check_output(["which", "v4l2-ctl"])
            logger.info("Found v4l2-ctl utility")
        except subprocess.SubprocessError:
            logger.error("v4l2-ctl not found. Please install v4l-utils package")
            return False
            
        return True
    except ImportError:
        logger.error("pytest not found. Please install pytest: pip install pytest")
        return False
    except subprocess.SubprocessError as e:
        logger.error(f"Error installing dependencies: {e}")
        return False

def generate_report_filename():
    """Generate a timestamped report filename"""
    timestamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
    return f"d4xx_test_report_{timestamp}.html"

def run_tests(args):
    """Run the D4XX tests with pytest"""
    # Ensure report directory exists
    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    
    # Generate report filename
    report_file = report_dir / generate_report_filename()
    
    # Build pytest command
    cmd = [
        "pytest",
        "-v",
        "-s",  # Don't capture output, show print statements
        f"--html={report_file}",
        "--self-contained-html",
        "--cov=d4xx",
        f"--log-cli-level={args.log_level}",  # Capture logs in CLI with specified level
        "--log-cli-format=%(asctime)s [%(levelname)8s] %(name)s: %(message)s",
        f"--log-file-level={args.log_level}",  # Capture logs in file with specified level
        "--capture=no",  # Don't capture stdout/stderr, show everything
        "--tb=short"  # Shorter traceback format for cleaner reports
    ]
    
    # Add additional arguments
    if args.markers:
        for marker in args.markers.split(','):
            cmd.append(f"-m {marker}")
            
    # Add parallelization if selected
    if args.parallel:
        cmd.append("-n auto")
        
    # Add test file path
    test_path = Path(__file__).parent / args.test_file
    cmd.append(str(test_path))
    
    try:
        # Run the tests
        logger.info(f"Running command: {' '.join(cmd)}")
        logger.info(f"Test report will be saved to: {report_file}")
        
        # Run with real-time output
        process = subprocess.Popen(cmd, 
                                 stdout=subprocess.PIPE, 
                                 stderr=subprocess.STDOUT,
                                 universal_newlines=True,
                                 bufsize=1)
        
        # Print output in real-time and capture it
        output_lines = []
        while True:
            output = process.stdout.readline()
            if output == '' and process.poll() is not None:
                break
            if output:
                print(output.strip())
                output_lines.append(output.strip())
        
        # Wait for process to complete
        return_code = process.poll()
        
        # Log completion status
        if return_code == 0:
            logger.info("Tests completed successfully!")
        else:
            logger.warning(f"Tests completed with return code: {return_code}")
        
        # Open the report if requested
        if args.open_report:
            report_url = f"file://{report_file.absolute()}"
            logger.info(f"Opening test report: {report_url}")
            webbrowser.open(report_url)
            
        return return_code
    except subprocess.SubprocessError as e:
        logger.error(f"Error running tests: {e}")
        return 1
        
def parse_arguments():
    """Parse command-line arguments"""
    parser = argparse.ArgumentParser(description="D4XX Test Runner")
    parser.add_argument("--report-dir", default=DEFAULT_REPORT_DIR,
                      help=f"Directory to store test reports (default: {DEFAULT_REPORT_DIR})")
    parser.add_argument("--test-file", default=DEFAULT_TEST_FILE,
                      help=f"Test file to run (default: {DEFAULT_TEST_FILE})")
    parser.add_argument("--markers", default="",
                      help="Comma-separated list of markers to filter tests")
    parser.add_argument("--parallel", action="store_true",
                      help="Run tests in parallel")
    parser.add_argument("--open-report", action="store_true",
                      help="Open HTML report after test completion")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"],
                      help="Set logging level for test output (default: INFO)")
    return parser.parse_args()

def main():
    """Main entry point"""
    args = parse_arguments()
    
    logger.info("Running D4XX Test Runner")
    logger.info(f"Report directory: {args.report_dir}")
    logger.info(f"Test file: {args.test_file}")
    
    if not check_prerequisites():
        logger.error("Prerequisite check failed. Please install required dependencies.")
        return 1
        
    return run_tests(args)

if __name__ == "__main__":
    sys.exit(main())

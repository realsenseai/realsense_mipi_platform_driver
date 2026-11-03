"""Summary report plugin for D4XX V4L2 tests.

Collects per-test outcomes, groups by category, and prints a formatted
summary table at session end.
"""

import time


# Map test file stems to category names
_CATEGORIES = {
    "test_discovery": "Discovery",
    "test_streaming": "Streaming",
    "test_controls": "Controls",
    "test_metadata": "Metadata",
    "test_error_handling": "Error Handling",
}


def _categorize(nodeid):
    """Extract category from test node id (e.g. tests/test_discovery.py::...)."""
    for stem, name in _CATEGORIES.items():
        if stem in nodeid:
            return name
    return "Other"


class D4xxReportPlugin:
    """Pytest plugin that collects results and prints a summary report."""

    def __init__(self):
        self.results = {}  # category -> list of (nodeid, outcome, duration)
        self.camera_info = None
        self._start_time = None

    def set_camera_info(self, card, fw_version, device_path):
        self.camera_info = {
            "card": card,
            "fw_version": fw_version,
            "device": device_path,
        }

    def pytest_sessionstart(self, session):
        self._start_time = time.time()

    def pytest_runtest_logreport(self, report):
        if report.when != "call":
            # Handle setup/teardown skip
            if report.when == "setup" and report.skipped:
                cat = _categorize(report.nodeid)
                self.results.setdefault(cat, []).append(
                    (report.nodeid, "skipped", 0)
                )
            return

        cat = _categorize(report.nodeid)
        if report.passed:
            outcome = "passed"
        elif report.failed:
            outcome = "failed"
        elif report.skipped:
            outcome = "skipped"
        else:
            outcome = "unknown"

        self.results.setdefault(cat, []).append(
            (report.nodeid, outcome, report.duration)
        )

    def pytest_sessionfinish(self, session, exitstatus):
        duration = time.time() - self._start_time if self._start_time else 0

        total = sum(len(v) for v in self.results.values())
        passed = sum(
            1 for v in self.results.values()
            for _, o, _ in v if o == "passed"
        )
        failed = sum(
            1 for v in self.results.values()
            for _, o, _ in v if o == "failed"
        )
        skipped = sum(
            1 for v in self.results.values()
            for _, o, _ in v if o == "skipped"
        )

        width = 70
        sep = "=" * width

        lines = [
            "",
            sep,
            "D4XX V4L2 TEST SUMMARY REPORT",
            sep,
        ]

        if self.camera_info:
            info = self.camera_info
            lines.append(
                f"Camera: {info['card']} | FW: {info['fw_version']} "
                f"| Device: {info['device']}"
            )

        lines.append(
            f"Total: {total}  Passed: {passed}  Failed: {failed}  "
            f"Skipped: {skipped}  Duration: {duration:.1f}s"
        )
        lines.append("")

        # Per-category breakdown in defined order
        cat_order = list(_CATEGORIES.values()) + [
            c for c in self.results if c not in _CATEGORIES.values()
        ]
        seen = set()
        for cat in cat_order:
            if cat in seen or cat not in self.results:
                continue
            seen.add(cat)

            items = self.results[cat]
            cat_passed = sum(1 for _, o, _ in items if o == "passed")
            cat_failed = sum(1 for _, o, _ in items if o == "failed")
            cat_skipped = sum(1 for _, o, _ in items if o == "skipped")
            cat_total = len(items)

            if cat_failed > 0:
                status = "[FAIL]"
            elif cat_skipped == cat_total:
                status = "[SKIP]"
            else:
                status = "[PASS]"

            # Summary line
            if cat_skipped > 0 and cat_passed == 0 and cat_failed == 0:
                lines.append(
                    f"  {status} {cat + ':':<18s} "
                    f"0/{cat_total} ({cat_skipped} skipped)"
                )
            else:
                lines.append(
                    f"  {status} {cat + ':':<18s} "
                    f"{cat_passed}/{cat_total} passed"
                )

            # List failed tests
            for nodeid, outcome, _ in items:
                if outcome == "failed":
                    # Shorten nodeid for display
                    short = nodeid.split("::", 1)[-1] if "::" in nodeid else nodeid
                    lines.append(f"         FAILED: {short}")

        lines.append(sep)

        # Print the report
        report_text = "\n".join(lines)
        print(report_text)

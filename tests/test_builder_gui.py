"""GUI state checks; skipped automatically on machines without Tk/display support."""
from __future__ import annotations

import sys
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import tkinter as tk
    import builder
except ImportError:
    builder = None

from build_dependencies import Dependency, DependencyReport


@unittest.skipIf(builder is None, "Tkinter is unavailable")
class BuilderGuiTests(unittest.TestCase):
    def setUp(self):
        self.report = DependencyReport("linux", "linux", {})
        patcher = mock.patch("builder.check_build_dependencies", side_effect=lambda _target: self.report)
        self.check = patcher.start()
        self.addCleanup(patcher.stop)
        try:
            self.app = builder.BuilderApp()
        except tk.TclError as exc:
            self.skipTest(f"Display unavailable: {exc}")
        self.app.withdraw()
        self.addCleanup(self.close_app)
        if self.app.target.get() != "linux":
            self.app.target.set("linux")
        self.settle()

    def close_app(self):
        self.app.progress.stop()
        for event in self.app.tk.call("after", "info"):
            self.app.after_cancel(event)
        self.app.destroy()

    def settle(self):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            self.app.update()
            if not self.app.busy and self.app.checked_target is not None:
                return
            time.sleep(0.01)
        self.fail("GUI dependency check did not finish")

    def test_retry_enables_build_after_dependencies_are_fixed(self):
        self.report.dependencies = [Dependency("cmake", None, "Install CMake", ok=False)]
        self.app._check()
        self.settle()
        self.assertIn("disabled", self.app.button.state())
        self.assertIn("Install CMake", self.app.log.get("1.0", "end"))
        self.report.dependencies = [Dependency("cmake", "/tools/cmake", "3.29")]
        self.app._check()
        self.settle()
        self.assertNotIn("disabled", self.app.button.state())

    def test_target_change_rechecks_dependencies(self):
        self.report = DependencyReport("linux", "windows", {}, dependencies=[Dependency("gcc", None, "Install MinGW", ok=False)])
        self.app.target.set("windows")
        self.settle()
        self.assertEqual(self.app.checked_target, "windows")
        self.assertIn("disabled", self.app.button.state())
        self.assertEqual(self.app.retry.state(), ())

    def test_wine_button_is_optional_after_windows_build(self):
        self.app.windows_package = Path("/local/windows-package")
        with mock.patch("builder.shutil.which", return_value=None):
            self.app._set_busy(False)
            self.assertIn("disabled", self.app.wine_button.state())
        with mock.patch("builder.shutil.which", return_value="/usr/bin/wine"), mock.patch("builder.platform.system", return_value="Linux"):
            self.app._set_busy(False)
            self.assertNotIn("disabled", self.app.wine_button.state())
            self.app._set_busy(True)
            self.assertIn("disabled", self.app.wine_button.state())

    def test_build_still_requires_disc_folder(self):
        with mock.patch("builder.messagebox.showerror") as error:
            self.app._start()
        error.assert_called_once()
        self.assertFalse(self.app.busy)


if __name__ == "__main__":
    unittest.main()

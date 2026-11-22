"""Real Windows process-exit regression; no model calls."""
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import team_project as project


@unittest.skipUnless(os.name == "nt", "Windows process handle semantics")
class WindowsLivenessTests(unittest.TestCase):
    def test_exited_process_with_retained_handle_is_dead_including_259(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        for code in (0, 259):
            with self.subTest(code=code):
                with subprocess.Popen([sys.executable, "-c", f"raise SystemExit({code})"]) as child:
                    self.assertEqual(child.wait(timeout=10), code)
                    # Popen retains its process handle until context exit; the
                    # process object still exists and can be opened by PID.
                    handle = kernel.OpenProcess(0x101000, False, child.pid)
                    self.assertTrue(handle, "exited process must still be openable")
                    try:
                        observed = wintypes.DWORD()
                        self.assertTrue(kernel.GetExitCodeProcess(handle, ctypes.byref(observed)))
                        self.assertEqual(observed.value, code)
                        self.assertFalse(project._alive(child.pid))
                    finally:
                        kernel.CloseHandle(handle)

    def test_live_process_is_not_interrupted(self):
        with subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.read()"],
                              stdin=subprocess.PIPE) as child:
            try:
                self.assertTrue(project._alive(child.pid))
                self.assertIsNone(child.poll(), "liveness probe must not stop the process")
            finally:
                child.communicate(timeout=10)

    def test_inaccessible_or_uncertain_process_is_not_declared_dead(self):
        import ctypes
        kernel = MagicMock()
        with patch.object(ctypes, "WinDLL", return_value=kernel):
            kernel.OpenProcess.return_value = None
            for error, expected in ((5, True), (87, False)):
                with patch.object(ctypes, "get_last_error", return_value=error):
                    self.assertEqual(project._alive(123), expected)
            kernel.OpenProcess.return_value = 1234
            kernel.WaitForSingleObject.return_value = 0xffffffff  # WAIT_FAILED
            self.assertTrue(project._alive(123))
            kernel.CloseHandle.assert_called_once_with(1234)


if __name__ == "__main__":
    unittest.main()

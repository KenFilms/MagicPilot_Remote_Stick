import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import flash


class FlashTests(unittest.TestCase):
    def test_supported_chips(self):
        self.assertEqual(flash.environment_for_chip("ESP32"), "m5stick-c")
        self.assertEqual(flash.environment_for_chip("ESP32-S3"), "m5stick-s3")

    def test_unsupported_chip_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unsupported chip"):
            flash.environment_for_chip("ESP32-C3")

    def test_single_usb_device(self):
        ports = [SimpleNamespace(device="bluetooth", vid=None),
                 SimpleNamespace(device="stick", vid=0x303A)]
        self.assertEqual(flash.select_port(ports), "stick")

    def test_no_usb_device_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "No USB serial"):
            flash.select_port([SimpleNamespace(device="bluetooth", vid=None)])

    def test_multiple_devices_require_explicit_port(self):
        ports = [SimpleNamespace(device="stick1", vid=0x303A),
                 SimpleNamespace(device="stick2", vid=0x10C4)]
        with self.assertRaisesRegex(RuntimeError, "Multiple USB serial"):
            flash.select_port(ports)
        self.assertEqual(flash.select_port(ports, "stick2"), "stick2")

    def run_main(self, chip, detect_only=False, error=None, probe_error=None):
        arguments = ["flash.py"] + (["--detect-only"] if detect_only else [])
        info = json.dumps({"python_exe": {"value": flash.sys.executable}})
        with patch.object(flash.sys, "argv", arguments), \
                patch.object(flash, "find_platformio", return_value="pio"), \
                patch.object(flash.subprocess, "check_output", return_value=info), \
                patch("serial.tools.list_ports.comports", return_value=[]), \
                patch.object(flash, "select_port", return_value="stick"), \
                patch.object(flash, "probe_chip", return_value=chip, side_effect=probe_error), \
                patch.object(flash.subprocess, "call", return_value=0) as upload:
            if error:
                with self.assertRaisesRegex(RuntimeError, error):
                    flash.main()
                upload.assert_not_called()
                return
            result = flash.main()
            return result, upload.call_args_list

    def test_upload_uses_detected_environment_and_port(self):
        for chip, environment in flash.ENVIRONMENTS.items():
            result, calls = self.run_main(chip)
            self.assertEqual(result, 0)
            self.assertEqual(calls[0].args[0], flash.upload_command("pio", environment, "stick"))
            self.assertEqual(len(calls), 1)

    def test_detect_only_never_uploads(self):
        result, calls = self.run_main("ESP32-S3", detect_only=True)
        self.assertEqual(result, 0)
        self.assertEqual(calls, [])

    def test_unsupported_chip_never_uploads(self):
        self.run_main("ESP32-C3", error="Unsupported chip")

    def test_probe_failure_never_uploads(self):
        self.run_main(None, error="probe failed", probe_error=RuntimeError("probe failed"))

    def test_usb_reenumeration_follows_same_location(self):
        original = SimpleNamespace(device="old", vid=0x303A, location="3-1")
        replacement = SimpleNamespace(device="new", vid=0x303A, location="3-1")
        with patch.object(flash, "probe_chip", side_effect=[OSError(), "ESP32-S3"]) as probe, \
                patch("serial.tools.list_ports.comports", side_effect=[[], [replacement]]):
            self.assertEqual(flash.detect_device("old", [original]), ("ESP32-S3", "new"))
            self.assertEqual(probe.call_args_list[-1].args, ("new",))

    def test_usb_reenumeration_does_not_select_another_device(self):
        original = SimpleNamespace(device="old", vid=0x303A, location="3-1")
        other = SimpleNamespace(device="other", vid=0x303A, location="3-2")
        with patch.object(flash, "probe_chip", side_effect=OSError()) as probe, \
                patch("serial.tools.list_ports.comports", side_effect=[[], [other]]), \
                patch.object(flash.time, "monotonic", side_effect=[0, 0, 11]), \
                patch.object(flash.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "did not return"):
                flash.detect_device("old", [original])
            self.assertEqual(probe.call_count, 1)

    def test_virtualenv_python_symlink_still_reexecutes(self):
        with tempfile.TemporaryDirectory() as directory:
            python = Path(directory) / "python"
            try:
                python.symlink_to(flash.sys.executable)
            except OSError:
                self.skipTest("Creating symlinks is not permitted")
            info = json.dumps({"python_exe": {"value": str(python)}})
            with patch.object(flash.sys, "argv", ["flash.py", "--detect-only"]), \
                    patch.object(flash, "find_platformio", return_value="pio"), \
                    patch.object(flash.subprocess, "check_output", return_value=info), \
                    patch.object(flash.subprocess, "call", return_value=0) as bootstrap:
                self.assertEqual(flash.main(), 0)
                self.assertEqual(bootstrap.call_args.args[0][0], str(python))


if __name__ == "__main__":
    unittest.main()
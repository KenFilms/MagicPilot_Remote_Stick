import unittest

import build_installer


class BuildInstallerTests(unittest.TestCase):
    def test_manifest_maps_each_chip_to_its_merged_image(self):
        builds = build_installer.manifest("v1.0.0")["builds"]
        self.assertEqual(builds, [
            {"chipFamily": "ESP32",
             "parts": [{"path": "MagicPilot_Remote_Stick-m5stick-c.bin", "offset": 0}]},
            {"chipFamily": "ESP32-S3",
             "parts": [{"path": "MagicPilot_Remote_Stick-m5stick-s3.bin", "offset": 0}]},
        ])

    def test_manifest_skips_improv_wait(self):
        manifest = build_installer.manifest("v1.0.0")
        self.assertEqual(manifest["version"], "v1.0.0")
        self.assertEqual(manifest["new_install_improv_wait_time"], 0)


if __name__ == "__main__":
    unittest.main()

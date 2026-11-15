"""Build both Stick firmwares and publish them to the browser installer in docs/install.

Each board gets one merged image (bootloader, partitions, boot_app0 and app) that is
flashed at offset 0x0. The manifest lets ESP Web Tools pick the image for the chip it
detects, the same ESP32 / ESP32-S3 mapping that flash.py uses.
"""

import json
import shutil
import subprocess
import sys

from flash import ENVIRONMENTS, PROJECT_DIR, find_platformio


INSTALL_DIR = PROJECT_DIR / "docs" / "install"


def image_name(environment):
    return "MagicPilot_Remote_Stick-" + environment + ".bin"


def firmware_version():
    try:
        return subprocess.check_output(
            ["git", "-C", str(PROJECT_DIR), "describe", "--tags", "--always", "--dirty"],
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def manifest(version):
    return {
        "name": "MagicPilot Remote Stick",
        "version": version,
        "new_install_improv_wait_time": 0,
        "builds": [
            {"chipFamily": chip, "parts": [{"path": image_name(environment), "offset": 0}]}
            for chip, environment in ENVIRONMENTS.items()
        ],
    }


def main():
    # Read the version before the copies below modify tracked files in docs/install.
    version = firmware_version()
    command = [find_platformio(), "run", "-d", str(PROJECT_DIR)]
    for environment in ENVIRONMENTS.values():
        command += ["-e", environment]
    subprocess.check_call(command)

    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    for environment in ENVIRONMENTS.values():
        merged = PROJECT_DIR / ".pio" / "build" / environment / "firmware-merged.bin"
        shutil.copyfile(merged, INSTALL_DIR / image_name(environment))
    (INSTALL_DIR / "manifest.json").write_text(json.dumps(manifest(version), indent=2) + "\n")
    print("Installer updated in " + str(INSTALL_DIR.relative_to(PROJECT_DIR)) + " (version " + version + ")")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print("Build failed; installer files were not changed.", file=sys.stderr)
        sys.exit(error.returncode)

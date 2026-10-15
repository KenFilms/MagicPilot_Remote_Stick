"""Detect the connected Stick's chip, then build and upload its firmware."""

import argparse
import json
import os
from pathlib import Path
import shutil
import site
import subprocess
import sys
import time


PROJECT_DIR = Path(__file__).resolve().parent
ENVIRONMENTS = {"ESP32": "m5stick-c", "ESP32-S3": "m5stick-s3"}
ESPTOOL_PACKAGE = "platformio/tool-esptoolpy@~2.40900.0"


def find_platformio():
    for name in ("pio", "platformio"):
        executable = shutil.which(name)
        if executable:
            return executable
    core_dir = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    relative = "Scripts/platformio.exe" if os.name == "nt" else "bin/platformio"
    executable = core_dir / "penv" / relative
    if executable.is_file():
        return str(executable)
    raise RuntimeError("PlatformIO not found. Install PlatformIO Core or its VS Code extension.")


def select_port(ports, requested=None):
    if requested:
        return requested
    candidates = [port.device for port in ports if port.vid is not None]
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("No USB serial device found. Connect a Stick or specify --port.")
    raise RuntimeError(
        "Multiple USB serial devices found: " + ", ".join(candidates)
        + ". Disconnect the others or specify --port."
    )


def environment_for_chip(chip):
    if chip not in ENVIRONMENTS:
        raise RuntimeError("Unsupported chip: " + chip + ". No firmware will be uploaded.")
    return ENVIRONMENTS[chip]


def probe_chip(port):
    from platformio.package.manager.tool import ToolPackageManager

    manager = ToolPackageManager()
    package = manager.get_package(ESPTOOL_PACKAGE)
    if package is None:
        package = manager.install(ESPTOOL_PACKAGE)
    sys.path.insert(0, str(package.path))
    dependencies = str(Path(package.path) / "_contrib")
    site.addsitedir(dependencies)
    sys.path.insert(0, dependencies)
    import esptool

    from serial import serial_for_url

    with serial_for_url(port, baudrate=115200) as connection:
        device = esptool.detect_chip(port=connection, connect_attempts=3)
        return device.CHIP_NAME


def detect_device(port, ports):
    from serial.tools import list_ports

    original = next((device for device in ports if device.device == port), None)
    try:
        return probe_chip(port), port
    except Exception:
        if original is None or not original.location:
            raise
        if any(device.device == port for device in list_ports.comports()):
            raise
        print("USB disconnected during reset; waiting for the same device...", flush=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            matches = [device for device in list_ports.comports()
                       if device.location == original.location and device.vid == original.vid]
            if len(matches) == 1:
                new_port = matches[0].device
                return probe_chip(new_port), new_port
            time.sleep(0.25)
        raise RuntimeError("USB device did not return. Put the Stick in download mode and retry.")


def upload_command(platformio, environment, port):
    return [platformio, "run", "-d", str(PROJECT_DIR), "-e", environment,
            "-t", "upload", "--upload-port", port]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, e.g. /dev/cu.usbmodem3101 or COM3")
    parser.add_argument("--detect-only", action="store_true",
                        help="Probe without writing flash; leaves the Stick in download mode")
    args = parser.parse_args()
    platformio = find_platformio()
    info = json.loads(subprocess.check_output(
        [platformio, "system", "info", "--json-output"], text=True
    ))
    python = info["python_exe"]["value"]
    if Path(sys.executable).absolute() != Path(python).absolute():
        return subprocess.call([python, str(Path(__file__).resolve()), *sys.argv[1:]])

    from serial.tools import list_ports

    ports = list(list_ports.comports())
    port = select_port(ports, args.port)
    print("Probing " + port + " (the Stick will reset)...", flush=True)
    chip, port = detect_device(port, ports)
    environment = environment_for_chip(chip)
    print("Detected " + chip + "; selected " + environment, flush=True)
    if args.detect_only:
        print("No flash written. Press reset or power-cycle to resume the existing firmware.")
        return 0
    return subprocess.call(upload_command(platformio, environment, port))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as error:
        print("Upload stopped: " + str(error), file=sys.stderr)
        print("Close serial monitors. On StickS3, hold reset until the green LED flashes, "
              "then retry with --port if needed. No fallback firmware is selected.", file=sys.stderr)
        sys.exit(1)
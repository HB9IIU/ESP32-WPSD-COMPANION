from __future__ import annotations

import atexit
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from configparser import ConfigParser
from datetime import datetime, timezone
from pathlib import Path

Import("env")

from SCons.Script import COMMAND_LINE_TARGETS, GetBuildFailures


CHIP_FAMILY_MAP = {
    "ESP32C61": "esp32c61",
    "ESP32C6": "esp32c6",
    "ESP32C5": "esp32c5",
    "ESP32C3": "esp32c3",
    "ESP32C2": "esp32c2",
    "ESP32S3": "esp32s3",
    "ESP32S2": "esp32s2",
    "ESP32H2": "esp32h2",
    "ESP32P4": "esp32p4",
    "ESP8266": "esp8266",
    "ESP32": "esp32",
}

FILESYSTEM_IMAGE_NAMES = {
    "spiffs": "spiffs.bin",
    "littlefs": "littlefs.bin",
    "fat": "fatfs.bin",
}

SKIP_EXPORT_ENV_VAR = "EXPORT_MERGED_FIRMWARE_SKIP"
BUILD_NUMBER_HEADER = "include/build_number.h"
BUILD_NUMBER_DEFINE = "FIRMWARE_BUILD_NUMBER"

FIRMWARE_URL = (
    "https://raw.githubusercontent.com/"
    "HB9IIU/ESP32-WPSD-COMPANION/main/firmware/firmware.bin"
)
APP_URL = (
    "https://raw.githubusercontent.com/"
    "HB9IIU/ESP32-WPSD-COMPANION/main/firmware/app.bin"
)
SPIFFS_URL = (
    "https://raw.githubusercontent.com/"
    "HB9IIU/ESP32-WPSD-COMPANION/main/firmware/spiffs.bin"
)


def log(message: str) -> None:
    print(f"[merged-firmware] {message}")


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def normalize_offset(value: str | int) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def detect_chip_name(idedata: dict) -> str:
    defines = set(idedata.get("defines", []))
    for macro, chip_name in CHIP_FAMILY_MAP.items():
        if macro in defines:
            return chip_name
    return "esp32"


def load_platformio_config(path: Path) -> ConfigParser:
    parser = ConfigParser(inline_comment_prefixes=(";", "#"))
    parser.optionxform = str
    parser.read(path, encoding="utf-8")
    return parser


def get_partitions_csv(project_dir: Path, env_name: str) -> Path | None:
    config = load_platformio_config(project_dir / "platformio.ini")
    section = f"env:{env_name}"

    if not config.has_section(section):
        return None

    value = config.get(section, "board_build.partitions", fallback="").strip()
    if not value:
        return None

    return (project_dir / value).resolve()


def parse_partitions_csv(csv_path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []

    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle)

        for raw_row in reader:
            if not raw_row:
                continue

            if raw_row[0].strip().startswith("#"):
                continue

            padded = [column.strip() for column in raw_row] + [""] * 6

            rows.append(
                {
                    "name": padded[0],
                    "type": padded[1],
                    "subtype": padded[2],
                    "offset": padded[3],
                    "size": padded[4],
                    "flags": padded[5],
                }
            )

    return rows


def find_platformio_cli() -> str | None:
    candidates = (
        shutil.which("platformio"),
        shutil.which("pio"),
        str(Path.home() / ".platformio" / "penv" / "bin" / "platformio"),
    )

    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate

    return None


def build_filesystem_image(project_dir: Path, env_name: str) -> None:
    data_dir = project_dir / "data"

    if not data_dir.exists():
        log("No data folder found, skipping filesystem image")
        return

    platformio = find_platformio_cli()

    if not platformio:
        log("PlatformIO CLI not found, skipping filesystem image rebuild")
        return

    log("Building filesystem image")

    command = [platformio, "run", "-e", env_name, "-t", "buildfs"]

    child_env = os.environ.copy()
    child_env[SKIP_EXPORT_ENV_VAR] = "1"

    result = subprocess.run(command, cwd=project_dir, env=child_env, check=False)

    if result.returncode != 0:
        log("Filesystem image build failed, continuing without rebuilding it")


def find_filesystem_part(
    project_dir: Path,
    env_name: str,
    build_dir: Path,
) -> tuple[int, Path] | None:
    partitions_csv = get_partitions_csv(project_dir, env_name)

    if partitions_csv is None or not partitions_csv.exists():
        log("No partitions CSV found")
        return None

    for row in parse_partitions_csv(partitions_csv):
        subtype = row["subtype"].lower()
        image_name = FILESYSTEM_IMAGE_NAMES.get(subtype)

        if not image_name:
            continue

        image_path = build_dir / image_name

        if not image_path.exists():
            log(f"Filesystem image not found: {image_path}")
            continue

        log(f"Adding filesystem image: {image_path}")
        return normalize_offset(row["offset"]), image_path

    return None


def find_esptool_command(build_env) -> list[str]:
    package_dir = build_env.PioPlatform().get_package_dir("tool-esptoolpy")

    if package_dir:
        esptool_path = Path(package_dir) / "esptool.py"

        if esptool_path.exists():
            return [sys.executable, str(esptool_path)]

    for executable in (shutil.which("esptool"), shutil.which("esptool.py")):
        if executable:
            return [executable]

    raise RuntimeError("Unable to locate esptool")


def calculate_sha256(file_path: Path) -> str:
    sha256 = hashlib.sha256()

    with file_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(4096), b""):
            sha256.update(chunk)

    return sha256.hexdigest()


def read_firmware_build_number(project_dir: Path) -> int:
    header_path = project_dir / BUILD_NUMBER_HEADER

    if not header_path.exists():
        log(f"Build number header not found: {header_path}")
        return 0

    match = re.search(
        rf"^\s*#define\s+{BUILD_NUMBER_DEFINE}\s+(\d+)\s*$",
        header_path.read_text(encoding="utf-8"),
        re.MULTILINE,
    )

    if match is None:
        log(f"Build number define not found in {header_path}")
        return 0

    return int(match.group(1))


def create_manifest(
    project_dir: Path,
    output_dir: Path,
    app_offset: int,
    filesystem_part: tuple[int, Path] | None,
) -> None:
    firmware_file = output_dir / "firmware.bin"
    app_file = output_dir / "app.bin"
    spiffs_file = output_dir / "spiffs.bin"
    manifest_file = output_dir / "manifest.json"

    if not firmware_file.exists():
        log("firmware.bin not found, cannot create manifest")
        return

    if not app_file.exists():
        log("app.bin not found, cannot create manifest")
        return

    now_utc = datetime.now(timezone.utc)
    build_number = read_firmware_build_number(project_dir)

    manifest = {
        "build_number": build_number,
        "build_time": now_utc.isoformat(),
        "firmware": "firmware.bin",
        "size": firmware_file.stat().st_size,
        "sha256": calculate_sha256(firmware_file),
        "url": FIRMWARE_URL,
        "update_parts": [
            {
                "name": "app",
                "firmware": "app.bin",
                "offset": app_offset,
                "size": app_file.stat().st_size,
                "sha256": calculate_sha256(app_file),
                "url": APP_URL,
            }
        ],
    }

    if filesystem_part is not None and spiffs_file.exists():
        filesystem_offset, _ = filesystem_part
        manifest["update_parts"].append(
            {
                "name": "spiffs",
                "firmware": "spiffs.bin",
                "offset": filesystem_offset,
                "size": spiffs_file.stat().st_size,
                "sha256": calculate_sha256(spiffs_file),
                "url": SPIFFS_URL,
            }
        )

    manifest_file.write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )

    log(f"Wrote {manifest_file}")


def export_update_parts(
    output_dir: Path,
    app_firmware_path: Path,
    filesystem_part: tuple[int, Path] | None,
) -> None:
    app_output = output_dir / "app.bin"
    shutil.copy2(app_firmware_path, app_output)
    log(f"Wrote {app_output}")

    spiffs_output = output_dir / "spiffs.bin"

    if filesystem_part is None:
        if spiffs_output.exists():
            spiffs_output.unlink()
        log("No filesystem image found, skipped spiffs.bin")
        return

    _, filesystem_path = filesystem_part
    shutil.copy2(filesystem_path, spiffs_output)
    log(f"Wrote {spiffs_output}")


def export_merged_firmware(source, target, env) -> None:
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    env_name = env.subst("$PIOENV")

    output_dir = project_dir / "firmware"
    output_file = output_dir / "firmware.bin"

    output_dir.mkdir(parents=True, exist_ok=True)

    build_filesystem_image(project_dir, env_name)

    idedata_path = build_dir / "idedata.json"
    app_firmware_path = build_dir / "firmware.bin"

    if not idedata_path.exists():
        raise RuntimeError(f"Missing {idedata_path}")

    if not app_firmware_path.exists():
        raise RuntimeError(f"Missing {app_firmware_path}")

    idedata = load_json(idedata_path)
    chip_name = detect_chip_name(idedata)

    parts: list[tuple[int, Path]] = []

    for image in idedata.get("extra", {}).get("flash_images", []):
        parts.append((normalize_offset(image["offset"]), Path(image["path"])))

    app_offset = normalize_offset(
        idedata.get("extra", {}).get("application_offset", "0x10000")
    )

    parts.append((app_offset, app_firmware_path))

    filesystem_part = find_filesystem_part(project_dir, env_name, build_dir)

    if filesystem_part is not None:
        parts.append(filesystem_part)

    export_update_parts(output_dir, app_firmware_path, filesystem_part)

    command = [
        *find_esptool_command(env),
        "--chip",
        chip_name,
        "merge_bin",
        "-o",
        str(output_file),
    ]

    for offset, path in parts:
        command.extend([hex(offset), str(path)])

    log("Creating merged firmware image")
    result = subprocess.run(command, cwd=project_dir, check=False)

    if result.returncode != 0:
        raise RuntimeError("Failed to create merged firmware image")

    log(f"Wrote {output_file}")

    create_manifest(project_dir, output_dir, app_offset, filesystem_part)


def should_export_on_exit() -> bool:
    if os.environ.get(SKIP_EXPORT_ENV_VAR) == "1":
        log("Skipping export because recursive buildfs call is active")
        return False

    if GetBuildFailures():
        log("Skipping export because build failed")
        return False

    if COMMAND_LINE_TARGETS and all(target == "clean" for target in COMMAND_LINE_TARGETS):
        log("Skipping export because target is clean")
        return False

    build_dir = Path(env.subst("$BUILD_DIR"))

    idedata_exists = (build_dir / "idedata.json").exists()
    firmware_exists = (build_dir / "firmware.bin").exists()

    if not idedata_exists:
        log("Skipping export because idedata.json does not exist")
        return False

    if not firmware_exists:
        log("Skipping export because build firmware.bin does not exist")
        return False

    return True


def export_merged_firmware_on_exit() -> None:
    log("Exit hook called")

    if should_export_on_exit():
        export_merged_firmware(None, None, env)
    else:
        log("Export condition not met")


atexit.register(export_merged_firmware_on_exit)

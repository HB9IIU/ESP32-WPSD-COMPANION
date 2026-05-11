from __future__ import annotations

import atexit
import csv
import json
import os
import shutil
import subprocess
import sys
from configparser import ConfigParser
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
        return

    platformio = find_platformio_cli()
    if not platformio:
        log("PlatformIO CLI not found, skipping filesystem image rebuild")
        return

    command = [platformio, "run", "-e", env_name, "-t", "buildfs"]
    child_env = os.environ.copy()
    child_env[SKIP_EXPORT_ENV_VAR] = "1"
    result = subprocess.run(command, cwd=project_dir, env=child_env, check=False)
    if result.returncode != 0:
        log("Filesystem image build failed, continuing without rebuilding it")


def find_filesystem_part(project_dir: Path, env_name: str, build_dir: Path) -> tuple[int, Path] | None:
    partitions_csv = get_partitions_csv(project_dir, env_name)
    if partitions_csv is None or not partitions_csv.exists():
        return None

    for row in parse_partitions_csv(partitions_csv):
        subtype = row["subtype"].lower()
        image_name = FILESYSTEM_IMAGE_NAMES.get(subtype)
        if not image_name:
            continue
        image_path = build_dir / image_name
        if not image_path.exists():
            continue
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


def export_merged_firmware(source, target, env) -> None:
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    env_name = env.subst("$PIOENV")
    output_dir = project_dir / "firmware"
    output_file = output_dir / "firmware.bin"
    output_dir.mkdir(parents=True, exist_ok=True)

    build_filesystem_image(project_dir, env_name)

    idedata = load_json(build_dir / "idedata.json")
    chip_name = detect_chip_name(idedata)

    parts: list[tuple[int, Path]] = []
    for image in idedata.get("extra", {}).get("flash_images", []):
        parts.append((normalize_offset(image["offset"]), Path(image["path"])))

    app_offset = normalize_offset(idedata.get("extra", {}).get("application_offset", "0x10000"))
    parts.append((app_offset, build_dir / "firmware.bin"))

    filesystem_part = find_filesystem_part(project_dir, env_name, build_dir)
    if filesystem_part is not None:
        parts.append(filesystem_part)

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

    result = subprocess.run(command, cwd=project_dir, check=False)
    if result.returncode != 0:
        raise RuntimeError("Failed to create merged firmware image")

    log(f"Wrote {output_file}")


def should_export_on_exit() -> bool:
    if os.environ.get(SKIP_EXPORT_ENV_VAR) == "1":
        return False

    if GetBuildFailures():
        return False

    if COMMAND_LINE_TARGETS and all(target == "clean" for target in COMMAND_LINE_TARGETS):
        return False

    build_dir = Path(env.subst("$BUILD_DIR"))
    return (build_dir / "idedata.json").exists() and (build_dir / "firmware.bin").exists()


def export_merged_firmware_on_exit() -> None:
    if should_export_on_exit():
        export_merged_firmware(None, None, env)


atexit.register(export_merged_firmware_on_exit)
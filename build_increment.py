from __future__ import annotations

import os
import re
from pathlib import Path

Import("env")

from SCons.Script import COMMAND_LINE_TARGETS


SKIP_EXPORT_ENV_VAR = "EXPORT_MERGED_FIRMWARE_SKIP"
HEADER_GUARD = "BUILD_NUMBER_H"
HEADER_NAME = "build_number.h"
DEFINE_NAME = "FIRMWARE_BUILD_NUMBER"
SKIPPED_TARGETS = {"buildfs", "uploadfs", "clean", "erase"}


def log(message: str) -> None:
    print(f"[build-number] {message}")


def make_header(build_number: int) -> str:
    return (
        f"#ifndef {HEADER_GUARD}\n"
        f"#define {HEADER_GUARD}\n"
        "\n"
        f"#define {DEFINE_NAME} {build_number}\n"
        "\n"
        f"#endif // {HEADER_GUARD}\n"
    )


def read_build_number(header_path: Path) -> int:
    if not header_path.exists():
        return 0

    match = re.search(
        rf"^\s*#define\s+{DEFINE_NAME}\s+(\d+)\s*$",
        header_path.read_text(encoding="utf-8"),
        re.MULTILINE,
    )

    if match is None:
        return 0

    return int(match.group(1))


def increment_build_number() -> None:
    if os.environ.get(SKIP_EXPORT_ENV_VAR) == "1":
        log("Skipping increment during recursive filesystem build")
        return

    if COMMAND_LINE_TARGETS and all(target in SKIPPED_TARGETS for target in COMMAND_LINE_TARGETS):
        log("Skipping increment for non-firmware target")
        return

    project_dir = Path(env.subst("$PROJECT_DIR"))
    header_path = project_dir / "include" / HEADER_NAME
    header_path.parent.mkdir(parents=True, exist_ok=True)

    next_build_number = read_build_number(header_path) + 1
    header_path.write_text(make_header(next_build_number), encoding="utf-8")

    log(f"Firmware build number: {next_build_number}")


increment_build_number()

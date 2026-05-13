#!/usr/bin/env python3
"""Download missing flag images and resize to small (24x17) and large (56x38)."""

import urllib.request
import urllib.error
import ssl
import io
import sys
from pathlib import Path
from PIL import Image

SSL_CTX = ssl.create_default_context()
SSL_CTX.check_hostname = False
SSL_CTX.verify_mode = ssl.CERT_NONE

MISSING = [
    "AI", "AS", "AW", "BM", "BQ", "CW", "FK", "FO", "GF",
    "GI", "GL", "GP", "GU", "KY", "LI", "MO", "MQ", "MS",
    "NC", "PR", "RE", "SH", "VG", "VI", "XK",
]

SIZES = {
    "small": (24, 17),
    "large": (56, 38),
}

# flagcdn.com serves PNGs by lowercase ISO code at various widths
CDN = "https://flagcdn.com/w160/{code}.png"

BASE = Path(__file__).parent / "data" / "flags"
JPEG_QUALITY = 85


def download_png(code: str) -> bytes | None:
    url = CDN.format(code=code.lower())
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=10, context=SSL_CTX) as resp:
            return resp.read()
    except Exception as exc:
        print(f"  ERROR downloading {url}: {exc}")
        return None


def save_jpeg(img: Image.Image, path: Path, size: tuple[int, int]) -> None:
    resized = img.resize(size, Image.LANCZOS)
    rgb = resized.convert("RGB")
    rgb.save(path, "JPEG", quality=JPEG_QUALITY, optimize=True)


def main() -> None:
    ok = []
    failed = []

    for code in MISSING:
        print(f"[{code}] downloading...", end=" ", flush=True)
        data = download_png(code)

        if data is None:
            failed.append(code)
            continue

        try:
            img = Image.open(io.BytesIO(data))
        except Exception as exc:
            print(f"ERROR decoding image: {exc}")
            failed.append(code)
            continue

        for folder, size in SIZES.items():
            dest = BASE / folder / f"{code}.jpg"
            save_jpeg(img, dest, size)

        print(f"OK  ({img.size[0]}x{img.size[1]} source)")
        ok.append(code)

    print()
    print(f"Done: {len(ok)} downloaded, {len(failed)} failed")
    if failed:
        print(f"Failed: {' '.join(failed)}")
        sys.exit(1)


if __name__ == "__main__":
    main()

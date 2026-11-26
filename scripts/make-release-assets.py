#!/usr/bin/env python3
"""Build the firmware.json descriptor for a master app release asset.

Usage:
    pio run -e esp32-c3-devkitm-1
    python3 scripts/make-release-assets.py

The descriptor is written next to firmware.bin in the PlatformIO build folder.
Upload both files to the coordinated GitHub Release (for example
`mqtt-compat-2`). No version is hard-coded here: the values come from
include/Version.h, so the asset cannot disagree with the flashed image.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_HEADER = ROOT / "include" / "Version.h"
DEFAULT_IMAGE = (
    ROOT / ".pio" / "build" / "esp32-c3-devkitm-1" / "firmware.bin"
)


def macro(name: str, header: str) -> str:
    match = re.search(rf"^#define\s+{name}\s+(.+)$", header, re.MULTILINE)
    if not match:
        raise SystemExit(f"missing #define {name} in {VERSION_HEADER}")
    return match.group(1).strip()


def unquote(value: str) -> str:
    return value[1:-1] if value.startswith('"') and value.endswith('"') else value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--image",
        type=Path,
        default=DEFAULT_IMAGE,
        help="built master app image (default: %(default)s)",
    )
    parser.add_argument(
        "--channel", default="stable", help="release channel (default: stable)"
    )
    parser.add_argument("--notes", default="Master application image")
    args = parser.parse_args()

    header = VERSION_HEADER.read_text(encoding="utf-8")
    bus = unquote(macro("MASTER_BUS_COMPAT", header))
    mqtt = unquote(macro("MASTER_MQTT_COMPAT", header))
    release = unquote(macro("MASTER_RELEASE", header))
    version = f"{bus}.{mqtt}.{release}"

    image = args.image.resolve()
    payload = image.read_bytes()
    descriptor = {
        "kind": "master-app",
        "schema": 1,
        "version": version,
        "mqtt_compat": int(mqtt),
        "bus_compat": int(bus),
        "image": image.name,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "channel": args.channel,
        "notes": args.notes,
    }
    output = image.with_name("firmware.json")
    output.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")

    print(f"version={version} size={len(payload)} sha256={descriptor['sha256']}")
    print(f"wrote {output}")


if __name__ == "__main__":
    main()

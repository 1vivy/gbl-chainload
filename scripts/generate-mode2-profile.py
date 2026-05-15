#!/usr/bin/env python3
"""Generate a minimal mode-2 profile XML from stock vbmeta bytes."""

from __future__ import annotations

import argparse
import hashlib
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stock-vbmeta", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--device", default="unknown")
    ap.add_argument("--security-patch", default="unknown")
    args = ap.parse_args()

    data = args.stock_vbmeta.read_bytes()
    if len(data) < 4 or not data.startswith(b"AVB0"):
        raise SystemExit("stock vbmeta must start with AVB0")

    root = ET.Element("gbl-chainload-profile", {"version": "1", "mode": "2"})
    ET.SubElement(root, "device").text = args.device
    ET.SubElement(root, "security-patch").text = args.security_patch
    stock = ET.SubElement(root, "stock-vbmeta")
    ET.SubElement(stock, "sha256").text = sha256(args.stock_vbmeta)
    ET.SubElement(stock, "size").text = str(len(data))
    ET.SubElement(root, "generated-utc").text = datetime.now(timezone.utc).isoformat()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(root)  # Python 3.9+
    ET.ElementTree(root).write(args.out, encoding="utf-8", xml_declaration=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

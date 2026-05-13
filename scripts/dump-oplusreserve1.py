#!/usr/bin/env python3
"""Dump and diff known OPPO/OnePlus oplusreserve1 structures.

This is intentionally read-only. It understands the fields statically mapped
from LinuxLoader_infiniti.efi: DeepTest fastboot token and UnlockRecord.
"""

from __future__ import annotations

import argparse
import hashlib
import string
import struct
from dataclasses import dataclass
from pathlib import Path


BLOCK_SIZE = 4096


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def u32le(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def ascii_clean(data: bytes) -> str:
    out = "".join(chr(b) if chr(b) in string.printable and b not in (0x0b, 0x0c) else "." for b in data)
    return out.rstrip("\x00")


def ascii_field(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def nonzero_count(data: bytes) -> int:
    return sum(1 for b in data if b != 0)


@dataclass(frozen=True)
class ReserveImage:
    path: Path
    data: bytes

    @property
    def block_count(self) -> int:
        return len(self.data) // BLOCK_SIZE

    @property
    def last_block(self) -> int:
        return self.block_count - 1

    def has_lba(self, lba: int) -> bool:
        return 0 <= lba < self.block_count

    def block(self, lba: int) -> bytes:
        if not self.has_lba(lba):
            raise IndexError(f"LBA {lba} is outside {self.path} ({self.block_count} blocks)")
        start = lba * BLOCK_SIZE
        return self.data[start : start + BLOCK_SIZE]


def load_image(path: Path) -> ReserveImage:
    data = path.read_bytes()
    if len(data) % BLOCK_SIZE != 0:
        raise SystemExit(f"{path}: size 0x{len(data):x} is not a multiple of 0x{BLOCK_SIZE:x}")
    return ReserveImage(path=path, data=data)


def known_lbas(img: ReserveImage) -> dict[str, int]:
    candidates = {
        # Canonical LBAs from full 8 MiB dumps (LastBlock=2047). Prefer these
        # when present because some user-supplied dumps are truncated but still
        # contain the canonical token/record offsets.
        "token_fastboot_unlock_data": 1114,
        "unlock_record": 1187,
        "uart_tail_flag": 1065,
        "uart_absolute_0x429": 0x429,
        "oplus_charge_ufs": 0x48A,
        "oplus_serial_ufs": 0x520,
        "bf_block": 0x514,
        "ddr_control_tail": (len(img.data) - 0x20000) // BLOCK_SIZE,
    }
    # Tail-derived candidates are useful for truly smaller partitions, but are
    # kept under explicit names to avoid confusing them with canonical 8 MiB
    # offsets in truncated dumps.
    candidates.update(
        {
            "tail_token_fastboot_unlock_data": img.last_block - 0x3A5,
            "tail_unlock_record": img.last_block - 0x35C,
            "tail_uart_flag": img.last_block - 0x3D6,
        }
    )
    # eMMC constants seen in ABL are outside 8 MiB sample dumps, but include
    # them automatically when the image is large enough.
    optional = {
        "oplus_charge_emmc": 0x233E,
        "oplus_serial_emmc": 0x26FA,
    }
    candidates.update(optional)
    return {k: v for k, v in candidates.items() if img.has_lba(v)}


def block_label(img: ReserveImage, lba: int) -> str:
    labels = [name for name, known in known_lbas(img).items() if known == lba]
    return ", ".join(labels) if labels else ""


def parse_token(block: bytes) -> dict[str, object]:
    return {
        "sha256": sha256(block),
        "nonzero_full": nonzero_count(block),
        "nonzero_first_0x140": nonzero_count(block[:0x140]),
        "is_all_zero": nonzero_count(block) == 0,
        "serial_0x100": ascii_field(block[0x100:0x108]),
        "marker_0x108": ascii_field(block[0x108:0x10C]),
        "permission_0x10c": ascii_field(block[0x10C:0x10D]),
        "binding31_0x10d": ascii_field(block[0x10D:0x12C]),
        "model_0x12c": ascii_field(block[0x12C:0x13C]),
        "first_0x140_hex": block[:0x140].hex(),
    }


def unlock_record_payload(record: bytes) -> bytes:
    return (
        record[0x24:0x28]
        + record[0x28:0x38]
        + struct.pack("<I", u32le(record, 0x38))
        + struct.pack("<I", u32le(record, 0x3C))
        + struct.pack("<I", 0xFE1956C9)
    )


def parse_unlock_record(block: bytes) -> dict[str, object]:
    stored = block[0x04:0x24]
    payload = unlock_record_payload(block)
    computed = hashlib.sha256(payload).digest()
    return {
        "block_sha256": sha256(block),
        "magic_le": f"0x{u32le(block, 0x00):08x}",
        "stored_hash": stored.hex(),
        "computed_hash": computed.hex(),
        "hash_valid": stored == computed,
        "version_or_initialized_0x24": u32le(block, 0x24),
        "serial_hash16_0x28": block[0x28:0x38].hex(),
        "counter_0x38": u32le(block, 0x38),
        "status_0x3c": u32le(block, 0x3C),
        "payload_hex": payload.hex(),
        "first_0x40_hex": block[:0x40].hex(),
    }


def summarize_image(img: ReserveImage) -> str:
    lbas = known_lbas(img)
    lines: list[str] = []
    lines.append(f"# {img.path}")
    lines.append(f"size: 0x{len(img.data):x} ({len(img.data)} bytes)")
    lines.append(f"sha256: {sha256(img.data)}")
    lines.append(f"block_size: 0x{BLOCK_SIZE:x}")
    lines.append(f"block_count: {img.block_count}")
    lines.append(f"last_block: {img.last_block}")
    lines.append("")
    lines.append("## Known LBAs")
    for name, lba in sorted(lbas.items(), key=lambda kv: kv[1]):
        block = img.block(lba)
        lines.append(
            f"- {name}: LBA {lba} offset 0x{lba * BLOCK_SIZE:x} "
            f"nonzero={nonzero_count(block)} sha256={sha256(block)}"
        )

    for token_name in ("token_fastboot_unlock_data", "tail_token_fastboot_unlock_data"):
        if token_name not in lbas:
            continue
        token_lba = lbas[token_name]
        token = parse_token(img.block(token_lba))
        lines.append("")
        lines.append(f"## Fastboot token block ({token_name}): LBA {token_lba} offset 0x{token_lba * BLOCK_SIZE:x}")
        for key in (
            "sha256",
            "nonzero_full",
            "nonzero_first_0x140",
            "is_all_zero",
            "serial_0x100",
            "marker_0x108",
            "permission_0x10c",
            "binding31_0x10d",
            "model_0x12c",
        ):
            lines.append(f"- {key}: {token[key]}")

    for rec_name in ("unlock_record", "tail_unlock_record"):
        if rec_name not in lbas:
            continue
        rec_lba = lbas[rec_name]
        rec = parse_unlock_record(img.block(rec_lba))
        lines.append("")
        lines.append(f"## UnlockRecord ({rec_name}): LBA {rec_lba} offset 0x{rec_lba * BLOCK_SIZE:x}")
        for key in (
            "block_sha256",
            "magic_le",
            "stored_hash",
            "computed_hash",
            "hash_valid",
            "version_or_initialized_0x24",
            "serial_hash16_0x28",
            "counter_0x38",
            "status_0x3c",
        ):
            lines.append(f"- {key}: {rec[key]}")

    serial_lba = lbas.get("oplus_serial_ufs")
    if serial_lba is not None:
        block = img.block(serial_lba)
        lines.append("")
        lines.append(f"## Oplus serial blob candidate: LBA {serial_lba} offset 0x{serial_lba * BLOCK_SIZE:x}")
        lines.append(f"- sha256: {sha256(block)}")
        lines.append(f"- nonzero: {nonzero_count(block)}")
        lines.append(f"- first_0x40_hex: {block[:0x40].hex()}")
        lines.append(f"- first_0x40_ascii: {ascii_clean(block[:0x40])}")

    return "\n".join(lines)


def diff_images(a: ReserveImage, b: ReserveImage, max_blocks: int = 256) -> str:
    lines: list[str] = []
    lines.append(f"# Diff: {a.path} -> {b.path}")
    lines.append(f"a_sha256: {sha256(a.data)}")
    lines.append(f"b_sha256: {sha256(b.data)}")
    lines.append(f"block_size: 0x{BLOCK_SIZE:x}")
    lines.append(f"a_size: 0x{len(a.data):x} ({a.block_count} blocks)")
    lines.append(f"b_size: 0x{len(b.data):x} ({b.block_count} blocks)")
    common_blocks = min(a.block_count, b.block_count)
    lines.append(f"common_blocks_compared: {common_blocks}")

    changed: list[int] = []
    for lba in range(common_blocks):
        if a.block(lba) != b.block(lba):
            changed.append(lba)

    lines.append(f"changed_blocks: {len(changed)}")
    lines.append("")
    lines.append("## Changed block summary")
    for lba in changed[:max_blocks]:
        ab = a.block(lba)
        bb = b.block(lba)
        first_diff = next((i for i, (x, y) in enumerate(zip(ab, bb)) if x != y), None)
        label = block_label(a, lba)
        suffix = f"  # {label}" if label else ""
        lines.append(
            f"- LBA {lba} offset 0x{lba * BLOCK_SIZE:x} first_diff=0x{first_diff:x} "
            f"a_nonzero={nonzero_count(ab)} b_nonzero={nonzero_count(bb)} "
            f"a_sha={sha256(ab)} b_sha={sha256(bb)}{suffix}"
        )
    if len(changed) > max_blocks:
        lines.append(f"- ... {len(changed) - max_blocks} more changed blocks omitted")

    lbas = {name: lba for name, lba in known_lbas(a).items() if b.has_lba(lba)}
    lines.append("")
    lines.append("## Known structure delta")
    for name in (
        "token_fastboot_unlock_data",
        "unlock_record",
        "uart_tail_flag",
        "oplus_charge_ufs",
        "oplus_serial_ufs",
        "bf_block",
        "ddr_control_tail",
        "tail_token_fastboot_unlock_data",
        "tail_unlock_record",
    ):
        if name not in lbas:
            lines.append(f"- {name}: not present in both images")
            continue
        lba = lbas[name]
        same = a.block(lba) == b.block(lba)
        lines.append(f"- {name}: LBA {lba} same={same}")

    lines.append("")
    lines.append("## Token parse")
    token_lba = lbas.get("token_fastboot_unlock_data")
    if token_lba is None:
        lines.append("- canonical token block is not present in both images")
    for label, img in (("a", a), ("b", b)):
        if token_lba is None:
            break
        token = parse_token(img.block(token_lba))
        lines.append(
            f"- {label}: zero={token['is_all_zero']} nonzero_first_0x140={token['nonzero_first_0x140']} "
            f"serial={token['serial_0x100']!r} marker={token['marker_0x108']!r} "
            f"permission={token['permission_0x10c']!r} binding31={token['binding31_0x10d']!r} "
            f"model={token['model_0x12c']!r} sha={token['sha256']}"
        )

    lines.append("")
    lines.append("## UnlockRecord parse")
    rec_lba = lbas.get("unlock_record")
    if rec_lba is None:
        lines.append("- canonical UnlockRecord block is not present in both images")
    for label, img in (("a", a), ("b", b)):
        if rec_lba is None:
            break
        rec = parse_unlock_record(img.block(rec_lba))
        lines.append(
            f"- {label}: magic={rec['magic_le']} hash_valid={rec['hash_valid']} "
            f"version={rec['version_or_initialized_0x24']} serial_hash16={rec['serial_hash16_0x28']} "
            f"counter={rec['counter_0x38']} status={rec['status_0x3c']} "
            f"stored_hash={rec['stored_hash']}"
        )

    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, nargs="?", help="single oplusreserve1 image to dump")
    parser.add_argument("--diff", nargs=2, type=Path, metavar=("A", "B"), help="diff two oplusreserve1 images")
    parser.add_argument("--max-blocks", type=int, default=256, help="maximum changed blocks to print in diff")
    args = parser.parse_args()

    if args.diff:
        print(diff_images(load_image(args.diff[0]), load_image(args.diff[1]), args.max_blocks))
        return
    if not args.image:
        parser.error("provide IMAGE or --diff A B")
    print(summarize_image(load_image(args.image)))


if __name__ == "__main__":
    main()

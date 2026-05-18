#!/usr/bin/env bash
# tests/host/077_gblp1_find_mode2_profile.sh — locate a 0x0010 entry.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/077
mkdir -p "$OUT"

# Build a GBLP1 container with one 0x0010 (mode2_profile) entry: a
# 120-byte profile payload. Mirrors the on-disk layout in
# tools/shared/gblp1.h (header 28, entry 48, payload 16-aligned, footer 8).
python3 - "$OUT/with_profile.bin" "$OUT/no_profile.bin" <<'PY'
import struct, sys, zlib, hashlib

def container(entry_type):
    profile = b"GM2P" + struct.pack("<HHIIII",1,0,0,0,0x40000,0x9A4)
    profile += bytes(96)
    assert len(profile) == 120
    hdr_size, ent_size, ftr = 28, 48, b"GBLP1END"
    pay_off = (hdr_size + ent_size + 15) & ~15
    total = pay_off + len(profile)
    total = (total + 15) & ~15
    total += len(ftr)
    buf = bytearray(total)
    buf[pay_off:pay_off+len(profile)] = profile
    buf[total-8:total] = ftr
    # entry
    ent = struct.pack("<HHIII", entry_type, 0, pay_off, len(profile), 0)
    ent += hashlib.sha256(profile).digest()
    buf[hdr_size:hdr_size+ent_size] = ent
    # header: magic,ver,hdrsize,flags,total,entry_count, then crc32[0..24)
    head = b"GBLP1\0\0\0" + struct.pack("<HHIII",1,28,1,total,1)
    buf[0:24] = head
    buf[24:28] = struct.pack("<I", zlib.crc32(bytes(buf[0:24])) & 0xffffffff)
    return bytes(buf)

open(sys.argv[1],"wb").write(container(0x0010))  # has mode2_profile
open(sys.argv[2],"wb").write(container(0x0001))  # cached_abl only
PY

# container WITH a 0x0010 entry -> status=0
"$H" find-mode2-profile "$OUT/with_profile.bin" | grep -q 'status=0' \
  || { echo "FAIL: 0x0010 entry not found"; exit 1; }

# container WITHOUT one -> non-zero (GBL_PAYLOAD_NO_MODE2_PROFILE)
"$H" find-mode2-profile "$OUT/no_profile.bin" | grep -q 'status=0' \
  && { echo "FAIL: missing 0x0010 reported as found"; exit 1; } || true

echo "PASS: 077 gblp1 find mode2_profile"

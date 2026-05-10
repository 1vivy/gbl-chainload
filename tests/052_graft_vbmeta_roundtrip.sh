#!/usr/bin/env bash
# 052_graft_vbmeta_roundtrip.sh — verify graft script round-trips correctly.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import os, sys, struct
tmp = sys.argv[1]
SIZE = 4096

stock = bytearray(SIZE)
for i in range(SIZE):
    stock[i] = 0xAA

vbm_off = 3000
vbm_size = 256
hdr = bytearray(256)
hdr[0:4] = b"AVB0"
struct.pack_into(">II", hdr, 4, 1, 1)
struct.pack_into(">QQ", hdr, 12, 64, 128)
struct.pack_into(">I",  hdr, 28, 1)
stock[vbm_off:vbm_off+vbm_size] = hdr

footer = bytearray(64)
footer[0:4] = b"AVBf"
struct.pack_into(">II", footer, 4, 1, 0)
struct.pack_into(">QQQ", footer, 12, SIZE, vbm_off, vbm_size)
stock[SIZE-64:] = footer

custom = bytearray(SIZE)
for i in range(SIZE):
    custom[i] = 0xCC

with open(os.path.join(tmp, "stock.img"),  "wb") as f: f.write(stock)
with open(os.path.join(tmp, "custom.img"), "wb") as f: f.write(custom)
print("synthetic images written")
PY

python3 scripts/graft-vbmeta.py \
  --partition recovery \
  --stock "$TMP/stock.img" \
  --custom "$TMP/custom.img" \
  --out "$TMP/grafted.img"

python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
stock   = open(f"{tmp}/stock.img",  "rb").read()
custom  = open(f"{tmp}/custom.img", "rb").read()
grafted = open(f"{tmp}/grafted.img","rb").read()
assert len(grafted) == len(stock), f"size mismatch {len(grafted)} != {len(stock)}"
assert grafted[:3000] == custom[:3000],  "above vbmeta: should be from custom"
assert grafted[3000:] == stock[3000:],   "vbmeta + footer: should be from stock"
print("roundtrip OK")
PY

echo "ok 052_graft_vbmeta_roundtrip"

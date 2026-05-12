#!/usr/bin/env bash
# 054_graft_vbmeta_from_stock_roundtrip.sh — verify graft-vbmeta-from-stock.py
# pastes the stock recovery's vbmeta bytes verbatim onto a custom image at
# the custom's natural offset (not stock's offset).
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. Build a fake "stock recovery.img" with a valid signed-looking vbmeta + footer.
#    We use a placeholder (random-but-fixed) signature/pubkey — the test only
#    checks that the bytes propagate verbatim.  Stock size = 32 KiB content + AVB.
python3 - "$TMP" <<'PY'
import os, struct, sys
tmp = sys.argv[1]

# Fake stock content (32 KiB of 0xAA).
stock_content_size = 32 * 1024
stock_content = bytes([0xAA]) * stock_content_size

# AvbHashDescriptor for stock (parent 16 + body 116 + name + digest=32 (using fake digest))
def be(v, n): return v.to_bytes(n, "big")
NAME = b"recovery"
STOCK_DIGEST = b"\xde" * 32                                    # placeholder stock digest
desc_unpad = (
    b"\x00\x00\x00\x00\x00\x00\x00\x08" +                     # image_size (= stock_content_size) — placeholder
    b"sha256" + b"\x00" * (32 - 6) +
    be(len(NAME), 4) + be(0, 4) + be(32, 4) +                # name_len, salt_len, digest_len
    be(0, 4) + b"\x00" * 60 +                                 # flags, reserved[60]
    NAME + STOCK_DIGEST
)
# Fix image_size:
desc_unpad = (
    be(stock_content_size, 8) +
    b"sha256" + b"\x00" * (32 - 6) +
    be(len(NAME), 4) + be(0, 4) + be(32, 4) +
    be(0, 4) + b"\x00" * 60 +
    NAME + STOCK_DIGEST
)
pad = (-len(desc_unpad)) & 7
desc = be(2, 8) + be(len(desc_unpad) + pad, 8) + desc_unpad + b"\x00" * pad
desc_size = len(desc)

# Aux: descriptor + 1032-byte pubkey blob (fake but valid SHAPE for RSA-4096)
pubkey_blob = be(4096, 4) + be(0xdeadbeef, 4) + b"\xaa"*512 + b"\xbb"*512
aux_unpad = desc + pubkey_blob
aux_pad = (-len(aux_unpad)) & 63
aux = aux_unpad + b"\x00" * aux_pad
aux_size = len(aux)

# Auth: 32 hash + 512 sig + pad to 64
auth_unpad = b"\xcc" * 32 + b"\xdd" * 512                    # placeholder hash + sig
auth_pad = (-len(auth_unpad)) & 63
auth = auth_unpad + b"\x00" * auth_pad
auth_size = len(auth)

# Header (256 bytes)
hdr = (
    b"AVB0" +
    be(1, 4) + be(0, 4) +                                     # required ver
    be(auth_size, 8) + be(aux_size, 8) +
    be(2, 4) +                                                # algo SHA256_RSA4096
    be(0, 8) + be(32, 8) +                                    # hash off+size
    be(32, 8) + be(512, 8) +                                  # sig off+size
    be(desc_size, 8) + be(1032, 8) +                          # pk off+size
    be(0, 8) + be(0, 8) +                                     # pkm off+size
    be(0, 8) + be(desc_size, 8) +                             # descriptors off+size
    be(0, 8) +                                                # rollback
    be(0, 4) + be(0, 4)                                       # flags + rb_loc
)
hdr += b"avbtool 1.3.0".ljust(48, b"\x00")
hdr += b"\x00" * 80
assert len(hdr) == 256

vbmeta = hdr + auth + aux
vbm_size = len(vbmeta)

# Footer (last 64 bytes)
stock_partition_size = 64 * 1024   # 64 KiB total
vbm_off = ((stock_content_size + 4095) & ~4095)
footer = b"AVBf" + be(1, 4) + be(0, 4) + be(stock_content_size, 8) + be(vbm_off, 8) + be(vbm_size, 8)
footer += b"\x00" * (64 - len(footer))

# Assemble
buf = bytearray(stock_partition_size)
buf[0:stock_content_size] = stock_content
buf[vbm_off:vbm_off+vbm_size] = vbmeta
buf[-64:] = footer

with open(os.path.join(tmp, "stock_recovery.img"), "wb") as f:
    f.write(buf)
print(f"fixture stock recovery.img: {stock_partition_size} B, vbmeta @ {vbm_off}, size {vbm_size}")
PY

# 2. Build a fake custom recovery (larger than stock, so the natural placement
#    differs from stock's vbmeta_offset).
python3 - "$TMP" <<'PY'
import os, sys
tmp = sys.argv[1]
custom_size = 48 * 1024              # bigger than stock's 32 KiB content
custom = bytes([0xCC]) * custom_size
with open(os.path.join(tmp, "custom.img"), "wb") as f: f.write(custom)
print(f"custom recovery: {custom_size} bytes")
PY

# 3. Graft.
python3 scripts/graft-vbmeta-from-stock.py \
  --partition recovery \
  --in "$TMP/custom.img" \
  --partition-size 65536 \
  --stock-recovery "$TMP/stock_recovery.img" \
  --out "$TMP/grafted.img"

# 4. Verify.
python3 - "$TMP" <<'PY'
import struct, sys
tmp = sys.argv[1]
stock   = open(f"{tmp}/stock_recovery.img", "rb").read()
custom  = open(f"{tmp}/custom.img", "rb").read()
grafted = open(f"{tmp}/grafted.img", "rb").read()

PSZ = 65536

# Re-extract stock vbmeta bytes for comparison.
s_footer = stock[-64:]
_, s_vbm_off, s_vbm_sz = struct.unpack(">QQQ", s_footer[12:36])
stock_vbm = stock[s_vbm_off:s_vbm_off+s_vbm_sz]

assert len(grafted) == PSZ
assert grafted[:len(custom)] == custom, "custom prefix corrupted"

g_footer = grafted[-64:]
assert g_footer[:4] == b"AVBf"
g_orig, g_off, g_sz = struct.unpack(">QQQ", g_footer[12:36])
# Custom is 48 KiB = 49152.  round_up(49152, 4096) = 49152.
assert g_orig == len(custom), f"footer.orig {g_orig} != custom {len(custom)}"
assert g_off == 49152,        f"footer.vbm_off {g_off} != 49152"
assert g_sz == s_vbm_sz,      f"footer.vbm_sz {g_sz} != stock's {s_vbm_sz}"
print(f"ok footer: orig={g_orig} off={g_off} sz={g_sz}")

# Bytes-for-bytes match with stock vbmeta.
assert grafted[g_off:g_off+g_sz] == stock_vbm, "grafted vbmeta != stock"
print("ok grafted vbmeta bytes == stock vbmeta byte-for-byte")

# Pad regions zero.
assert all(b == 0 for b in grafted[len(custom):g_off])
assert all(b == 0 for b in grafted[g_off+g_sz:PSZ-64])
print("ok zero padding regions clean")

# Crucially: grafted vbmeta is at DIFFERENT offset than stock had it.
# Stock placed at 32768 (= stock content end, 4K-aligned); we place at 49152.
assert g_off != s_vbm_off, f"placement should differ: stock={s_vbm_off}, ours={g_off}"
print(f"ok placement diff: stock had vbmeta @ {s_vbm_off}, we placed @ {g_off}")
PY

# 5. Error path: too-small partition.
set +e
python3 scripts/graft-vbmeta-from-stock.py \
  --partition recovery \
  --in "$TMP/custom.img" \
  --partition-size 4096 \
  --stock-recovery "$TMP/stock_recovery.img" \
  --out "$TMP/should_not_exist.img" 2>"$TMP/err.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error for too-small partition"
  exit 1
fi
grep -q "partition_size" "$TMP/err.log" || {
  echo "FAIL: error log should mention partition_size"; cat "$TMP/err.log"; exit 1; }
echo "ok error: too-small partition rejected"

# 6. Error path: missing footer magic in stock.
dd if=/dev/zero of="$TMP/bad_stock.img" bs=1 count=65536 status=none
set +e
python3 scripts/graft-vbmeta-from-stock.py \
  --partition recovery \
  --in "$TMP/custom.img" \
  --partition-size 65536 \
  --stock-recovery "$TMP/bad_stock.img" \
  --out "$TMP/should_not_exist2.img" 2>"$TMP/err2.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error for stock without footer"
  exit 1
fi
grep -q "AvbFooter" "$TMP/err2.log" || {
  echo "FAIL: error log should mention AvbFooter"; cat "$TMP/err2.log"; exit 1; }
echo "ok error: missing stock footer rejected"

echo "ok 054_graft_vbmeta_from_stock_roundtrip"

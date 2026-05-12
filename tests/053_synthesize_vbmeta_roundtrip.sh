#!/usr/bin/env bash
# 053_synthesize_vbmeta_roundtrip.sh — verify synthesize-vbmeta.py
# produces a well-formed AVB partition layout whose hash descriptor's
# expected_digest is genuinely SHA256(salt || input).
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. Synthesize a known input — deterministic, byte 0xCC repeated.
python3 - "$TMP" <<'PY'
import os, sys
tmp = sys.argv[1]
IMG_SIZE = 64 * 1024
img = bytes([0xCC]) * IMG_SIZE
with open(os.path.join(tmp, "input.img"), "wb") as f: f.write(img)
print(f"input image: {IMG_SIZE} bytes of 0xCC")
PY

# 2. Run synthesize: 128 KiB partition, partition_name = "recovery", no salt.
PARTITION_SIZE=131072
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size "$PARTITION_SIZE" \
  --out "$TMP/out_nosalt.img"

# 3. Verify the no-salt output structurally and digest-wise.
python3 - "$TMP" "$PARTITION_SIZE" <<'PY'
import hashlib, struct, sys
tmp = sys.argv[1]
PSZ = int(sys.argv[2])

with open(f"{tmp}/input.img", "rb") as f: input_data = f.read()
with open(f"{tmp}/out_nosalt.img", "rb") as f: out = f.read()

assert len(out) == PSZ, f"output size {len(out)} != partition_size {PSZ}"

# AvbFooter (last 64 bytes).
footer = out[-64:]
assert footer[:4] == b"AVBf", f"footer magic missing: {footer[:4]!r}"
f_major, f_minor = struct.unpack(">II", footer[4:12])
orig_size, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])
assert f_major == 1 and f_minor == 0, f"footer version {f_major}.{f_minor}"
assert orig_size == len(input_data), f"footer.original_image_size {orig_size} != {len(input_data)}"
assert vbm_off >= len(input_data), f"vbmeta starts inside image: {vbm_off}"
assert vbm_off + vbm_size <= PSZ - 64, "vbmeta region overlaps footer"
assert vbm_off % 4096 == 0, f"vbmeta_offset not 4K-aligned: {vbm_off}"
print(f"ok AvbFooter: image_size={orig_size} vbm_off={vbm_off} vbm_size={vbm_size}")

# Bytes between input end and vbmeta start must be zero padding.
pad = out[len(input_data):vbm_off]
assert all(b == 0 for b in pad), f"padding {len(pad)}B should be zero"
print(f"ok padding zero: {len(pad)} bytes between image end and vbmeta")

# AvbVBMetaImageHeader (256 bytes at vbm_off).
hdr = out[vbm_off:vbm_off + 256]
assert hdr[:4] == b"AVB0", f"vbmeta magic missing: {hdr[:4]!r}"
h_major, h_minor = struct.unpack(">II", hdr[4:12])
auth_sz, aux_sz = struct.unpack(">QQ", hdr[12:28])
algo, = struct.unpack(">I", hdr[28:32])
desc_off, desc_sz = struct.unpack(">QQ", hdr[0x60:0x70])
assert h_major == 1 and h_minor == 0, f"header version {h_major}.{h_minor}"
assert auth_sz == 0, f"auth_size {auth_sz} != 0 (must be unsigned)"
assert algo == 0,   f"algorithm_type {algo} != 0 (NONE)"
assert desc_off == 0 and desc_sz == aux_sz, "descriptors should fill aux block"
print(f"ok vbmeta header: avb={h_major}.{h_minor} algo={algo} auth=0 aux={aux_sz}")

# AvbHashDescriptor at vbm_off + 256.
desc = out[vbm_off + 256 : vbm_off + 256 + aux_sz]
tag, num_after = struct.unpack(">QQ", desc[:16])
assert tag == 2, f"descriptor tag {tag} != AVB_DESCRIPTOR_TAG_HASH(2)"
assert 16 + num_after == len(desc), \
    f"descriptor header says {num_after}B following + 16 header, got total {len(desc)}"

# AvbHashDescriptor fixed body is 116 bytes after the 16-byte parent header.
body = desc[16:16+116]
img_sz, = struct.unpack(">Q", body[0:8])
algo_field = body[8:40].rstrip(b"\x00")
name_len, salt_len, digest_len = struct.unpack(">III", body[40:52])
flags, = struct.unpack(">I", body[52:56])
# bytes 56..116 are reserved[60] — must be zero
assert body[56:116] == b"\x00" * 60, "reserved[60] not zero"
assert algo_field == b"sha256", f"hash_algorithm {algo_field!r}"
assert img_sz == len(input_data), f"descriptor.image_size {img_sz} != {len(input_data)}"
assert flags == 0
assert salt_len == 0, f"expected zero salt for default invocation, got {salt_len}"
assert digest_len == 32, f"digest_len {digest_len} != 32 (sha256)"

trailer = desc[16+116 : 16+116 + name_len + salt_len + digest_len]
partition_name = trailer[:name_len].decode("ascii")
salt           = trailer[name_len : name_len + salt_len]
digest         = trailer[name_len + salt_len : name_len + salt_len + digest_len]
assert partition_name == "recovery", f"partition_name {partition_name!r}"
print(f"ok descriptor: partition={partition_name!r} image_size={img_sz} salt={len(salt)}B digest={digest.hex()}")

# Recompute the digest from input data and compare.
h = hashlib.sha256()
h.update(salt)
h.update(input_data)
expected = h.digest()
assert digest == expected, f"digest mismatch: got {digest.hex()} expected {expected.hex()}"
print("ok digest matches SHA256(salt||input)")

# Trailing bytes between vbmeta end and footer must be zero.
trail = out[vbm_off + vbm_size : PSZ - 64]
assert all(b == 0 for b in trail), f"trailing pad {len(trail)}B should be zero"
print(f"ok trailing pad zero: {len(trail)} bytes between vbmeta end and footer")

# And the input prefix must be untouched.
assert out[:len(input_data)] == input_data, "image prefix corrupted"
print("ok input image prefix preserved")
PY

# 4. Also exercise the salt path.
python3 scripts/synthesize-vbmeta.py \
  --partition dtbo \
  --in "$TMP/input.img" \
  --partition-size "$PARTITION_SIZE" \
  --salt "deadbeefcafef00d" \
  --out "$TMP/out_salt.img"

python3 - "$TMP" "$PARTITION_SIZE" <<'PY'
import hashlib, struct, sys
tmp = sys.argv[1]
PSZ = int(sys.argv[2])

with open(f"{tmp}/input.img", "rb") as f: input_data = f.read()
with open(f"{tmp}/out_salt.img", "rb") as f: out = f.read()
assert len(out) == PSZ

footer = out[-64:]
_, _, _ = struct.unpack(">II", footer[4:12]) + (0,)
_, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])
desc = out[vbm_off + 256 : vbm_off + 256 + vbm_size - 256]
body = desc[16:16+72]
name_len, salt_len, digest_len = struct.unpack(">III", body[40:52])
assert salt_len == 8, f"salt_len {salt_len} != 8 (expected from deadbeefcafef00d)"
trailer = desc[16+116 : 16+116 + name_len + salt_len + digest_len]
partition_name = trailer[:name_len].decode("ascii")
salt   = trailer[name_len : name_len + salt_len]
digest = trailer[name_len + salt_len : name_len + salt_len + digest_len]
assert partition_name == "dtbo", f"partition_name {partition_name!r}"
assert salt == bytes.fromhex("deadbeefcafef00d"), f"salt {salt.hex()}"

h = hashlib.sha256()
h.update(salt)
h.update(input_data)
expected = h.digest()
assert digest == expected, f"salted digest mismatch"
print(f"ok salt path: partition=dtbo salt={salt.hex()} digest matches SHA256(salt||input)")
PY

# 5. Error path: partition size too small.
set +e
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size 8192 \
  --out "$TMP/should_not_exist.img" 2>"$TMP/err.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error when partition_size < image_size"
  exit 1
fi
if ! grep -q "partition_size" "$TMP/err.log"; then
  echo "FAIL: error message should mention partition_size"
  cat "$TMP/err.log"
  exit 1
fi
echo "ok error path: too-small partition_size rejected"

# 6. Error path: invalid hex in --salt.
set +e
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size "$PARTITION_SIZE" \
  --salt "not-hex-at-all" \
  --out "$TMP/should_not_exist2.img" 2>"$TMP/err_hex.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error for invalid --salt hex"
  exit 1
fi
if ! grep -q "valid hex" "$TMP/err_hex.log"; then
  echo "FAIL: error message should mention salt hex validity"
  cat "$TMP/err_hex.log"
  exit 1
fi
echo "ok error path: invalid --salt hex rejected cleanly (no traceback)"

echo "ok 053_synthesize_vbmeta_roundtrip"

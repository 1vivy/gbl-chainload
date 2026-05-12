#!/usr/bin/env bash
# 053_synthesize_vbmeta_roundtrip.sh — verify synthesize-vbmeta.py
# produces a well-formed AVB partition layout for both modes:
#   default: fake-signed SHA256_RSA2048 → SIGNATURE_MISMATCH at libavb
#   --unsigned: algorithm_type=NONE → OK_NOT_SIGNED at libavb
# In both cases the hash descriptor's expected_digest is the SHA256 of
# (salt || input).
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

PARTITION_SIZE=131072

# ---------------------------------------------------------------------------
# 2. Default invocation: fake-signed SHA256_RSA2048.
# ---------------------------------------------------------------------------
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size "$PARTITION_SIZE" \
  --out "$TMP/out_signed.img"

# Verify the signed output structurally and digest-wise.
python3 - "$TMP" "$PARTITION_SIZE" <<'PY'
import hashlib, struct, sys
tmp = sys.argv[1]
PSZ = int(sys.argv[2])

with open(f"{tmp}/input.img", "rb") as f: input_data = f.read()
with open(f"{tmp}/out_signed.img", "rb") as f: out = f.read()

assert len(out) == PSZ, f"output size {len(out)} != partition_size {PSZ}"

# AvbFooter (last 64 bytes).
footer = out[-64:]
assert footer[:4] == b"AVBf", f"footer magic missing: {footer[:4]!r}"
f_major, f_minor = struct.unpack(">II", footer[4:12])
orig_size, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])
assert f_major == 1 and f_minor == 0, f"footer version {f_major}.{f_minor}"
assert orig_size == len(input_data), f"footer.original_image_size {orig_size} != {len(input_data)}"
assert vbm_off + vbm_size <= PSZ - 64, "vbmeta region overlaps footer"
assert vbm_off % 4096 == 0, f"vbmeta_offset not 4K-aligned: {vbm_off}"
print(f"ok AvbFooter: image_size={orig_size} vbm_off={vbm_off} vbm_size={vbm_size}")

pad = out[len(input_data):vbm_off]
assert all(b == 0 for b in pad), f"padding {len(pad)}B should be zero"

# AvbVBMetaImageHeader (256 bytes at vbm_off).
hdr = out[vbm_off:vbm_off + 256]
assert hdr[:4] == b"AVB0", f"vbmeta magic missing: {hdr[:4]!r}"
h_major, h_minor = struct.unpack(">II", hdr[4:12])
auth_sz, aux_sz = struct.unpack(">QQ", hdr[12:28])
algo, = struct.unpack(">I", hdr[28:32])
hash_off, hash_sz = struct.unpack(">QQ", hdr[0x20:0x30])
sig_off,  sig_sz  = struct.unpack(">QQ", hdr[0x30:0x40])
pk_off,   pk_sz   = struct.unpack(">QQ", hdr[0x40:0x50])
desc_off, desc_sz = struct.unpack(">QQ", hdr[0x60:0x70])

assert h_major == 1 and h_minor == 0, f"header version {h_major}.{h_minor}"
# Signed-junk requirements that make libavb hit ERROR_VERIFICATION (vs reject
# the structure with INVALID_VBMETA_HEADER).
assert algo == 1,    f"algorithm_type {algo} != 1 (SHA256_RSA2048)"
assert auth_sz % 64 == 0, f"auth size {auth_sz} not 64-aligned"
assert aux_sz  % 64 == 0, f"aux size {aux_sz} not 64-aligned"
assert hash_off == 0 and hash_sz == 32, f"hash region {hash_off},{hash_sz}"
assert sig_off == 32 and sig_sz == 256, f"sig region {sig_off},{sig_sz}"
assert pk_sz == 520, f"pubkey size {pk_sz} != 520 (RSA-2048)"
assert desc_off == 0, f"descriptors_offset {desc_off} != 0"
print(f"ok vbmeta header (signed): algo={algo} auth={auth_sz} aux={aux_sz} pk_sz={pk_sz}")

# Auth block at vbm_off + 256.
auth = out[vbm_off + 256 : vbm_off + 256 + auth_sz]
hash_field = auth[hash_off : hash_off + hash_sz]
sig_field  = auth[sig_off  : sig_off  + sig_sz]
assert sig_field == b"\x00" * sig_sz, "signature should be all zeros (junk)"

# Verify the hash field matches SHA256(header || aux_block).
aux_block = out[vbm_off + 256 + auth_sz : vbm_off + 256 + auth_sz + aux_sz]
computed = hashlib.sha256(hdr + aux_block).digest()
assert hash_field == computed, \
    f"auth.hash mismatch: file={hash_field.hex()} computed={computed.hex()}"
print(f"ok auth: hash = SHA256(header||aux), signature = zeros (will fail RSA verify)")

# Descriptor at start of aux.
desc = aux_block[0 : desc_sz]
tag, num_after = struct.unpack(">QQ", desc[:16])
assert tag == 2, f"descriptor tag {tag} != AVB_DESCRIPTOR_TAG_HASH(2)"
assert 16 + num_after == len(desc), \
    f"descriptor says {num_after}B following + 16 header, got {len(desc)}"
body = desc[16:16+116]
img_sz, = struct.unpack(">Q", body[0:8])
algo_field = body[8:40].rstrip(b"\x00")
name_len, salt_len, digest_len = struct.unpack(">III", body[40:52])
flags, = struct.unpack(">I", body[52:56])
assert body[56:116] == b"\x00" * 60, "reserved[60] not zero"
assert algo_field == b"sha256", f"hash_algorithm {algo_field!r}"
assert img_sz == len(input_data)
assert flags == 0
assert salt_len == 0
assert digest_len == 32

trailer = desc[16+116 : 16+116 + name_len + salt_len + digest_len]
partition_name = trailer[:name_len].decode("ascii")
digest = trailer[name_len + salt_len : name_len + salt_len + digest_len]
assert partition_name == "recovery", f"partition_name {partition_name!r}"

expected = hashlib.sha256(input_data).digest()
assert digest == expected, f"descriptor digest mismatch"
print(f"ok descriptor: partition={partition_name!r} digest matches SHA256(input)")

# Pubkey blob check.
pubkey = aux_block[pk_off : pk_off + pk_sz]
key_num_bits, n0inv = struct.unpack(">II", pubkey[:8])
N  = pubkey[8:8+256]
RR = pubkey[8+256:8+512]
assert key_num_bits == 2048, f"key_num_bits {key_num_bits} != 2048"
assert N == b"\xff" * 256, "modulus should be all-0xFF (junk)"
assert RR == b"\x00" * 256, "RR should be zero (junk)"
print(f"ok pubkey blob: key_num_bits=2048, N=ff*256, RR=00*256 (junk)")

assert out[:len(input_data)] == input_data, "image prefix corrupted"
print("ok signed-junk path complete")
PY

# ---------------------------------------------------------------------------
# 3. --unsigned: algorithm_type = NONE.
# ---------------------------------------------------------------------------
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size "$PARTITION_SIZE" \
  --unsigned \
  --out "$TMP/out_unsigned.img"

python3 - "$TMP" "$PARTITION_SIZE" <<'PY'
import hashlib, struct, sys
tmp = sys.argv[1]
PSZ = int(sys.argv[2])

with open(f"{tmp}/input.img", "rb") as f: input_data = f.read()
with open(f"{tmp}/out_unsigned.img", "rb") as f: out = f.read()

footer = out[-64:]
_, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])

hdr = out[vbm_off:vbm_off + 256]
assert hdr[:4] == b"AVB0"
auth_sz, aux_sz = struct.unpack(">QQ", hdr[12:28])
algo, = struct.unpack(">I", hdr[28:32])
pk_off, pk_sz = struct.unpack(">QQ", hdr[0x40:0x50])
desc_off, desc_sz = struct.unpack(">QQ", hdr[0x60:0x70])

assert algo == 0, f"--unsigned: algorithm_type {algo} != 0 (NONE)"
assert auth_sz == 0, f"--unsigned: auth_sz {auth_sz} != 0"
assert aux_sz % 64 == 0, f"--unsigned: aux_sz {aux_sz} not 64-aligned"
assert pk_sz == 0, f"--unsigned: pk_sz {pk_sz} != 0"
assert desc_off == 0
print(f"ok unsigned vbmeta: algo=0 auth=0 aux={aux_sz} pk=0")

# Descriptor still present and digest still matches.
aux_block = out[vbm_off + 256 : vbm_off + 256 + aux_sz]
desc = aux_block[0:desc_sz]
tag, num_after = struct.unpack(">QQ", desc[:16])
assert tag == 2
body = desc[16:16+116]
name_len, salt_len, digest_len = struct.unpack(">III", body[40:52])
trailer = desc[16+116 : 16+116 + name_len + salt_len + digest_len]
partition_name = trailer[:name_len].decode("ascii")
digest = trailer[name_len + salt_len : name_len + salt_len + digest_len]
expected = hashlib.sha256(input_data).digest()
assert digest == expected, "unsigned: descriptor digest mismatch"
print(f"ok --unsigned descriptor: partition={partition_name!r} digest matches")
PY

# ---------------------------------------------------------------------------
# 4. Salt path (default signed mode + --salt).
# ---------------------------------------------------------------------------
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
_, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])

hdr = out[vbm_off:vbm_off + 256]
auth_sz, aux_sz = struct.unpack(">QQ", hdr[12:28])
algo, = struct.unpack(">I", hdr[28:32])
desc_sz, = struct.unpack(">Q", hdr[0x68:0x70])
assert algo == 1, "salt path should still be fake-signed by default"

aux_block = out[vbm_off + 256 + auth_sz : vbm_off + 256 + auth_sz + aux_sz]
desc = aux_block[0:desc_sz]
body = desc[16:16+116]
name_len, salt_len, digest_len = struct.unpack(">III", body[40:52])
assert salt_len == 8, f"salt_len {salt_len} != 8"
trailer = desc[16+116 : 16+116 + name_len + salt_len + digest_len]
partition_name = trailer[:name_len].decode("ascii")
salt   = trailer[name_len : name_len + salt_len]
digest = trailer[name_len + salt_len : name_len + salt_len + digest_len]
assert partition_name == "dtbo"
assert salt == bytes.fromhex("deadbeefcafef00d")

h = hashlib.sha256()
h.update(salt)
h.update(input_data)
expected = h.digest()
assert digest == expected, "salted digest mismatch"
print(f"ok salt path: partition=dtbo salt={salt.hex()} digest matches SHA256(salt||input)")
PY

# ---------------------------------------------------------------------------
# 5. Error path: partition size too small.
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# 6. Error path: invalid hex in --salt.
# ---------------------------------------------------------------------------
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

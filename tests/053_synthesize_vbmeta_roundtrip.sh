#!/usr/bin/env bash
# 053_synthesize_vbmeta_roundtrip.sh — verify synthesize-vbmeta.py
# produces a stock-shaped fake-signed vbmeta whose hash descriptor's
# expected_digest is SHA256(salt || input) and whose embedded pubkey is
# the OEM pubkey extracted from stock vbmeta.img.
#
# Test fixture builds a stock-like vbmeta.img on the fly that contains
# RSA-2048 and RSA-4096 chain descriptors so we can exercise both
# key-size branches of synthesize without depending on a real OEM image.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1. Synthesize a known input.
python3 - "$TMP" <<'PY'
import os, sys
tmp = sys.argv[1]
img = bytes([0xCC]) * (64 * 1024)
with open(os.path.join(tmp, "input.img"), "wb") as f: f.write(img)
print(f"input image: {len(img)} bytes of 0xCC")
PY

# 2. Build a fixture stock-like vbmeta.img with chain descriptors for two
#    partitions: "recovery" using RSA-4096, "dtbo" using RSA-2048.
python3 - "$TMP" <<'PY'
import os, struct, sys
tmp = sys.argv[1]

def chain_desc(name, pk_len, key_num_bits, rb_loc):
    name_b = name.encode("ascii")
    # AvbRSAPublicKey: key_num_bits(u32 BE) + n0inv(u32 BE) + N + RR
    bits = key_num_bits
    pubkey = (
        struct.pack(">I", bits) + struct.pack(">I", 0xDEADBEEF) +
        b"\xab" * (bits // 8) + b"\xcd" * (bits // 8)
    )
    assert len(pubkey) == pk_len
    # body: rb_loc(u32)+name_len(u32)+pk_len(u32)+flags(u8)+reserved[3]+reserved2[60]=76, then name+pk
    body = struct.pack(">III", rb_loc, len(name_b), pk_len) + b"\x00" * 4 + b"\x00" * 60
    body += name_b + pubkey
    pad = (-len(body)) & 7
    body += b"\x00" * pad
    # parent: tag(u64 BE) + num_bytes_following(u64 BE)
    return struct.pack(">QQ", 4, len(body)) + body

aux_block = chain_desc("recovery", 1032, 4096, 1) + chain_desc("dtbo", 520, 2048, 3)
aux_size = ((len(aux_block) + 63) & ~63)
aux_block += b"\x00" * (aux_size - len(aux_block))

# Build header: unsigned (algo=0, auth=0) — we only need the descriptor area
# parseable for extract_chain_pubkey.
hdr = (
    b"AVB0" +
    struct.pack(">II", 1, 0) +              # required ver
    struct.pack(">Q", 0) +                  # auth_size
    struct.pack(">Q", aux_size) +           # aux_size
    struct.pack(">I", 0) +                  # algo NONE
    struct.pack(">Q", 0) * 8 +              # hash/sig/pk/pkm off+sizes
    struct.pack(">Q", 0) +                  # desc_off
    struct.pack(">Q", aux_size) +           # desc_size
    struct.pack(">Q", 0) +                  # rollback
    struct.pack(">I", 0) +                  # flags
    struct.pack(">I", 0)                    # rb_loc
)
hdr += b"\x00" * (256 - len(hdr))
assert len(hdr) == 256
with open(os.path.join(tmp, "stock_vbmeta.img"), "wb") as f:
    f.write(hdr + aux_block)
print(f"fixture vbmeta.img: {256 + aux_size} B; chain[recovery]=RSA-4096 chain[dtbo]=RSA-2048")
PY

# Helper that re-parses synthesize output and checks invariants for a given
# partition name + expected algorithm/sizes.
verify_py='
import hashlib, struct, sys
tmp, out_name, part_name, want_algo, want_pk_size, want_sig_size = sys.argv[1:7]
want_algo, want_pk_size, want_sig_size = int(want_algo), int(want_pk_size), int(want_sig_size)
PSZ = int(sys.argv[7])

with open(f"{tmp}/input.img", "rb") as f: input_data = f.read()
with open(f"{tmp}/{out_name}", "rb") as f: out = f.read()
assert len(out) == PSZ, f"output size {len(out)} != partition_size {PSZ}"

footer = out[-64:]
assert footer[:4] == b"AVBf", f"footer magic: {footer[:4]!r}"
_, vbm_off, vbm_sz = struct.unpack(">QQQ", footer[12:36])
assert vbm_off % 4096 == 0
assert vbm_off + vbm_sz <= PSZ - 64

hdr = out[vbm_off:vbm_off + 256]
assert hdr[:4] == b"AVB0"
auth_sz, aux_sz = struct.unpack(">QQ", hdr[12:28])
algo, = struct.unpack(">I", hdr[28:32])
hash_off, hash_sz = struct.unpack(">QQ", hdr[0x20:0x30])
sig_off, sig_sz   = struct.unpack(">QQ", hdr[0x30:0x40])
pk_off,  pk_sz    = struct.unpack(">QQ", hdr[0x40:0x50])
desc_off, desc_sz = struct.unpack(">QQ", hdr[0x60:0x70])

assert algo == want_algo, f"algo {algo} != {want_algo}"
assert hash_sz == 32 and sig_sz == want_sig_size and pk_sz == want_pk_size, \
    f"hash={hash_sz} sig={sig_sz} pk={pk_sz} (want {want_sig_size} sig, {want_pk_size} pk)"
assert auth_sz % 64 == 0 and aux_sz % 64 == 0, f"sizes not 64-aligned"
assert (32 + want_sig_size) <= auth_sz <= (32 + want_sig_size + 63), "auth size off"
assert desc_off == 0, f"desc_off {desc_off}"

# Auth block: real hash + zero signature.
auth = out[vbm_off + 256 : vbm_off + 256 + auth_sz]
hash_field = auth[0:32]
sig_field  = auth[32:32 + want_sig_size]
assert sig_field == b"\x00" * want_sig_size, "signature must be zero"

aux_block = out[vbm_off + 256 + auth_sz : vbm_off + 256 + auth_sz + aux_sz]
computed = hashlib.sha256(hdr + aux_block).digest()
assert hash_field == computed, "auth.hash != SHA256(header||aux)"

# Descriptor at start of aux.
desc = aux_block[0:desc_sz]
tag, num_after = struct.unpack(">QQ", desc[:16])
assert tag == 2, f"descriptor tag {tag} != HASH(2)"
body = desc[16:16+116]
img_sz, = struct.unpack(">Q", body[0:8])
name_len, salt_len, dig_len = struct.unpack(">III", body[40:52])
assert dig_len == 32
trailer = desc[16+116 : 16+116 + name_len + salt_len + dig_len]
name = trailer[:name_len].decode("ascii")
salt = trailer[name_len:name_len+salt_len]
digest = trailer[name_len+salt_len : name_len+salt_len+dig_len]
assert name == part_name, f"partition_name {name!r}"

# Verify descriptor digest matches SHA256(salt||input).
h = hashlib.sha256(); h.update(salt); h.update(input_data)
assert digest == h.digest(), "descriptor digest mismatch"

# Pubkey bytes: should be the fixture pubkey we constructed above.
pubkey = aux_block[pk_off : pk_off + pk_sz]
k_bits, n0inv = struct.unpack(">II", pubkey[:8])
assert k_bits in (2048, 4096, 8192), f"key_num_bits {k_bits}"
assert n0inv == 0xDEADBEEF, f"n0inv 0x{n0inv:x}"
N = pubkey[8:8 + k_bits//8]
RR = pubkey[8 + k_bits//8 : 8 + 2*(k_bits//8)]
assert N == b"\xab" * (k_bits//8), "pubkey N bytes != fixture"
assert RR == b"\xcd" * (k_bits//8), "pubkey RR bytes != fixture"

print(f"ok {part_name}: algo={algo} auth={auth_sz} aux={aux_sz} pk={pk_sz}, descriptor digest matches, pubkey matches fixture")
'

# 3. Recovery → RSA-4096 path.
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size 131072 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --out "$TMP/out_recovery.img"
python3 -c "$verify_py" "$TMP" out_recovery.img recovery 2 1032 512 131072

# 4. dtbo → RSA-2048 path.
python3 scripts/synthesize-vbmeta.py \
  --partition dtbo \
  --in "$TMP/input.img" \
  --partition-size 131072 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --out "$TMP/out_dtbo.img"
python3 -c "$verify_py" "$TMP" out_dtbo.img dtbo 1 520 256 131072

# 5. Salt path.
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size 131072 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --salt "deadbeefcafef00d" \
  --out "$TMP/out_salt.img"
python3 -c "$verify_py" "$TMP" out_salt.img recovery 2 1032 512 131072
echo "ok salt path (recovery + 8B salt)"

# 6. Error: partition not present in the supplied vbmeta.
set +e
python3 scripts/synthesize-vbmeta.py \
  --partition not_a_real_part \
  --in "$TMP/input.img" \
  --partition-size 131072 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --out "$TMP/should_not_exist.img" 2>"$TMP/err_missing.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error when partition not in vbmeta"
  exit 1
fi
grep -q "no chain partition descriptor" "$TMP/err_missing.log" || {
  echo "FAIL: error log missing 'no chain partition descriptor':"
  cat "$TMP/err_missing.log"
  exit 1
}
echo "ok error: missing chain descriptor rejected cleanly"

# 7. Error: too-small partition.
set +e
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size 8192 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --out "$TMP/too_small.img" 2>"$TMP/err_size.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error when partition_size < image"
  exit 1
fi
grep -q "partition_size" "$TMP/err_size.log" || {
  echo "FAIL: error log missing 'partition_size'"
  cat "$TMP/err_size.log"
  exit 1
}
echo "ok error: too-small partition rejected"

# 8. Error: invalid hex in --salt.
set +e
python3 scripts/synthesize-vbmeta.py \
  --partition recovery \
  --in "$TMP/input.img" \
  --partition-size 131072 \
  --vbmeta "$TMP/stock_vbmeta.img" \
  --salt "not-hex-at-all" \
  --out "$TMP/bad_salt.img" 2>"$TMP/err_hex.log"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
  echo "FAIL: expected error for invalid --salt hex"
  exit 1
fi
grep -q "valid hex" "$TMP/err_hex.log" || {
  echo "FAIL: error log missing 'valid hex'"
  cat "$TMP/err_hex.log"
  exit 1
}
echo "ok error: invalid --salt hex rejected"

echo "ok 053_synthesize_vbmeta_roundtrip"

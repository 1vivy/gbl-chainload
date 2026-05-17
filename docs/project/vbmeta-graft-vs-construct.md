# vbmeta: graft vs. construct — a cryptographic reference

This document answers one question rigorously:

> Starting from only the **stock `vbmeta.img`** and other publicly
> available material — i.e. **without the OEM private key** — can we
> *construct* a modified vbmeta (or per-partition AVB metadata) that
> AOSP `init` / `libavb` will accept?

Short answer: **no.** Not by descriptor surgery, not by forged-modulus
tricks, not by flag flips. The graft (substituting whole, already-signed
OEM bytes) is not a convenience — it is the **only** path that survives
verification, and therefore a recovery-graft utility is cryptographically
unavoidable for the mode-1 fakelock use case. The reasoning is below, with
real byte layouts and real numbers from the repo fixture
`images/grafted-recovery.img`.

A note on terminology, because the user's phrasing ("vbmeta footer")
conflates two distinct structures. There are **two** AVB structures and
they are not the same thing:

1. The **`AvbVBMetaImageHeader`** — the 256-byte struct at the *start* of
   the signed vbmeta blob (header + authentication block + auxiliary
   block). This is what carries the signature, the public key, and the
   descriptors.
2. The **`AvbFooter`** — a separate 64-byte trailer at the *very end* of a
   chained partition (boot/recovery/system…). It is **not signed**; it is
   pure location metadata that tells a parser where the embedded vbmeta
   blob lives inside that partition.

Section 1 lays both out with field offsets so the distinction is concrete.

---

## 1. vbmeta structure — annotated byte layout

All multi-byte integers in AVB structures are **big-endian**. This is
confirmed in the repo parser: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`
reads every field through `AvbReadU32Be` / `AvbReadU64Be`.

The numbers below are not spec-canonical guesses — they are decoded from
the real fixture `images/grafted-recovery.img` (a 100 MiB custom recovery
partition with stock OEM recovery vbmeta grafted in). Where a field is
spec-defined but absent from the fixture, that is called out.

### 1a. The signed vbmeta blob — three contiguous regions

A vbmeta blob (whether the standalone `vbmeta.img` or the copy embedded in
a chained partition) is three regions laid out back-to-back:

```
+==============================================================+  offset 0
|  AvbVBMetaImageHeader            256 bytes  (fixed)           |
+==============================================================+  256
|  Authentication block            authentication_data_block_size |
|    - hash      (hash_size bytes)                              |
|    - signature (signature_size bytes)                         |
+==============================================================+  256 + auth
|  Auxiliary block                 auxiliary_data_block_size    |
|    - public key          (public_key_size bytes)             |
|    - public key metadata (public_key_metadata_size bytes)    |
|    - descriptors          (descriptors_size bytes)           |
+==============================================================+  256 + auth + aux  = end
```

For the fixture's grafted recovery vbmeta:

| Region                 | Size (bytes) | Notes                                  |
|------------------------|-------------:|----------------------------------------|
| `AvbVBMetaImageHeader`  |          256 | fixed                                  |
| Authentication block    |          576 | `authentication_data_block_size`       |
| Auxiliary block         |         1408 | `auxiliary_data_block_size`            |
| **Total vbmeta blob**   |     **2240** | `256 + 576 + 1408`; matches footer `vbmeta_size` |

### 1b. `AvbVBMetaImageHeader` — 256 bytes (`libavb/avb_vbmeta_image.h`)

Decoded from the fixture (vbmeta blob starts at file offset `39288832`):

| Offset | Size | Field                          | Fixture value           |
|-------:|-----:|--------------------------------|-------------------------|
| 0      | 4    | `magic`                        | `"AVB0"`                |
| 4      | 4    | `required_libavb_version_major`| `1`                     |
| 8      | 4    | `required_libavb_version_minor`| `0`                     |
| 12     | 8    | `authentication_data_block_size`| `576`                  |
| 20     | 8    | `auxiliary_data_block_size`    | `1408`                  |
| 28     | 4    | `algorithm_type`               | `2` (`SHA256_RSA4096`)  |
| 32     | 8    | `hash_offset`                  | `0`  (within auth blk)  |
| 40     | 8    | `hash_size`                    | `32` (SHA-256)          |
| 48     | 8    | `signature_offset`             | `32` (within auth blk)  |
| 56     | 8    | `signature_size`               | `512` (RSA-4096 = 4096/8)|
| 64     | 8    | `public_key_offset`            | `336` (within aux blk)  |
| 72     | 8    | `public_key_size`              | `1032`                  |
| 80     | 8    | `public_key_metadata_offset`   | `1368`                  |
| 88     | 8    | `public_key_metadata_size`     | `0`                     |
| 96     | 8    | `descriptors_offset`           | `0` (within aux blk)    |
| 104    | 8    | `descriptors_size`             | `336`                   |
| 112    | 8    | `rollback_index`               | `1`                     |
| 120    | 4    | `flags`                        | `0`                     |
| 124    | 4    | `rollback_index_location`      | `0`                     |
| 128    | 48   | `release_string`               | `"avbtool 1.3.0"`       |
| 176    | 80   | `reserved`                     | zero                    |

All offsets in the header are **relative to the start of their own
block**, not the start of the file. `signature_offset = 32` means "32
bytes into the authentication block", i.e. file offset
`39288832 + 256 + 32`.

### 1c. Authentication block — 576 bytes (hash + signature)

| Sub-offset (in auth blk) | Size | Content                                  |
|-------------------------:|-----:|------------------------------------------|
| 0                        | 32   | `hash`  — SHA-256 of `header ‖ aux block`|
| 32                       | 512  | `signature` — RSA-4096 PKCS#1 v1.5 over `hash` |
| 544                      | 32   | padding to `authentication_data_block_size` |

This is the only region that is *not itself hashed* (see §2). It carries
the proof, not the payload.

### 1d. Auxiliary block — 1408 bytes (key + descriptors)

| Sub-offset (in aux blk) | Size | Content                                |
|------------------------:|-----:|----------------------------------------|
| 0                       | 336  | descriptors (`descriptors_size`)       |
| 336                     | 1032 | `AvbRSAPublicKey` blob (`public_key_size`)|
| 1368                    | 0    | public key metadata (absent here)      |

The `AvbRSAPublicKey` blob (`libavb/avb_crypto.h`) at aux offset 336:

| Sub-offset | Size | Field            | Fixture value                       |
|-----------:|-----:|------------------|-------------------------------------|
| 0          | 4    | `key_num_bits`   | `4096`                              |
| 4          | 4    | `n0inv`          | `0x8d526509` (Montgomery constant)  |
| 8          | 512  | `n` (modulus)    | the 4096-bit RSA modulus, big-endian|
| 520        | 512  | `rr` (R² mod n)  | Montgomery precompute               |
| **Total**  | 1032 | = `8 + 512 + 512`|                                     |

**Read that table again.** The RSA modulus `n` — the *entire* public key —
sits at aux offsets 8..520. The auxiliary block is part of the signed
data (§2). This single fact is the crux of the whole document: see §3c.

The descriptors in this fixture, decoded:

```
desc tag=2 (AvbHashDescriptor)  num_bytes_following=176  total=192
   image_size=39288832  hash_algorithm="sha256"
   partition="recovery"  salt_len=16  digest_len=32  flags=0
desc tag=0 (AvbPropertyDescriptor)  num_bytes_following=128  total=144
```

So this is a **`AvbHashDescriptor`** for the `recovery` partition: a
whole-image hash (boot/recovery use hash, not hashtree). It pins
`recovery` to a 32-byte SHA-256 digest over exactly `39288832` bytes of
image content with a fixed 16-byte salt.

Relevant descriptor types (all in `libavb/`):

- `AvbHashDescriptor` (tag 2) — whole-partition hash; used for `boot`,
  `recovery`, `init_boot`, `vendor_boot`. Carries the expected digest.
- `AvbHashtreeDescriptor` (tag 1) — dm-verity hashtree root; used for
  large read-only filesystems (`system`, `vendor`, `product`).
- `AvbChainPartitionDescriptor` (tag 4) — delegates a partition to its
  *own* vbmeta footer, embedding the public key that the chained
  partition's vbmeta must be signed with.
- `AvbKernelCmdlineDescriptor` (tag 3), `AvbPropertyDescriptor` (tag 0).

### 1e. `AvbFooter` — 64 bytes (`libavb/avb_footer.h`)

This is the structure the user called "the vbmeta footer". It is **not**
the vbmeta header and it is **not signed**. It is a pure pointer record
written at the *last 64 bytes* of a chained partition, so a parser that
only knows the partition's total size can find the embedded vbmeta blob.

Decoded from the fixture (file is `104857600` bytes; footer at
`104857600 − 64 = 104857536`):

| Offset (in footer) | Size | Field                 | Fixture value      |
|-------------------:|-----:|-----------------------|--------------------|
| 0                  | 4    | `magic`               | `"AVBf"`           |
| 4                  | 4    | `version_major`       | `1`                |
| 8                  | 4    | `version_minor`       | `0`                |
| 12                 | 8    | `original_image_size` | `39288832`         |
| 20                 | 8    | `vbmeta_offset`       | `39288832`         |
| 28                 | 8    | `vbmeta_size`         | `2240`             |
| 36                 | 28   | `reserved`            | zero               |

`vbmeta_offset` (39288832) and `vbmeta_size` (2240) together say: "the
signed vbmeta blob is at byte 39288832, and is 2240 bytes long" — exactly
the `256 + 576 + 1408` from §1a. `original_image_size` says the actual
recovery payload occupies the first 39288832 bytes; everything after that
up to the footer is AVB metadata and zero padding.

The repo parser `AvbParse_Footer()` implements exactly this: read the last
64 bytes, check `"AVBf"`, pull the three `u64`s, sanity-check that
`vbmeta_offset + vbmeta_size <= PartitionSize`.

> **Fixture caveat (a known bug, preserved deliberately).** In this
> fixture `vbmeta_offset = 39288832`, which is *stock's* offset — the end
> of a ~39 MB stock recovery. The custom recovery (OrangeFox) here is
> ~67 MB of payload inside a 100 MiB partition. Writing stock vbmeta at
> 39288832 overwrites live OrangeFox bytes. Project memory
> `graft_at_natural_offset_wins.md` records the fix: place the vbmeta blob
> at `round_up(custom_content_size, 4 KiB)` instead, and write the footer
> to point there. The *graft mechanism* in §5 is correct; this particular
> fixture was captured before the offset fix.

---

## 2. What is signed, where the signature lives, where the key lives

### 2a. The exact signed range

For `algorithm_type != 0` (here `2 = SHA256_RSA4096`), libavb's
`avb_vbmeta_image_verify()` (`libavb/avb_vbmeta_image.c`) does this:

1. **Hash.** Compute `H = SHA-256( AvbVBMetaImageHeader ‖ Auxiliary block )`.
   That is the 256-byte header **plus** the entire auxiliary block —
   contiguous — fed to the digest. The **authentication block is excluded**
   (you cannot hash the signature into the thing the signature signs).
2. **Compare hash.** `H` must equal the 32 bytes stored at
   `authentication_block + hash_offset`. Mismatch → `AVB_VBMETA_VERIFY_RESULT_HASH_MISMATCH`.
3. **Verify signature.** RSA-verify the `signature_size`-byte blob at
   `authentication_block + signature_offset` against `H`, using the
   `AvbRSAPublicKey` found at `auxiliary_block + public_key_offset`.
   PKCS#1 v1.5, public exponent `e = 65537`. Mismatch →
   `AVB_VBMETA_VERIFY_RESULT_SIGNATURE_MISMATCH`.

Both must pass for `AVB_VBMETA_VERIFY_RESULT_OK`.

The critical structural fact, restated:

```
            +-----------------------+
   SIGNED → |  AvbVBMetaImageHeader  |  256 B   ┐
            +-----------------------+          │  H = SHA-256 of
 NOT SIGNED |  Authentication block  |  (skip)  │  these two
            |   hash + signature     |          │  regions only
            +-----------------------+          │
   SIGNED → |  Auxiliary block       |  ────────┘
            |   - descriptors        |
            |   - AvbRSAPublicKey  ← │  the public key is INSIDE
            |     (modulus n)        |  the hashed/signed range
            +-----------------------+
```

The signature covers the header and the aux block. The aux block contains
the public key. **The public key is inside the signed data.**

### 2b. The verification flow `init` / `libavb` runs

On a real boot (`bootloader → init`), AVB is checked at two layers, and
the recovery-graft problem lives at the second:

1. **Bootloader stage (ABL + libavb).** ABL calls `avb_slot_verify()`,
   which for each partition listed in the top-level vbmeta:
   a. Loads that partition's vbmeta (the `vbmeta` partition for the main
      blob; the embedded blob via `AvbFooter` for chained partitions).
   b. Runs `avb_vbmeta_image_verify()` (§2a) → records a per-image
      `verify_result`.
   c. Walks descriptors; for each `AvbHashDescriptor` re-hashes the named
      partition's content and compares to the descriptor digest; for each
      `AvbHashtreeDescriptor` sets up dm-verity parameters.
   d. Checks the top-level public key against the device's locked-state
      OEM key, and checks `rollback_index`.
2. **Userspace stage (first-stage `init` + `libavb` again).** AOSP
   first-stage init (`first_stage_mount.cpp` `InitAvbHandle()` →
   `fs_avb.cpp` `AvbHandle::Open()` → `avb_ops.cpp` `FsManagerAvbOps`)
   **re-reads the on-disk metadata and re-verifies it**. For `recovery`
   this means re-reading the `AvbFooter` and the embedded vbmeta blob from
   the live `recovery` partition and re-running the §2a checks plus the
   descriptor hash check.

This second layer is why mode-1 cannot fix custom recovery alone.
Mode-1's `patch10` (see `docs/project/re-findings.md`) forces libavb to
return success *inside ABL* — but it does nothing to the copy of libavb
that runs later in userspace `init`. Userspace re-reads the recovery
partition's AVB metadata from disk, and if that metadata does not verify,
normal Android boot breaks. The data on disk must be made satisfiable;
there is no in-ABL hook reachable from the userspace re-check.

---

## 3. The user's exact questions, answered head-on

### 3a. "Is it just that we don't have the private key, so copying is merely easier?"

**No. The graft is a hard cryptographic requirement, not a convenience.**

The signature in the authentication block is an RSA-4096 PKCS#1 v1.5
signature over `H = SHA-256(header ‖ aux)`. RSA signature verification is:

```
recovered = signature ^ e  mod n          (e = 65537, n = OEM modulus)
accept iff  recovered == PKCS1_v1.5_encode(H)
```

Producing a `signature` value that satisfies this for a chosen `H`
requires computing `signature = EM ^ d mod n`, where `d` is the **private
exponent**. `d` is derived from the prime factorisation of `n`. Recovering
`d` from the public `n` is the RSA problem — factoring a 4096-bit modulus,
which is computationally infeasible.

Therefore: **any change to even one byte inside the signed range (header
or aux block) changes `H`, which invalidates the existing signature.** To
make a verifying signature for the new `H` you must sign with `d`. You do
not have `d`. The OEM has `d`. End of story.

"Copying" (graft) is not "the easy version of constructing". Constructing
is *impossible without `d`*; copying is the *only* thing left.

### 3b. "Why can't we copy the vbmeta key?"

Because "the key" the user is thinking of is the **public** key, and
having the public key does not let you sign anything. This is the
asymmetry that the entire scheme is built on.

- The **public key** (`n`, `e`) verifies signatures. It is — literally —
  `1032` bytes inside the aux block of every vbmeta (§1d). It is not
  secret; it is published in the image. You already have it.
- The **private key** (`d`, or equivalently the primes `p, q`) *creates*
  signatures. It exists only on an OEM signing server / HSM. It is never
  in any image, any partition, or any OTA package. There is nothing on the
  device or in public material to "copy".

So "copy the vbmeta key" — if it means copy the public key — is something
you can do trivially and it buys you **nothing**: you can verify with it,
you cannot sign with it.

What the **graft** actually does is subtler and is the reason it works: it
does not copy "the key" in isolation. It copies the **entire signed unit
together** — header + authentication block + aux block, as one
indivisible 2240-byte blob. That blob already contains a *matching set*:
the OEM public key, the OEM signature, and the exact header+descriptors
those were computed over. Because all three travel together unmodified,
the relationship `verify(signature, H, pubkey) == OK` that the OEM
established at signing time is preserved byte-for-byte. You are not
re-deriving the relationship; you are *transporting an intact one*.

### 3c. The "constructed-verify" / forged-modulus idea — and exactly why it is blocked

This is the cleverest attack the project considered (PR #10 thread; see
project memory `avb_constructed_verify_blocked.md`). It deserves to be
spelled out fully, because it is *algebraically real* — and then shown to
be *structurally impossible here*.

**The algebra.** RSA verification accepts `signature` iff
`signature^e mod n == EM`, where `EM` is the PKCS#1 v1.5 encoded message:

```
EM(H) = 0x00 0x01 [0xFF × k] 0x00 [DigestInfo] [H]
```

`EM` is fully determined by the hash `H` (the DigestInfo prefix for
SHA-256 is a fixed constant). Now the trick: you do not have to *solve*
for a signature under a *given* `n`. You get to **choose `n`** — the
modulus is just bytes in the aux block. So choose:

```
n = EM + 1          (the modulus)
s = EM              (the signature you will write)
```

Then, with `e = 65537` (odd):

```
s^e mod n  =  EM^65537 mod (EM + 1)
           ≡  (−1)^65537   mod (EM + 1)      [since EM ≡ −1 (mod EM+1)]
           ≡  −1           mod (EM + 1)
           ≡  EM           mod (EM + 1)
           =  s^e mod n  == EM     ✓
```

So `s = EM` verifies under `n = EM + 1` **with no private key at all**.
libavb's RSA check would return OK. (`n` must be odd for RSA; if `H`'s low
byte makes `EM` odd, tweak a header byte and re-hash until `EM` is even so
`EM+1` is odd. That is cheap.)

**Why it is blocked here — the circular dependency.** The attack needs
three things to be simultaneously true:

```
(1)  H  = SHA-256( header ‖ aux )          ← H is the hash of the blob
(2)  n  = EM(H) + 1                        ← the forged modulus depends on H
(3)  n  lives inside the aux block         ← because AvbRSAPublicKey is in aux
```

Combine them. By (3), `n` is part of the bytes hashed in (1). So:

```
H        = SHA-256( header ‖ aux-containing-n )
n        = EM(H) + 1
⇒  H     = SHA-256( header ‖ aux-containing-(EM(H)+1) )
```

`H` is now defined in terms of itself. To place the forged modulus `n`
into the aux block, you change the aux block. Changing the aux block
changes `H` (it is hashed). Changing `H` changes `EM(H)`, which changes
the *required* value of `n`. You write the new `n`, which changes the aux
block again, which changes `H` again… The construction never converges.

Reaching a fixed point would require finding aux-block content whose
SHA-256 hash, after the `EM(·)+1` transform, reproduces the very modulus
bytes you wrote — i.e. a **SHA-256 (second-)preimage on a self-referential
constraint**. SHA-256 is preimage-resistant; this is infeasible.

Picture the dependency cycle:

```
        ┌────────────────────────────────────────────┐
        │                                            │
        ▼                                            │
   aux block bytes ──(hashed with header)──▶  H       │
        ▲                                     │       │
        │                                     ▼       │
   write n into aux  ◀──  n = EM(H) + 1  ◀────┘       │
        │                                            │
        └──────────── n IS aux bytes ────────────────┘
```

The cycle exists *only because the modulus is inside the signed region.*
If AVB had put the public key outside the hashed range, the forged-modulus
attack would work. It does not.

**Equivalent escapes, all ruled out** (from `avb_constructed_verify_blocked.md`):

- *Put `n` somewhere outside the aux block.* libavb only reads the modulus
  from `aux_block + public_key_offset`. The header's `reserved[80]` is
  never interpreted as a key. There is no out-of-band slot for `n`.
- *Use `flags & VERIFICATION_DISABLED` to skip the check.* The flag is
  only honored on the **top-level** vbmeta, not on chained partitions like
  `recovery`; and flipping it is itself a signed-byte edit (§4).
- *Carmichael universal forgery* (an `s` with `s^65537 ≡ s mod n` for all
  `s`): needs `λ(n) | 65536`, which forces `n` to be a product of distinct
  Fermat primes — the known Fermat primes cap a product at roughly `2^32`,
  nowhere near a 4096-bit `n`.
- *Zero the stored hash to skip step 2.* The hash comparison is an exact
  `memcmp`; a zero hash just fails `HASH_MISMATCH` immediately.

Every angle collapses back to the same wall: **the public key is inside
the signed data**, so you cannot adjust it to your advantage without
invalidating the thing it has to satisfy.

---

## 4. The flag-flip alternative — and why mode-1 can't use it

`AvbVBMetaImageHeader.flags` (header offset 120, §1b) defines two bits
(`libavb/avb_vbmeta_image.h`):

- `AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED` (bit 1)
- `AVB_VBMETA_IMAGE_FLAGS_HASHTREE_DISABLED` (bit 0)

This is what tools like AndroidKitchen / AK3's `PATCH_VBMETA_FLAG` flip
(see `docs/project/zip-methodology.md` §A7): a 1–2 byte edit on a vbmeta
image's header.

**But `flags` is at header offset 120 — inside the 256-byte header, which
is inside the SHA-256'd range (§2a).** Flipping a flag bit changes `H`,
which invalidates the signature exactly like any other signed-byte edit.
A flag-flipped vbmeta is therefore an **unsigned / signature-broken**
vbmeta.

Whether that is *honored* depends entirely on the device state:

| Device / boot state                        | Flag-flipped (unsigned) vbmeta honored? |
|---------------------------------------------|-----------------------------------------|
| Bootloader **unlocked**, AVB accepts unsigned vbmeta | Yes — unlocked AVB treats a bad signature as "orange state, boot anyway" and will honor `VERIFICATION_DISABLED`. |
| Bootloader **locked**                       | No — locked AVB requires a good signature chaining to the OEM key; a broken signature is a hard `RED` stop. |
| **mode-1 fakelock** (this project)          | No — see below. |

The mode-1 case is the subtle one. mode-1's entire purpose is to *present
a verified (green/locked) state* to ABL and Keymaster while running stock
images. It does this by hooking `QCOM_VERIFIEDBOOT_PROTOCOL` so that a
*cleanly verifying* stock image yields a locked/green report.

A flag-flipped vbmeta is the opposite of what mode-1 needs:

- If mode-1 just *disabled AVB*, there would be no verified-boot state to
  present — Keymaster's `SET_ROOT_OF_TRUST` would get an unverified /
  orange root of trust, attestation and `/data` (FBE — see project memory
  `infiniti_km_hidden_set_invariant.md`) would see an inconsistent state.
- mode-1 is trying to look *locked*. A locked device does **not** honor a
  flag-flipped unsigned vbmeta — it rejects it. So presenting one would be
  internally contradictory: claiming "locked + verified" while shipping
  metadata that a locked device would reject.

So flag-flipping is a real technique for an *honestly unlocked* device
that just wants verity off — but it is structurally unavailable to mode-1,
which must keep verification *passing*, not *disabled*. mode-1 needs the
metadata to genuinely verify against the OEM key, which loops straight
back to §3: you need OEM-signed bytes, i.e. a graft.

---

## 5. Why the graft works

The graft is **substitution, not modification**. It never edits a byte
inside a signed region. It takes the complete, already-OEM-signed stock
recovery vbmeta blob — header + authentication block + aux block, the full
2240 bytes in the fixture — and writes that blob, verbatim, onto the
custom recovery partition, then writes an `AvbFooter` pointing at it.

Because not one byte of the signed region is touched:

- `H = SHA-256(header ‖ aux)` is identical to what the OEM computed.
- The stored `signature` still verifies against the stored `H` under the
  stored OEM public key — the OEM established this relationship at signing
  time and it travels intact.
- `avb_vbmeta_image_verify()` (§2a) returns `AVB_VBMETA_VERIFY_RESULT_OK`
  for the blob itself.

The graft does **not** make the *whole partition* verify — the custom
recovery's actual content does not match the stock `AvbHashDescriptor`
digest, so the descriptor hash check still fails. Project memory
`graft_at_natural_offset_wins.md` records exactly this and why it is the
*right* outcome:

- **Synthesize** (zero out the RSA signature) → per-vbmeta result is
  `SIGNATURE_MISMATCH`. Recovery boot works, but **normal Android boot
  breaks** — userspace AVB keys behaviour off the per-image
  `verify_result` field, and `SIGNATURE_MISMATCH` propagates badly.
- **Graft** (real OEM bytes verbatim) → per-vbmeta result is `OK`, then a
  later `HASH_MISMATCH` at the descriptor walk → slot-level
  `ERROR_VERIFICATION`, which is recoverable and which mode-1's `patch10`
  catches cleanly. **Both normal boot and recovery boot work.**

The slot-level *class* is the same (`ERROR_VERIFICATION`) in both cases,
but the per-vbmeta `verify_result` differs — and that field propagates
into userspace. Only the graft yields `verify_result = OK` at the
image-verify stage, and that is what keeps normal boot alive.

**The natural offset.** The footer's `vbmeta_offset` must point at the
real location of the blob. Stock's footer says `39288832` (end of a
~39 MB stock recovery). A custom recovery is larger, so the blob is placed
at `round_up(custom_content_size, 4 KiB)` — past the end of the real
custom payload, 4 KiB-aligned — and the footer (always the last 64 bytes
of the partition) is written with `vbmeta_offset` set to that natural
offset. The parser then finds the blob exactly where the footer says
(`AvbParse_Footer` → read `vbmeta_offset` → `AvbParse_VbmetaHeader` at
that offset), the magic and size checks pass, and the embedded blob —
being untouched OEM bytes — passes image verification.

---

## 6. Conclusion — is there ANY construct path?

**No.** Enumerating every avenue considered:

| Avenue                          | Verdict | Why it fails                                                                 |
|---------------------------------|---------|------------------------------------------------------------------------------|
| Re-sign edited vbmeta           | ✗       | Needs the OEM private key `d`; recovering `d` from `n` = factoring RSA-4096.  |
| Copy "the key"                  | ✗       | Only the *public* key is available; it verifies, it cannot sign.             |
| Descriptor surgery (edit digest)| ✗       | Descriptors are in the aux block → inside the signed range → breaks `H`.      |
| Forged modulus `n = EM+1`       | ✗       | `n` is inside the aux block; the construction is self-referential → SHA-256 preimage. |
| Flag flip (`VERIFICATION_DISABLED`)| ✗    | `flags` is in the signed header → breaks the signature; and a *locked*/fakelock device rejects unsigned vbmeta anyway. |
| Hashtree-disable flag           | ✗       | Same — signed header bit; and `recovery` uses a hash, not a hashtree.        |
| Chained-partition pubkey swap   | ✗       | The chain descriptor's embedded pubkey is in the *parent* aux block → signed; the chained vbmeta still must be signed by *some* key the parent vouches for, which still needs that key's `d`. |
| Carmichael universal forgery    | ✗       | Requires `λ(n) \| 65536`; impossible for a 4096-bit `n`.                      |
| Zero the stored hash            | ✗       | Exact `memcmp` → immediate `HASH_MISMATCH`.                                   |
| **Graft whole OEM-signed blob** | ✓       | No signed byte changes; the OEM signature/key/hash relationship is preserved verbatim. |

**The cryptographic floor, stated plainly:** AVB binds the public key
*into* the signed payload. That single design choice means there is no
way to produce a vbmeta blob that `libavb` accepts unless you either
(a) hold the OEM private key, or (b) reuse an intact run of bytes the OEM
already signed. Public material — including the public key, which you can
read straight out of any image — does not let you construct a fresh
verifying blob. It is not a matter of effort or cleverness; the
forged-modulus analysis in §3c shows the one algebraically interesting
shortcut is closed by the key-in-signed-data layout.

**Downstream answer — is the graft (and a recovery-graft utility ZIP)
unavoidable?**

Yes, for the mode-1 use case. mode-1 must present a *verified* state, so
it cannot take the "disable AVB" exit that an honestly-unlocked device
could. The only way to give userspace `init`/`libavb` a recovery vbmeta
that returns `verify_result = OK` at the image-verify stage — which is
what keeps normal Android boot working — is to put genuinely OEM-signed
bytes on disk. No construct path produces those bytes. Therefore:

- The **graft is cryptographically mandatory**, not a shortcut.
- A **recovery-graft utility** (host script and/or the device-side module,
  ultimately delivered as the recovery-graft ZIP per
  `docs/project/next-milestone.md` §1) is the *minimum* tooling that can
  solve "custom recovery + mode-1 normal boot". No amount of bootloader-
  side hooking removes it, because the failing verification is a userspace
  re-read of on-disk metadata (§2b), and `patch10` only reaches the
  in-ABL libavb instance.

The only thing that would eliminate the graft is possession of the OEM
recovery-signing private key — which, by construction of AVB and of the
OEM's key custody, is exactly what an unlocked-device project does not and
will not have.

---

## Appendix — sources

- Real layout numbers: decoded from `images/grafted-recovery.img`
  (fixture; see §1 caveat about its pre-fix offset).
- Repo parser: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`,
  `GblChainloadPkg/Include/Library/AvbParseLib.h`, validated by
  `tests/avb/test_avbparse.c` (12 tests, `ALL PASS`).
- Prior conclusions: project memory
  `avb_constructed_verify_blocked.md`, `graft_at_natural_offset_wins.md`.
- Project docs: `docs/project/re-findings.md` (recovery normal-boot
  failure, `patch10`), `docs/project/decisions.md` (recovery fix path),
  `docs/project/zip-methodology.md` §A7 (flag-flip vs descriptor graft).
- libavb structures referenced by name: `AvbVBMetaImageHeader`
  (`avb_vbmeta_image.h`), `AvbFooter` (`avb_footer.h`), `AvbHashDescriptor`
  / `AvbHashtreeDescriptor` / `AvbChainPartitionDescriptor`
  (`avb_*_descriptor.h`), `AvbRSAPublicKey` (`avb_crypto.h`),
  `avb_vbmeta_image_verify()` (`avb_vbmeta_image.c`).
- AOSP userspace re-verify path: `first_stage_mount.cpp`
  (`InitAvbHandle()`), `fs_avb.cpp` (`AvbHandle::Open()`), `avb_ops.cpp`
  (`FsManagerAvbOps`).

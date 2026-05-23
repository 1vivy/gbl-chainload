# gbl-chainload host-side tools

Seven small C utilities (plus Python wrappers) for turning a dumped ABL
partition image into a ready-to-stage EFISP payload, then optionally writing
it to disk. Some workflows also consume additional inputs; for example, a
stock `vbmeta.img` is only needed for mode 2. Most of the C tools build
without extra library dependencies; `fv-unwrap` links `liblzma`.
`mode2-profile` ships as both a C binary and a pure-Python script — they
produce byte-identical output, so the C tool is the shippable build path and
`mode2-profile.py` is the dev iteration path. The off-device chain is also
wrapped by `scripts/efisp-package.py`, which calls these tools in order and
produces a single `installed.efi`; `scripts/vbmeta-graft.py` wraps the graft
tool for released host bundles.

## Host packaging workflow

Use `efisp-package.py` when you want to prepare an EFISP payload on a desktop
host instead of inside the recovery ZIP. Pick the mode first, gather the
required files for that mode, then run the package step and only use the
RAM-only fastboot stage path for device testing.

### Mode prep and required files

All modes require:

- a dumped stock ABL partition image (`--abl <abl.img>`), not just an already
  extracted PE;
- the matching base EFI (`--efi <mode-N.efi>`);
- the released host-tool binaries next to `efisp-package.py` in `bin/`, or a
  tool directory passed with `--bin-dir` / `--tools-dir`.

Examples below use bare tool names for readability. In a release bundle, either
run through `efisp-package.py`'s auto-discovery or prefix individual tools with
`./bin/` unless you added them to `PATH`.

Mode-specific inputs:

| Mode | Base EFI | Required extra files | What gets packed |
|---|---|---|---|
| 0 | `mode-0.efi` | none | cached ABL patched with `--no-mode1` |
| 1 | `mode-1.efi` | none | cached ABL with universal + `mode_1` patches |
| 2 | `mode-2.efi` | stock main `vbmeta.img` and OEM id (`--stock-vbmeta`, `--oem`) | cached ABL patched with OEM group + a compiled mode-2 profile |

For mode 2, `--stock-vbmeta` is the device's main vbmeta image used to derive
the 120-byte mode-2 profile. It is separate from any partition image used with
`vbmeta-graft`.

### efisp-package.py

Off-device chaining of `fv-unwrap → abl-patcher → gbl-pack` (plus
`mode2-profile derive`+`compile` in mode 2), concatenated with a base
`mode-N.efi` to produce a single ready-to-stage EFISP image. This is the
host-side equivalent of the install ZIP's on-device `build_payload`. The
script only *produces* the file — it does not write device storage.

```
efisp-package.py --abl <abl.img> --mode 0|1|2 --efi <mode-N.efi>
                 [--stock-vbmeta <vbmeta.img>] [--oem <id>]
                 [--bin-dir <dir>] [--out <path>]
```

Tool lookup order: `--bin-dir`/`--tools-dir` if given, then `bin/` adjacent to
`efisp-package.py` (release bundle layout), then a platform `dist/<os>/`
directory when running from a source checkout, then `PATH`.

Mode 0 (no `mode_1` patches; minimal payload):

```
python3 efisp-package.py \
  --abl  stock_abl.img \
  --mode 0 \
  --efi  mode-0.efi \
  --out  installed-mode0.efi
```

Mode 1 (universal + `mode_1` patches; cached ABL in the overlay):

```
python3 efisp-package.py \
  --abl  stock_abl.img \
  --mode 1 \
  --efi  mode-1.efi \
  --out  installed-mode1.efi
```

Mode 2 (no `mode_1`; mode-2 profile + OEM patch group):

```
python3 efisp-package.py \
  --abl          stock_abl.img \
  --mode         2 \
  --efi          mode-2.efi \
  --stock-vbmeta stock_vbmeta.img \
  --oem          oneplus \
  --out          installed-mode2.efi
```

Default output, when omitted, is
`dist/efisp-payload/<abl-basename>-mode<N>.efi` relative to the current working
directory; pass `--out` when using the release bundle outside the repo.

### Graft usage notes

`vbmeta-graft` is for partition-level AVB cohabitation, not for building the
EFISP overlay itself. Use it when a custom partition image needs its custom
vbmeta placed before the stock OEM vbmeta footer so the existing chain
descriptor can still validate the image.

`--part-size` is the final target partition size: the size of the image you
intend to write for that partition. The tool separately handles the
custom image payload size: if `--custom` is already AVB-footered, it uses that
footer's `OriginalImageSize`; otherwise it uses the custom file size.
For a full, partition-sized custom image, `--part-size` is usually that custom
image's file size. For a trimmed/bare payload, derive it from the real target
partition or another known-good full partition image.

The release bundle includes `vbmeta-graft.py`, a convenience wrapper that
auto-discovers `bin/vbmeta-graft` and defaults `--part-size` to the custom
image size:

```
python3 vbmeta-graft.py \
  --stock  stock_partition.img \
  --custom custom_partition.img \
  --out    grafted_partition.img
```

Use `--part-size <bytes>` or `--size-from <image-or-device>` with the wrapper
when the custom image is not the full destination-sized image.

Recommended host flow:

1. Inspect the stock partition or candidate image:

   ```
   vbmeta-graft list <stock-or-candidate-partition.img>
   vbmeta-graft list-hash <stock-or-candidate-partition.img>
   ```

2. Graft the stock footer from the stock partition below the custom image.
   `--part-size` must match the destination partition size; for a full custom
   partition image, use that file's size:

   ```
   vbmeta-graft graft \
     --stock     stock_partition.img \
     --custom    custom_partition.img \
     --part-size <target-partition-bytes> \
     --out       grafted_partition.img
   ```

3. Optionally verify against the device's main vbmeta chain descriptor:

   ```
   vbmeta-graft check grafted_partition.img vbmeta.img <partition-name>
   ```

The `graft` step needs the stock partition image and the custom image. The
optional `check` step additionally needs the device main `vbmeta.img` and the
partition name from the chain descriptor.

### Testing and device staging

Verify the packaged payload before booting it:

```
gblp1-inspect installed-mode1.efi
```

Device testing must use the RAM-only staging path:

```
fastboot stage installed-mode1.efi
fastboot oem boot-efi
```

Do not use host-tool docs as a license to flash firmware partitions. The safe
iteration loop is `fastboot stage` followed by `fastboot oem boot-efi`; it is a
one-shot boot path and survives a power cycle without persistent writes.

### Advanced host-tool usage

- Use `fv-unwrap` by itself to debug ABL/FV extraction failures before running
  the full package script.
- Use `abl-patcher --check-anchors-only --in <extracted.efi>` to validate patch
  anchor coverage without producing a patched output.
- Use `mode2-profile derive` to review the TOML profile derived from a stock
  `vbmeta.img`; use `compile` to turn the reviewed TOML into the binary profile
  consumed by `gbl-pack`.
- Use `gbl-pack` directly when combining a cached ABL and mode-2 profile by
  hand, or when constructing focused regression fixtures.
- Use `gblp1-inspect <image>` on either a bare `payload.bin` or a full
  base-EFI-plus-GBLP1 image to verify per-entry SHA-256 status.
- Use `gbl-commit` only for file/block-device write workflows you intentionally
  control; `efisp-package.py` itself does not call it and does not flash.

## Building

All builds happen inside a single docker image. Build it once:

```
docker build -t gbl-chainload-build:latest -f docker/Dockerfile .
```

- **Host (Linux dev binary):** `make -C tools/<tool>` → `tools/<tool>/<tool>`.
  The default target builds a native binary suitable for local testing on
  the Linux build host.
- **Android (aarch64, for the recovery ZIP):**
  `scripts/build-recovery-tools.sh` → `dist/recovery/<tool>` (statically
  linked, runs in TWRP / recovery shells).
- **Windows / macOS:** `scripts/build-cross-tools.sh windows|macos|all` →
  `dist/windows/<tool>.exe` and `dist/macos/<tool>` (universal
  `x86_64+arm64`). `SHA256SUMS` is emitted alongside the binaries.

## Tools

### fv-unwrap

Extracts the EFI PE32+ payload out of a dumped Qualcomm-style ABL/XBL
partition image. Walks the arm32-ELF wrapper, the EDK2 firmware volume,
the LZMA-compressed `EFI_SECTION_GUID_DEFINED` section, and nested PE32
sections; emits the inner PE.

```
fv-unwrap <partition.bin> <output.efi>
```

```
fv-unwrap stock_abl.img extracted.efi
```

### abl-patcher

Drives the same `DynamicPatchLib` code that runs on-device, but against a
PE on the host. Used to either dry-check anchor coverage on a candidate
partition image, or to produce a pre-patched PE for the GBLP1 cache.

```
abl-patcher --in <abl.bin> [--out <patched.bin>]
abl-patcher --check-anchors-only --in <abl.bin>
abl-patcher --oem <id> --in <abl.bin> [--out <patched.bin>]
```

Flags:

- `--oem <id>` — OEM patch group. `<id>` is one of `{ oplus, none }`. Default
  `none` (no OEM group). `oneplus` is accepted as a deprecation alias for
  `oplus` (still maps to `GBL_OEM_OPLUS`; will be removed in a future release).
  `abl_permissive` patches are always applied at host packing time — the
  on-device manifest decides at runtime whether they take effect.

```
abl-patcher --check-anchors-only --in extracted.efi
```

### gbl-pack

Builds the GBLP1 overlay binary that the on-device `GblPayloadLib` consumes.
Two non-overlapping payload kinds:

- `--cached-abl PE --source RAW --extracted PE` — cache a pre-patched ABL
  PE (`gbl_cached_abl` record), so the on-device patch step can be skipped.
- `--mode2-profile BIN` — embed the 120-byte `gbl_mode2_profile` struct
  (mode-2 overlay).

Both can be combined in a single overlay.

```
gbl-pack --out OUT [--cached-abl PE --source RAW --extracted PE] [--mode2-profile BIN]
```

```
gbl-pack \
  --cached-abl patched.efi \
  --source     stock_abl.img \
  --extracted  extracted.efi \
  --out        payload.bin
```

### gbl-commit

POSIX raw write of `--src` to `--dst`. Same code on host (writes regular
files; used by tests) and Android (writes `/dev/block/by-name/efisp` from
inside the recovery ZIP). `--backup` first reads the destination and saves
a restore copy; `--verify` reads the destination back and SHA-256-checks it
against the source, restoring from backup on mismatch.

```
gbl-commit --src FILE --dst PATH [--backup BACKUP_PATH] [--verify]
```

```
gbl-commit \
  --src installed.efi \
  --dst /tmp/efisp.out \
  --backup /tmp/efisp.bak \
  --verify
```

### vbmeta-graft

vbmeta-aware "stretch a custom image to partition size and graft the
stock OEM vbmeta below it" — the on-device mode-2 cohabit pattern, but
done off-device. Three subcommands.

`list` dumps the descriptors of any vbmeta-bearing image (debug aid):

```
vbmeta-graft list <vbmeta-or-partition-img>
```

`graft` is the easy host path. Needs **only** the stock partition image
(stock vbmeta footer is read from it directly), the custom partition image,
the target partition size, and an output path. Does **not** need the device's
main `vbmeta.img`. If the custom image is AVB-footered, the tool uses its
`OriginalImageSize`; otherwise it treats the whole custom file as the payload:
for a full custom partition image, `--part-size` is usually the custom file
size.

```
vbmeta-graft graft --stock <stock-part> --custom <custom-part> --part-size <target-partition-bytes> --out <out>
```

```
vbmeta-graft graft \
  --stock     stock_partition.img \
  --custom    custom_partition.img \
  --part-size <target-partition-bytes> \
  --out       grafted_partition.img
```

`check` is the optional safety verification. It walks the *device's* main
`vbmeta.img`, finds the chain descriptor for `<part>`, and confirms the
candidate partition image's vbmeta key matches that descriptor's public
key — i.e. the device will still authenticate this candidate at boot.
Needs the device main `vbmeta.img`:

```
vbmeta-graft check <candidate-part-img> <main-vbmeta-img> <part>
```

```
vbmeta-graft check grafted_partition.img vbmeta.img <partition-name>
```

### mode2-profile

Reads a stock `vbmeta.img` and emits the 120-byte `gbl_mode2_profile`
struct (`tools/shared/gbl_mode2_profile.h`) that `gbl-pack --mode2-profile`
embeds into a GBLP1 `0x0010` overlay. Two stages: `derive` extracts the
relevant fields into a human-readable TOML; `compile` reads that TOML and
writes the packed binary.

```
mode2-profile derive  <vbmeta.img> -o <out.toml>
mode2-profile compile <in.toml>    -o <out.bin>
```

```
mode2-profile derive  stock_vbmeta.img -o profile.toml
mode2-profile compile profile.toml     -o profile.bin
```

A pure-Python equivalent ships alongside, `tools/mode2-profile/mode2-profile.py`,
with the same two subcommands and byte-identical output. Use the C tool for
the shippable build (cross-compiles to Windows/macOS/Android with no Python
runtime requirement); use the `.py` for dev iteration on the host:

```
python3 mode2-profile.py derive  stock_vbmeta.img -o profile.toml
python3 mode2-profile.py compile profile.toml     -o profile.bin
```

**`mode2-profile.py derive` requires `avbtool.py`.** It is resolved in order:
`$AVBTOOL`, `~/avbtool.py`, `/usr/bin/avbtool`, `/usr/local/bin/avbtool`.
The C `mode2-profile derive` does not need `avbtool.py` (it has its own AVB parser).

The `mode2-profile/vendor/tomlc99/` directory contains
[tomlc99](https://github.com/cktan/tomlc99) by CK Tan (MIT) — a single-file
C99 TOML parser used by `compile`.

### gblp1-inspect

GBLP1 container inspector — parses + verifies a GBLP1 container and reports
per-entry SHA-256 status. Used by the diag mode. Scans for the GBLP1 header
inside an arbitrary image (so it works on a bare `payload.bin` or a full
EFISP = base EFI || GBLP1), validates the header and every entry digest, and
prints a `result:` verdict (`ok`, `entry_sha_mismatch`, `not_a_gblp1`),
exiting non-zero on any failure.

```
gblp1-inspect <image>
```

```
gblp1-inspect payload.bin
```

## Platform matrix

| Tool             | Linux | Android | Windows | macOS |
|------------------|-------|---------|---------|-------|
| fv-unwrap        | ✓     | ✓       | ✓       | ✓     |
| abl-patcher      | ✓     | ✓       | ✓       | ✓     |
| gbl-pack         | ✓     | ✓       | ✓       | ✓     |
| gbl-commit       | ✓     | ✓       | ✓       | ✓     |
| vbmeta-graft     | ✓     | ✓       | ✓       | ✓     |
| vbmeta-graft.py  | ✓     | —       | ✓       | ✓     |
| mode2-profile    | ✓     | ✓       | ✓       | ✓     |
| gblp1-inspect    | ✓     | ✓       | ✓       | ✓     |
| mode2-profile.py | ✓     | —       | ✓       | ✓     |

`mode2-profile.py` runs on any host with Python 3.11+; `derive` additionally
needs `avbtool.py` reachable via `$AVBTOOL`, `~/avbtool.py`, `/usr/bin/avbtool`,
or `/usr/local/bin/avbtool`.

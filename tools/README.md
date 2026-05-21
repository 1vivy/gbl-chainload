# gbl-chainload host-side tools

Seven small C utilities (and one Python script) for turning a dumped ABL
partition image into a ready-to-flash EFISP payload, then optionally writing
it to disk. Some workflows also consume additional inputs; for example, a
stock `vbmeta.img` is only needed for mode 2. Most of the C tools build
without extra library dependencies; `fv-unwrap` links `liblzma`.
`mode2-profile` ships as both a C binary and a pure-Python script — they
produce byte-identical output, so the C tool is the shippable build path and
`mode2-profile.py` is the dev iteration path. The off-device chain is also
wrapped by `scripts/efisp-package.py`, which calls these tools in order and
produces a single `installed.efi`.

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
tools/fv-unwrap/fv-unwrap tests/images/op15-infiniti-201-abl.img extracted.efi
```

### abl-patcher

Drives the same `DynamicPatchLib` code that runs on-device, but against a
PE on the host. Used to either dry-check anchor coverage on a candidate
partition image, or to produce a pre-patched PE for the GBLP1 cache.

```
abl-patcher --in <abl.bin> [--out <patched.bin>]
abl-patcher --check-anchors-only --in <abl.bin>
abl-patcher --oem <id> [--no-mode1] --in <abl.bin> [--out <patched.bin>]
```

Flags:

- `--oem <id>` — OEM patch group (e.g. `oneplus`). Default `GBL_OEM_NONE`,
  i.e. universal + `mode_1` patches only.
- `--no-mode1` — exclude `mode_1` patches (used by the mode-2 profile path,
  which keeps ABL honest).

```
tools/abl-patcher/abl-patcher --check-anchors-only \
  --in images/infiniti/LinuxLoader_infiniti.efi
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
tools/gbl-pack/gbl-pack \
  --cached-abl patched.efi \
  --source     tests/images/op15-infiniti-201-abl.img \
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
tools/gbl-commit/gbl-commit \
  --src installed.efi \
  --dst /tmp/efisp.out \
  --backup /tmp/efisp.bak \
  --verify
```

### vbmeta-graft

vbmeta-aware "stretch a custom vbmeta to partition size and graft the
stock OEM vbmeta below it" — the on-device mode-2 cohabit pattern, but
done off-device. Three subcommands.

`list` dumps the descriptors of any vbmeta-bearing image (debug aid):

```
vbmeta-graft list <vbmeta-or-partition-img>
```

`graft` is the easy host path. Needs **only** the stock partition image
(stock vbmeta footer is read from it directly), the custom vbmeta to graft
on top, the target partition size, and an output path. Does **not** need
the device's main `vbmeta.img`:

```
vbmeta-graft graft --stock <stock-part> --custom <custom-vbmeta> --part-size <N> --out <out>
```

```
tools/vbmeta-graft/vbmeta-graft graft \
  --stock     stock_recovery.img \
  --custom    custom_recovery_vbmeta.img \
  --part-size 100663296 \
  --out       grafted_recovery.img
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
tools/vbmeta-graft/vbmeta-graft check \
  grafted_recovery.img \
  images/vbmeta-infiniti-IN-16.0.7.201.img \
  recovery
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
tools/mode2-profile/mode2-profile derive \
  images/vbmeta-infiniti-IN-16.0.7.201.img -o profile.toml
tools/mode2-profile/mode2-profile compile \
  profile.toml -o profile.bin
```

A pure-Python equivalent ships alongside, `tools/mode2-profile/mode2-profile.py`,
with the same two subcommands and byte-identical output. Use the C tool for
the shippable build (cross-compiles to Windows/macOS/Android with no Python
runtime requirement); use the `.py` for dev iteration on the host:

```
python3 tools/mode2-profile/mode2-profile.py derive  images/vbmeta-infiniti-IN-16.0.7.201.img -o profile.toml
python3 tools/mode2-profile/mode2-profile.py compile profile.toml              -o profile.bin
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
tools/gblp1-inspect/gblp1-inspect payload.bin
```

## scripts/efisp-package.py

Off-device chaining of `fv-unwrap → abl-patcher → gbl-pack` (plus
`mode2-profile derive`+`compile` in mode 2), concatenated with a base
`mode-N.efi` to produce a single ready-to-flash `installed.efi`. This is
the host-side equivalent of the install ZIP's on-device `build_payload`.
The script only *produces* the file — flashing is the user's manual step
(`fastboot stage` + `oem boot-efi`).

```
efisp-package.py --abl <abl.img> --mode 0|1|2 --efi <mode-N.efi>
                 [--stock-vbmeta <vbmeta.img>] [--oem <id>]
                 [--tools-dir <dir>] [--out <path>]
```

Tool lookup order: `--tools-dir` if given, then the platform `dist/<os>/`
directory (`dist/windows/` on Windows, `dist/macos/` on macOS), then the
script's own directory, then `PATH`. On Linux the default flow expects
binaries built locally under `tools/<tool>/<tool>` and reachable via
`--tools-dir`.

Mode 0 (no `mode_1` patches; minimal payload):

```
python3 scripts/efisp-package.py \
  --abl  tests/images/op15-infiniti-201-abl.img \
  --mode 0 \
  --efi  dist/mode-0.efi
```

Mode 1 (universal + `mode_1` patches; cached ABL in the overlay):

```
python3 scripts/efisp-package.py \
  --abl  tests/images/op15-infiniti-201-abl.img \
  --mode 1 \
  --efi  dist/mode-1.efi
```

Mode 2 (no `mode_1`; mode-2 profile + OEM patch group):

```
python3 scripts/efisp-package.py \
  --abl          tests/images/op15-infiniti-201-abl.img \
  --mode         2 \
  --efi          dist/mode-2.efi \
  --stock-vbmeta images/vbmeta-infiniti-IN-16.0.7.201.img \
  --oem          oneplus
```

Default output is `dist/efisp-payload/<abl-basename>-mode<N>.efi`; override
with `--out`.

## Platform matrix

| Tool             | Linux | Android | Windows | macOS |
|------------------|-------|---------|---------|-------|
| fv-unwrap        | ✓     | ✓       | ✓       | ✓     |
| abl-patcher      | ✓     | ✓       | ✓       | ✓     |
| gbl-pack         | ✓     | ✓       | ✓       | ✓     |
| gbl-commit       | ✓     | ✓       | ✓       | ✓     |
| vbmeta-graft     | ✓     | ✓       | ✓       | ✓     |
| mode2-profile    | ✓     | ✓       | ✓       | ✓     |
| gblp1-inspect    | ✓     | ✓       | ✓       | ✓     |
| mode2-profile.py | ✓     | —       | ✓       | ✓     |

`mode2-profile.py` runs on any host with Python 3.11+; `derive` additionally
needs `avbtool.py` reachable via `$AVBTOOL`, `~/avbtool.py`, `/usr/bin/avbtool`,
or `/usr/local/bin/avbtool`.

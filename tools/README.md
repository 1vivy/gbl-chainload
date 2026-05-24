# tools/

Host + recovery tooling for gbl-chainload. A single Rust multicall binary
`gbl` replaces the seven legacy C tools (`gbl-commit`, `gbl-pack`,
`gblp1-inspect`, `fv-unwrap`, `mode2-profile`, `vbmeta-graft`,
`abl-patcher`). Each former tool maps 1:1 to a subcommand of `gbl`:

| Old binary       | New invocation                          |
|------------------|-----------------------------------------|
| `abl-patcher`    | `gbl patch`                             |
| `gbl-commit`     | `gbl commit`                            |
| `gbl-pack`       | `gbl pack`                              |
| `gblp1-inspect`  | `gbl inspect`                           |
| `fv-unwrap`      | `gbl unwrap`                            |
| `mode2-profile`  | `gbl mode2 {derive,compile,build}`      |
| `vbmeta-graft`   | `gbl avb {list,check,graft,list-hash}`  |

Argv shape, exit codes, and byte-for-byte outputs are preserved against the
captured C-tool golden outputs in `tests/host/goldens/` (see
`docs/superpowers/pr-evidence/2026-05-23-rust-consolidation-parity.md` for
the bit-level parity report). The Python parity tools
(`scripts/mode2-profile.py`, `scripts/vbmeta-graft.py`) still exist as
reference / convenience wrappers and now dispatch through `gbl <sub>`
under the hood.

## Building

All builds go through a single docker image:

```
docker build -t gbl-chainload-build:latest -f docker/Dockerfile .
```

| Platform                    | Output                  | How                                                                            |
|-----------------------------|-------------------------|--------------------------------------------------------------------------------|
| Host (Linux dev binary)     | `target/release/gbl`    | `cargo build --release -p gbl`                                                 |
| Static Linux musl release   | `dist/linux/gbl`        | `scripts/build-cross-tools.sh linux`                                           |
| Android (aarch64, recovery) | `dist/recovery/gbl`     | `scripts/build-recovery-tools.sh`                                              |
| Windows (x86_64 PE)         | `dist/windows/gbl.exe`  | `scripts/build-cross-tools.sh windows`                                         |
| macOS (universal Mach-O)    | `dist/macos/gbl`        | `scripts/build-cross-tools.sh macos` (x86_64 + arm64 merged via `llvm-lipo`)   |

`scripts/build.sh` orchestrates the full four-phase build (cargo workspace
+ EDK2 firmware + recovery cross-build + host build) and is what CI runs.
`SHA256SUMS` is emitted alongside every `dist/<os>/gbl` artifact.

`gbl --version` reports the Cargo package version. The compiled-in
`packer_version` string embedded in `gbl pack` output picks up the in-tree
`VERSION` file for release-branch stability; runs that need
deterministic timestamps set `SOURCE_DATE_EPOCH` (the in-tree golden
tests do exactly this — see `tests/host/060_pack_roundtrip.sh`).

## Subcommands

### `gbl unwrap`

```
gbl unwrap <INPUT> <OUTPUT>
```

Extracts the PE32+ payload out of a dumped Qualcomm-style ABL/XBL
partition image. Walks the arm32-ELF wrapper, the EDK2 firmware volume,
the LZMA-compressed `EFI_SECTION_GUID_DEFINED` section, and nested PE32
sections; emits the inner PE. LZMA decompression uses the pure-Rust
`lzma-rs` crate — no `liblzma` link.

```
gbl unwrap stock_abl.img extracted.efi
```

### `gbl patch`

```
gbl patch --in <PE> [--out <OUT>] [--check-anchors-only] [--oem <id>]
```

Drives the same `DynamicPatchLib` Rust code that runs on-device, but
against a PE on the host. Used to either dry-check anchor coverage on a
candidate partition image (`--check-anchors-only`) or produce a
pre-patched PE for the GBLP1 cache.

Flags:

- `--oem <id>` — OEM patch group. Canonical: `oplus`, `none`. Default
  `none` (no OEM-scoped patches). `oneplus` is accepted as a deprecation
  alias for `oplus` (still maps to `Oem::Oplus`; prints a one-time
  warning; will be removed in a future release).
- `abl_permissive` patches are always applied at host packing time — the
  on-device manifest decides at runtime whether they take effect.

Note (post-PR1): `gbl patch` has NO `--no-mode1` flag. Engine-rework
PR1 collapsed the mode-1 patch group into the always-on `abl_permissive`
set; the on-device manifest gates execution.

```
gbl patch --check-anchors-only --in extracted.efi
gbl patch --oem oplus --in extracted.efi --out patched.efi
```

### `gbl pack`

```
gbl pack --out OUT
         [--cached-abl PE --source RAW --extracted PE]
         [--mode2-profile BIN]
         [--manifest BITS]
```

Builds the GBLP1 overlay binary that the on-device `GblPayloadLib`
consumes. Three non-overlapping entry kinds, any of which can be
combined into a single overlay:

- `--cached-abl PE --source RAW --extracted PE` — cache a pre-patched
  ABL PE (`gbl_cached_abl` record), so the on-device patch step can be
  skipped. `--source` and `--extracted` provide the source-meta SHA
  trail.
- `--mode2-profile BIN` — embed the 120-byte `gbl_mode2_profile` struct
  (mode-2 overlay; emitted by `gbl mode2 compile`).
- `--manifest BITS` — capability-bits manifest entry (PR1 Task 3 wire
  format; type `0x0020`). `BITS` accepts hex (`0x01`) or decimal (`2`).

`SOURCE_DATE_EPOCH` is honored for the embedded ISO timestamp; the
golden tests pin it to `0` for byte-stable output.

```
gbl pack \
  --cached-abl patched.efi \
  --source     stock_abl.img \
  --extracted  extracted.efi \
  --mode2-profile profile.bin \
  --manifest      0x01 \
  --out           payload.bin
```

### `gbl inspect`

```
gbl inspect <IMAGE>
```

GBLP1 container inspector — parses + verifies a GBLP1 container and
reports per-entry SHA-256 status. Used by the diag mode and host-side
regression suite. Scans for the GBLP1 header inside an arbitrary image
(so it works on a bare `payload.bin` or a full EFISP = base EFI ||
GBLP1), validates the header and every entry digest, and prints a
`result:` verdict (`ok`, `entry_sha_mismatch`, `not_a_gblp1`), exiting
non-zero on any failure.

Pretty-prints the PR1 manifest entry's capability bits when present.

```
gbl inspect payload.bin
```

### `gbl mode2`

Mode-2 profile tooling. Three subcommands:

```
gbl mode2 derive  <VBMETA>  -o <OUT.TOML>     # vbmeta → human-readable TOML
gbl mode2 compile <IN.TOML> -o <OUT.BIN>      # TOML → 120-byte binary struct
gbl mode2 build   <VBMETA>  -o <OUT.BIN>      # composite: derive + compile
```

The 120-byte `gbl_mode2_profile` binary (`OUT.BIN`) is what
`gbl pack --mode2-profile` consumes. The intermediate TOML is for
human review and diff against operator-curated baselines.

The standalone `scripts/mode2-profile.py` (pure-Python equivalent of the
old C `mode2-profile`) still exists for dev iteration and now dispatches
through `gbl mode2` for the produced binary. Use `gbl mode2` directly
for shippable builds — no Python runtime requirement.

```
gbl mode2 derive  stock_vbmeta.img -o profile.toml
gbl mode2 compile profile.toml     -o profile.bin
# or composite:
gbl mode2 build   stock_vbmeta.img -o profile.bin
```

### `gbl avb`

AVB vbmeta tooling for partition-level cohabitation (the on-device
mode-2 pattern, done off-device). Four subcommands:

```
gbl avb list      <IMAGE>                                  # walk descriptors
gbl avb check     <CANDIDATE> <MAIN_VBMETA> <PART>         # chain-validate candidate
gbl avb graft     --stock <S> --custom <C> --part-size <N> --out <O>
gbl avb list-hash <ACTIVE_VBMETA> <BYNAME_DIR>             # walk hash + chain over byname
```

`graft` is the easy host path: needs only the stock partition image
(stock vbmeta footer is read from it directly), the custom partition
image, the target partition size, and an output path. If the custom
image is AVB-footered, the tool uses its `OriginalImageSize`;
otherwise it treats the whole custom file as the payload. For a full
partition-sized custom image, `--part-size` is usually the custom file
size.

`check` is the optional safety verification — walks the *device's*
main `vbmeta.img`, finds the chain descriptor for `<part>`, and confirms
the candidate partition image's vbmeta key matches that descriptor's
public key.

`list-hash` walks both hash and chain descriptors over an on-device-style
`by-name/` directory, producing the structured forensic report used by
diag mode and the host parity tests (`tests/host/090`).

```
gbl avb list <stock-or-candidate-partition.img>
gbl avb graft \
    --stock     stock_partition.img \
    --custom    custom_partition.img \
    --part-size <target-partition-bytes> \
    --out       grafted_partition.img
gbl avb check grafted_partition.img vbmeta.img <partition-name>
```

### `gbl commit`

```
gbl commit --src FILE --dst PATH [--backup PATH] [--verify]
```

POSIX raw write of `--src` to `--dst`. Same code on host (writes regular
files; used by tests) and Android (writes `/dev/block/by-name/efisp`
from inside the recovery ZIP). `--backup` first reads the destination
and saves a restore copy; `--verify` reads the destination back through
an **uncached** path (`posix_fadvise(POSIX_FADV_DONTNEED)` on the dst
fd before re-read) and SHA-256-checks it against the source, restoring
from backup on mismatch.

The uncached read-back catches non-persisting writes — see issue #43 /
main commit 6d3adc6 for the bug it shipped to address.

```
gbl commit \
  --src installed.efi \
  --dst /tmp/efisp.out \
  --backup /tmp/efisp.bak \
  --verify
```

## Engine-rework deltas (PR1 contract)

- `gbl patch` has no `--no-mode1` flag (PR1 Task 12 dropped the mode-1
  patch group as a separate gate; `abl_permissive` is always applied;
  the on-device manifest decides at runtime).
- `gbl pack --manifest <bits>` emits a PR1 Task 3 capability-bits
  manifest entry (type `0x0020`); `BITS` accepts hex or decimal.
- `gbl inspect` pretty-prints the manifest entry's bit fields (PR1
  Task 4).
- `gbl patch --oem oplus` is canonical; `--oem oneplus` is a deprecation
  alias.

## Host packaging workflow

Use `scripts/efisp-package.py` when you want to prepare an EFISP payload
on a desktop host instead of inside the recovery ZIP. The script chains
`gbl unwrap → gbl patch → gbl pack` (plus `gbl mode2 derive` + `compile`
for mode 2) and concatenates the result with a base `mode-N.efi` to
produce a single ready-to-stage EFISP image. It produces a file — it
does not write device storage.

```
python3 scripts/efisp-package.py \
  --abl  stock_abl.img \
  --mode 0|1|2 \
  --efi  mode-N.efi \
  [--stock-vbmeta stock_vbmeta.img] \
  [--oem oplus] \
  [--out installed.efi]
```

Mode-specific inputs:

| Mode | Base EFI     | Required extra files                                                    | What gets packed                                          |
|------|--------------|-------------------------------------------------------------------------|-----------------------------------------------------------|
| 0    | `mode-0.efi` | none                                                                    | cached ABL with `abl_permissive` only                     |
| 1    | `mode-1.efi` | none                                                                    | cached ABL with `abl_permissive` + manifest mode-1 bit    |
| 2    | `mode-2.efi` | stock main `vbmeta.img` (`--stock-vbmeta`) and OEM id (`--oem`)         | cached ABL with OEM group + compiled mode-2 profile       |

For mode 2, `--stock-vbmeta` is the device's main vbmeta image used to
derive the 120-byte mode-2 profile. It is separate from any partition
image used with `gbl avb graft`.

### Testing and device staging

Verify the packaged payload before booting it:

```
gbl inspect installed.efi
```

Device testing must use the RAM-only staging path:

```
fastboot stage installed.efi
fastboot oem boot-efi
```

`fastboot stage` + `fastboot oem boot-efi` is a one-shot RAM load that
survives a power cycle without persistent writes. Never flash an EFI
overlay to a non-HLOS partition without operator-in-the-loop
verification — the `.claude/hooks/block-non-hlos-flash.py` PreToolUse
hook will block such commands inside an agent session.

## Advanced usage

- `gbl unwrap` by itself to debug ABL/FV extraction failures before
  running the full package script.
- `gbl patch --check-anchors-only --in <extracted.efi>` to validate
  patch anchor coverage without producing a patched output. Useful
  when surveying a fresh OTA ABL for engine compatibility.
- `gbl mode2 derive` to review the TOML profile derived from a stock
  `vbmeta.img`; `gbl mode2 compile` to turn the reviewed TOML into the
  binary profile consumed by `gbl pack`.
- `gbl pack` directly when combining a cached ABL and mode-2 profile by
  hand, or when constructing focused regression fixtures.
- `gbl inspect <image>` on either a bare `payload.bin` or a full
  base-EFI-plus-GBLP1 image to verify per-entry SHA-256 status.
- `gbl commit` only for file/block-device write workflows you
  intentionally control; `efisp-package.py` itself does not call it
  and does not flash.

## Platform matrix

| Tool       | Linux (host) | Linux (musl) | Android | Windows | macOS |
|------------|--------------|--------------|---------|---------|-------|
| `gbl`      | ✓            | ✓            | ✓       | ✓ *     | ✓     |

\* Windows build requires `x86_64-w64-mingw32-dlltool` in the docker
image (mingw-w64 binutils). Currently missing from `docker/Dockerfile`;
test 084 SKIPs cleanly until added. See
`docs/superpowers/pr-evidence/2026-05-23-rust-consolidation-parity.md`
for the follow-up note.

The host (native) build uses the system Rust toolchain. All cross
targets use the docker build image's pinned Rust (currently 1.85) +
zig for the Windows/macOS targets and the Android NDK for the recovery
target.

## Crate map

The `gbl` multicall is a thin clap front-end over five in-workspace
crates that hold the actual logic. The same crates are linked into the
EDK2 firmware build as `aarch64-unknown-none` staticlibs via FFI shims:

| Crate                              | Role                                            |
|------------------------------------|-------------------------------------------------|
| `crates/pe-utils`                  | PE32+ sanity + UTF-16 efisp marker scan         |
| `crates/gblp1`                     | GBLP1 v1 parse + pack + streaming SHA + CRC     |
| `crates/mode2-profile-core`        | Mode-2 derive (vbmeta) + compile/parse (TOML)   |
| `crates/patch-engine`              | DynamicPatchLib host inversion                  |
| `crates/avb-parse`                 | AvbParseLib inversion (descriptor walk + chain) |

See each crate's `lib.rs` doc block for the API surface; the
`tests/parity.rs` file under each crate locks behavior against the
captured pre-Rust C-tool goldens.

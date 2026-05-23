# Rust tooling consolidation — design

Status: design approved 2026-05-22.

Parent design context: `tools-rs/README.md` from the original spike
(PR #44) establishes that `gbl-commit` ports cleanly to Rust with FFI to
`Sha256.c`. This spec supersedes that pilot's narrow scope: instead of
porting one tool, the chainloader's **core libraries** are inverted to
Rust and the host tools collapse into a single multi-call binary that
links the same crates the firmware does.

## 1. Goal & scope

**Goal.** Eliminate code-duplication and toolchain juggling in the
host/recovery toolchain by making the chainloader's core libraries the
single source of truth: each shared library lives once, in Rust, in
`crates/` at the repo root. The EDK2 firmware *and* a single multi-call
host binary `gbl` both link the same code. Memory-safe parsing on
attacker-controllable bytes (AVB, FV/PE, GBLP1) is a deliberate by-product.

**In scope.** Inversion of `AvbParseLib`, `DynamicPatchLib`, and the SHA/
CRC primitives currently in `GblPayloadLib`; collapse of every host C
tool (`gbl-commit`, `gbl-pack`, `gblp1-inspect`, `fv-unwrap`,
`mode2-profile`, `vbmeta-graft`, `abl-patcher`) into one Rust binary at
`tools/gbl/`; swap of cross-built `liblzma` for the pure-Rust `lzma-rs`;
swap of vendored `tomlc99` for the `toml` crate; deletion of every
shared C header in `tools/shared/`; deletion of the dead
`tools/cache-abl-overlay/`. Single mega-PR; nine sequenced commits.

**Out of scope.** Replacing the EDK2 firmware payload itself (C-locked
by EDK2). Replacing `busybox-arm64`. Collapsing the install pipeline
into a Rust subcommand — preserves shell as the iteration surface where
device-specific learnings land. Performance work. Any *new* functionality
beyond what the C tools already do; this is migration only.

## 2. Architecture

Workspace at repo root. Shared core crates in `crates/`. One binary
crate in `tools/gbl/`. EDK2 directory keeps its current shape; each
`.inf` gains one line referencing the staticlib emitted by cargo.

```
gbl-chainload/
├── Cargo.toml                       workspace { crates/*, tools/gbl }
├── Cargo.lock                       committed; every build uses --locked
├── crates/                          first-class core libraries
│   ├── avb-parse/                   no_std; replaces AvbParseLib/{AvbParse.c, AvbBigEndian.h}
│   ├── patch-engine/                no_std; replaces DynamicPatchLib + holds kEfispUtf16Pattern
│   ├── gblp1/                       no_std; replaces tools/shared/gblp1.h and PayloadParse.c logic
│   ├── pe-utils/                    no_std; pe_sanity + efisp_scan (12-byte detection variant)
│   └── mode2-profile-core/          no_std; derive/compile/parse_toml
├── tools/
│   └── gbl/                         one binary crate, depends on every crates/*
│       ├── Cargo.toml               + sha2, crc32fast, lzma-rs, toml, clap
│       └── src/{main.rs, commands/{commit,unwrap,patch,pack,inspect,mode2,avb}.rs}
├── GblChainloadPkg/                 EDK2 stays where it is
│   └── Library/{AvbParseLib,DynamicPatchLib,GblPayloadLib}/
│                                    each .inf gains [Binaries] LIB|target/.../lib<crate>.a|*
└── scripts/build.sh                 drives the sequenced cargo + EDK2 builds
```

Per shared crate: **two front doors over one safe core.** An idiomatic
Rust API used by cargo consumers (the host CLIs and other crates), plus
an `extern "C"` shim in `ffi.rs` exporting EDK2-shaped types
(`EFI_STATUS`, `UINTN`, …) for firmware `.inf` linking. Each shared
crate has `crate-type = ["rlib", "staticlib"]` so cargo emits both an
rlib (for cargo consumers) and a `.a` (for the firmware INF).

The host tool layer collapses from seven binaries to one. The ZIP ships
`zip/bin/gbl` plus the existing `busybox-arm64`. **No symlinks** anywhere
— shell scripts call `gbl <sub>` directly. The one-time call-site edit
is on the order of fifteen lines across `core/*.sh` and `modes/*.sh`
(see the survey of invocation sites: `gbl-commit` in `core/safety.sh`,
`fv-unwrap` / `abl-patcher` / `gbl-pack` in `modes/install-common.sh`,
`mode2-profile` derive/compile in `modes/mode-2-install.sh`,
`vbmeta-graft` subcommands in `modes/graft-common.sh` and `modes/diag.sh`,
`gblp1-inspect` in `modes/diag.sh`).

## 3. Per-crate inventory

| Crate | Replaces | Rust API | `extern "C"` shim | Consumers |
|---|---|---|---|---|
| `avb-parse` | `AvbParseLib/{AvbParse.c, AvbBigEndian.h}` | `parse_vbmeta`, `parse_footer`, descriptor iterators | firmware-callable `avb_*` mirroring today's `AvbParseLib.h` | firmware (fastboot menu, vbmeta path); `gbl avb`, `gbl mode2` |
| `patch-engine` | `DynamicPatchLib/*` + `tools/shared/patch_signatures.h` (`kEfispUtf16Pattern`) | `scan`, `apply`; `Signature` / `Patch` types | firmware-callable `dpatch_*` | firmware boot-time patcher; `gbl patch` |
| `gblp1` | `tools/shared/gblp1.h` + SHA/CRC parsing in `PayloadParse.c` | `pack`, `parse`, `Entry`, `EntryType` | firmware `gblp1_parse_payload(…)` (thin C shim replaces `PayloadParse.c`) | firmware boot-time payload reader; `gbl pack`, `gbl inspect` |
| `pe-utils` | `Internal/PeSanity.{c,h}` + `tools/shared/efisp_scan.h` | `pe_sanity(&[u8]) -> Result<(), PeError>`, `efisp_marker_present(&[u8]) -> bool` | firmware-callable `gbl_pe_sanity`, `gbl_contains_utf16_efisp` | firmware (PE pre-launch sanity, patcher loader-path gate); `gbl pack`, `gbl unwrap` |
| `mode2-profile-core` | `gbl_mode2_profile.h` + C profile reader | `derive`, `compile`, `parse_toml` (via `toml` crate) | firmware-callable `mode2_profile_*` | firmware (boot-time profile read); `gbl mode2`, `gbl pack` |

**Third-party crates pulled from crates.io and locked** in `Cargo.lock`:
`sha2`, `crc32fast`, `lzma-rs`, `toml`, `clap`. **No wrapper crates of
our own** around these — every Rust caller uses them directly.
`Sha256.c` and `Crc32.c` are deleted together with `PayloadParse.c`
because that is their only firmware caller; once `gblp1` parses the
overlay in Rust, the raw `gbl_sha256` / `gbl_crc32` functions have no
remaining callers.

**`tools/gbl` subcommands:**

```
gbl commit  --src --dst --backup --verify
gbl unwrap  <in> <out>
gbl patch   --in --out [--oem ID] [--no-mode1]
gbl pack    --cached-abl --source --extracted [--mode2-profile] --out
gbl inspect <efisp.img>
gbl mode2   derive | compile | build
gbl avb     check | graft | list | list-hash
```

`gbl mode2 build` is a new additive composite of `derive` + `compile` so
the install path can do one call. `derive` and `compile` remain
individually invokable; `gbl avb` is a pure rename of `vbmeta-graft`
(same subcommands, same semantics).

## 4. Build orchestration

Rust targets added via `rustup target add`:

| Target | Purpose | Linker / cc |
|---|---|---|
| `aarch64-unknown-uefi` | firmware staticlibs (crates only) | `rust-lld` (bundled with rustc) |
| `aarch64-linux-android` | recovery `gbl` binary | NDK r27 `aarch64-linux-android31-clang` (already in image) |
| `x86_64-pc-windows-gnu` | host release | `zig cc` (already in image) |
| `x86_64-apple-darwin`, `aarch64-apple-darwin` | host release | `zig cc` |
| `x86_64-unknown-linux-musl` | host release (static) | `zig cc` |

Each firmware-consumable crate's `.inf` gains one line:

```
[Binaries]
  LIB|target/aarch64-unknown-uefi/release/lib<crate>.a|*
```

EDK2's existing binary-library mechanism pulls the `.a` in at firmware
link time. Each `.inf`'s `[Sources]` shrinks to whatever C glue still has
to live in EDK2 itself (entry-point shims, if any). The `.c` and
internal `.h` files those `.inf`s used to compile are deleted.

`scripts/build.sh` orchestrates in sequence:

```
1. cargo build --release --locked --target aarch64-unknown-uefi \
     -p avb-parse -p patch-engine -p gblp1 -p pe-utils -p mode2-profile-core
2. EDK2 build (existing path) — links the .a's into firmware EFIs
3. cargo build --release --locked --target aarch64-linux-android -p gbl
4. cargo build --release --locked -p gbl                               # host native
   cargo build --release --locked --target <X> -p gbl                  # for each host cross target
```

Each step is idempotent and individually invokable, so iterating just
firmware or just `gbl` stays fast. No new Makefiles — cargo + the existing
`scripts/build.sh` are the only build entry points.

**Dockerfile diff.** Add a rust block: `rustup` install, six targets,
linker environment variables routing the android target to NDK clang
and the three host cross targets to `zig cc`. **Delete** the entire
xz-utils cross-build per target (~50 lines), all `LIBLZMA_*` envs, the
`zig-rc-win` stub used only to satisfy xz's Windows resource-compiler
dance, and the `XZ_VER` ARG. Net: Dockerfile loses ~70 lines.

**`Cargo.lock` discipline.** Committed at repo root. Every build uses
`--locked`. CI fails if the lock file would change. Updates land in
explicit `cargo update` commits, not implicit.

## 5. Parity & test gates

The mega-PR's review-ability problem is "did we keep behaviour the same?"
The discipline answering it: **freeze the C tools' outputs as
checked-in goldens before deleting the C, and the new Rust outputs must
byte-match those goldens.** The goldens are the explicit parity contract
that outlives the C code.

**Goldens** (first commit of the PR). `tests/host/goldens/` is checked
into the tree, populated by running the existing C tools against
representative fixtures from `tests/images/` *and real-device fixtures
captured from infiniti*: the stock OEM vbmeta, a real `vbmeta_a.img`
from the 2026-05-21 diag bundle, a real post-install EFISP image, real
patched ABLs per mode/OEM combination. Every existing host test (060
through 091) grows a one-line golden assertion that still passes with the
C tools — proving the goldens are correct before any C is replaced.

**Per-crate Rust tests.** `crates/<name>/tests/parity.rs` exercises the
idiomatic Rust API against the same fixtures and asserts equality with
the goldens. Tests run under `cargo test --workspace --locked`.

**`tools/gbl` integration tests.** Every existing `tests/host/0XX.sh` is
redirected at `gbl <sub>`, with the golden assertion line. Strict
byte-exact equality, no tolerance.

**Bit-level parity report for the firmware-linked crates.** As part of
the migration work, `avb-parse`, `patch-engine`, `gblp1`,
`mode2-profile-core`, and `pe-utils` outputs are compared C-vs-Rust on
real-device fixtures and the diff attached in the PR description as
supporting evidence. AVB parsing and the other parsers are pure
deterministic logic over byte buffers — host parity is sufficient
evidence that the firmware path will produce identical outputs, because
the firmware crate is the same crate.

**Firmware build smoke.** EDK2 build produces the three base EFIs at
plausible sizes and the `.a`s link cleanly with no missing symbols.

**On-device validation.** Recommended post-merge sanity (mode-2 install
on infiniti from a fresh state; mode2-validated 2026-05-18 standards:
key attestation + RKP + Widevine + Strongbox + SOTER + zero ABL
`vb-fakelock` lines), **not blocking**. Rationale: the chainloader's
safety architecture is already the failure backstop — mode-2 ABL stays
honest, broken verified-boot falls through to fastbootlib rather than
bricking; a regression that escaped host goldens gets a follow-up commit,
not a merge revert.

**CI shape.**

```
cargo build --workspace --locked                               # native host
cargo test  --workspace --locked                               # crate parity
cargo build --release --locked --target aarch64-unknown-uefi   # firmware staticlibs build
cargo build --release --locked --target aarch64-linux-android  # recovery gbl builds
EDK2 firmware build                                            # links the .a's
bash tests/host/run-all.sh                                     # 060–092 vs goldens
```

If any of those red, the PR doesn't merge.

## 6. Commit sequence

Nine commits inside the one PR. Each leaves the tree green. Each
commit either captures a golden, introduces a crate, consolidates the
tools, or deletes its now-redundant C — never both in one commit.

| # | Commit | Lands | Deletes |
|---|---|---|---|
| 1 | `goldens: capture pre-migration C tool outputs` | `tests/host/goldens/` (including real-device fixtures from infiniti); golden assertions added to 060–091 (still passing with C) | — |
| 2 | `workspace: root cargo + Dockerfile rust toolchain` | root `Cargo.toml`, `Cargo.lock`, Dockerfile rust block + six targets | `tools-rs/` (PR #44 scaffold superseded by root layout), entire xz/liblzma cross-build per target, `LIBLZMA_*` envs, `zig-rc-win` stub, `XZ_VER` ARG |
| 3 | `crates/pe-utils` | `pe_sanity` + `efisp_scan` crate + firmware shim; `tests/host/helpers/test_pe_sanity.c` and `test_efisp_scan.c` become `crates/pe-utils/tests/*` | `Internal/PeSanity.{c,h}`, `tools/shared/efisp_scan.h` |
| 4 | `crates/gblp1 + PayloadParse port` | `gblp1` crate (consumes `sha2`+`crc32fast` directly); thin C shim in `PayloadParse` calling `gblp1_parse_payload` via `extern "C"` | `PayloadParse.c`, `Sha256.{c,h}`, `Crc32.{c,h}`, `tools/shared/gblp1.h` (all delete together — only callers gone) |
| 5 | `crates/mode2-profile-core` | derive/compile/parse_toml crate + firmware shim | C profile reader + `gbl_mode2_profile.h` |
| 6 | `crates/patch-engine + DynamicPatchLib inversion` | `patch-engine` crate (includes `kEfispUtf16Pattern` const); `DynamicPatchLib.inf` repoints | `DynamicPatchLib/*.c`, `PatchScope.h`, `tools/shared/patch_signatures.h` |
| 7 | `crates/avb-parse + AvbParseLib inversion` | `avb-parse` crate + firmware shim; bit-level C-vs-Rust parity diff attached in PR description as evidence | `AvbParseLib/AvbParse.c`, `AvbBigEndian.h` |
| 8 | `tools/gbl: single multi-call binary` | `tools/gbl/` consuming every crate + `lzma-rs` + `toml` + `clap`; every `tests/host/0XX.sh` re-pointed at `gbl <sub>` (still asserting against the same goldens) | every `tools/<each>/` (gbl-commit, gbl-pack, gblp1-inspect, fv-unwrap, mode2-profile, vbmeta-graft, abl-patcher), per-tool Makefiles, `.pdb`, `-android` artifacts |
| 9 | `zip: shell calls gbl <sub>; cleanup; scripts/build.sh` | shell rewritten across `core/*.sh` and `modes/*.sh`; `zip/bin/MANIFEST` reduced to one entry; `scripts/build.sh` orchestrates cargo+EDK2 | per-tool `MANIFEST` entries and per-tool binaries in `zip/bin/`, `tools/cache-abl-overlay/`, `tools/shared/` remnants, any stray artifacts |

## 7. Branch and PR housekeeping

Continue on the existing `spike/rust-tooling-pilot` branch — PR #44 evolves
into this mega-PR. Title and description get rewritten to reflect the
new scope. The existing spike commit `d0486e5` stays as the first
foundation; the nine migration commits stack on top.

**Pre-merge checklist** (lives in PR description):

- [ ] `cargo build --workspace --locked` clean on every target
- [ ] `cargo test --workspace --locked` green
- [ ] `bash tests/host/run-all.sh` green against goldens
- [ ] EDK2 firmware build produces the three base EFIs at plausible sizes
- [ ] Bit-level parity report (C vs Rust on real-device fixtures) attached
- [ ] `Cargo.lock` reviewed for surprise dep bumps

On-device validation is recommended post-merge but not a blocking gate
on this checklist, by deliberate choice (§5 rationale).

## 8. Open questions / follow-ups (not in this PR)

- **`busybox-arm64`** stays as-is. The ZIP needs `sh`/`awk`/`grep`/`dd`
  for shell helpers; not in scope to replace.
- **Performance.** These tools run once per install on a recovery-side
  device that's already taking minutes. Not in scope; would notice and
  fix if egregious.
- **Refactoring the goldens themselves.** Goldens are *outputs*, not
  implementations. Future intentional behavior changes rebuild the
  golden in deliberate commits — that's how a parity contract is meant
  to evolve.

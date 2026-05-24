# PR2 Rust Consolidation — Pre-Merge Parity Report

Captured: 2026-05-23 on branch `spike/rust-tooling-pilot` at HEAD `d390e8f`
(+ Task 10 cleanup commits). Plan: `docs/superpowers/plans/2026-05-22-rust-tooling-consolidation.md`.

## Summary

PR2 collapses the seven host-side C tools (`abl-patcher`, `gbl-commit`,
`gbl-pack`, `gblp1-inspect`, `fv-unwrap`, `mode2-profile`, `vbmeta-graft`)
into a single Rust multicall binary `gbl` with seven subcommands, backed by
five in-workspace crates (`pe-utils`, `gblp1`, `mode2-profile-core`,
`patch-engine`, `avb-parse`). Each former C tool's argv shape, exit codes,
and byte-for-byte outputs are preserved — verified by the
`tests/host/0XX_*.sh` suite against frozen pre-migration goldens under
`tests/host/goldens/` (captured in Task 2).

Evidence captured in this report:

- pre-merge checklist items 1-6 each walked with command output;
- per-crate parity evidence (representative goldens, all byte-identical);
- known gap on test 084 (Windows cross-build dlltool prerequisite, see
  *Known gaps* below);
- the `[user-provides]` fixture gap inherited from Task 2.

Tooling versions: `cargo 1.95.0`, `rustc 1.95.0` (host). Docker build image
ships `cargo 1.85.0` (bumped in Task 8 to satisfy `clap_lex 1.1`).

## Pre-merge checklist

### 1. `cargo build --workspace --locked` clean

```
$ cargo build --workspace --locked
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.02s
```

All six members (`avb-parse`, `gblp1`, `mode2-profile-core`, `patch-engine`,
`pe-utils`, `gbl`) compile under `--locked` with no warnings. Release
profile is equally clean (used by `scripts/build.sh` and the host tests
via `cargo build --release -p gbl`).

### 2. `cargo test --workspace --locked` green

```
$ cargo test --workspace --locked
[snip]
test result: ok. 19 passed; 0 failed; 0 ignored        (avb-parse lib unit)
test result: ok. 8 passed;  0 failed; 0 ignored        (avb-parse parity)
test result: ok. 3 passed;  0 failed; 0 ignored        (gbl main unit)
test result: ok. 3 passed;  0 failed; 0 ignored        (gblp1 lib unit)
test result: ok. 17 passed; 0 failed; 0 ignored        (gblp1 parity)
test result: ok. 17 passed; 0 failed; 0 ignored        (mode2-profile-core lib)
test result: ok. 4 passed;  0 failed; 0 ignored        (mode2-profile-core parity)
test result: ok. 24 passed; 0 failed; 0 ignored        (patch-engine lib)
test result: ok. 11 passed; 0 failed; 0 ignored        (patch-engine parity)
test result: ok. 8 passed;  0 failed; 0 ignored        (pe-utils lib)
test result: ok. 13 passed; 0 failed; 0 ignored        (pe-utils parity)
TOTAL: passed=127 failed=0
```

Two stale test fixtures were corrected by Task 10 to align them with the
fixes Task 8 made to the underlying crates (these were missed in Task 8):

- `crates/gblp1/tests/parity.rs::mode2_profile_present_and_absent` —
  Task 8 fixed `GBL_M2P_SIZE` in `gblp1::lib` from 256 → 120 to match the
  wire size in `mode2-profile-core` / `tools/shared/gbl_mode2_profile.h`,
  but left the test fixture at 256 bytes. Test now uses a 120-byte
  GM2P-prefixed blob.
- `crates/mode2-profile-core/tests/parity.rs::derive_infiniti_vbmeta_matches_golden` —
  the captured-pre-Rust C-tool TOML golden was relocated by Task 8 from
  `tools/mode2-profile/tests/baseline.toml.golden` (path no longer
  exists; C tool tree deleted) to `tests/host/goldens/087/baseline.toml`.
  Test now reads from the relocated path.

### 3. `bash tests/runall.sh` green vs goldens

```
$ bash tests/runall.sh
[snip]
== 010_build_smoke ==
[snip dist/gbl-chainload.efi 724992 bytes, dist/host/gbl 816936 bytes,
 dist/recovery/gbl 794320 bytes]
--- VERBOSE strip verification ---
OK: 3 VERBOSE probe marker(s) present in verbose build
ok 010_build_smoke
ALL TESTS PASS
```

Tests 042, 045-047, 051 (engine + lints), 060-094 (host parity suite), and
010 (build smoke) all pass. Test 084 SKIPs cleanly (see *Known gaps*).

### 4. Firmware build produces `dist/gbl-chainload.efi`

```
$ ls -la dist/gbl-chainload.efi dist/host/gbl dist/recovery/gbl
-rw-r--r-- 1 vivy vivy 724992 May 23 19:40 dist/gbl-chainload.efi
-rwxr-xr-x 1 vivy vivy 816936 May 23 13:40 dist/host/gbl
-rwxr-xr-x 1 root root 794320 May 23 13:40 dist/recovery/gbl
```

`dist/gbl-chainload.efi` lands at 724,992 bytes — inside the
~720-725 KB band predicted by the plan. Verbose strip verification fires
3 probe markers as expected.

### 5. Bit-level C-vs-Rust parity diffs

Per-crate parity evidence. The C tools' frozen outputs are in
`tests/host/goldens/` (captured in Task 2). The Rust subcommands' outputs
are byte-compared against the goldens by each `tests/host/0XX_*.sh`.
Empty diffs are the expected case — `cmp` exits 0 when bytes match.

**crates/pe-utils** — `pe_sanity` + `efisp_marker` (no produced binary
output to diff; behavioural via unit + parity tests):

```
$ cargo test -p pe-utils --locked
test result: ok. 8 passed; 0 failed   (lib unit)
test result: ok. 13 passed; 0 failed  (parity)
```

The PE sanity gate fires correctly across `tests/host/063` (unit) and is
exercised transitively by every patch / pack test below.

**crates/gblp1** — header parse + entry walk + manifest:

```
$ cmp tests/host/.last/060/payload.bin    tests/host/goldens/060/payload.bin   && echo identical
identical
$ cmp tests/host/.last/060/patched.efi    tests/host/goldens/060/patched.efi   && echo identical
identical
$ cmp tests/host/.last/064/infiniti-EU-16.0.5.703.bin          tests/host/goldens/064/infiniti-EU-16.0.5.703.bin          && echo identical
identical
$ cmp tests/host/.last/064/infiniti-EU-16.0.5.703.patched.efi  tests/host/goldens/064/infiniti-EU-16.0.5.703.patched.efi  && echo identical
identical
$ cmp tests/host/.last/094/with-manifest.bin  tests/host/goldens/094/with-manifest.bin  && echo identical
identical
$ cmp tests/host/.last/094/dec-manifest.bin   tests/host/goldens/094/dec-manifest.bin   && echo identical
identical
$ cmp tests/host/.last/094/no-manifest.bin    tests/host/goldens/094/no-manifest.bin    && echo identical
identical
```

All byte-identical. Tests 060/061/064/067/069/081/089/094 cover the
pack/parse roundtrip across the manifest matrix.

**crates/mode2-profile-core** — parse + compile (derive uses Option-A
defer of vbmeta_hash recomputation, structurally identical to the C tool):

```
$ bash tests/host/082_mode2_profile_parity.sh
  derive parity: PASS (vbmeta fixture found)
PASS: 082 mode2-profile parity
$ bash tests/host/087_mode2_profile_regression.sh
PASS: 087 mode2-profile regression
$ bash tests/host/080_mode2_profile_compile.sh
PASS: 080 mode2 profile compile
```

082 cross-checks the Rust `derive`/`compile` chain against the captured
in-tree vbmeta fixture; 080 confirms `compile` matches the Python
reference; 087 locks the TOML format (golden under
`tests/host/goldens/087/baseline.toml`, the relocated test fixture cited
in checklist item 2 above).

**crates/patch-engine** — patch6/7/10 on the 4 ABL fixtures:

```
$ bash tests/host/088_patch7_multi_abl.sh
  ok: op15-infiniti-703-abl      — patch7 + patch10 + patch6 applied; patch7 idempotent
  ok: op15-infiniti-201-abl      — patch7 + patch10 + patch6 applied; patch7 idempotent
  ok: op15t-fairlady-201-abl     — patch7 + patch10 + patch6 applied; patch7 idempotent
  ok: xi17-pudding-44-abl        — patch7 MISS (non-oplus); abl_permissive (patch6+patch10) OK
PASS: 088 patch7 multi-abl

$ for f in tests/host/goldens/088/*; do n=$(basename "$f"); cmp tests/host/.last/088/"$n" "$f" && echo "$n: identical"; done
op15-infiniti-201-abl.p1.efi:       identical
op15-infiniti-201-abl.pe.efi:       identical
op15-infiniti-703-abl.p1.efi:       identical
op15-infiniti-703-abl.pe.efi:       identical
op15t-fairlady-201-abl.p1.efi:      identical
op15t-fairlady-201-abl.pe.efi:      identical
xi17-pudding-44-abl.p.efi:          identical
xi17-pudding-44-abl.pe.efi:         identical

$ bash tests/host/083_abl_patcher_oem.sh
  ok: --oem oplus applies patch7 + abl_permissive patches
  ok: --oem oneplus prints deprecation msg, still maps to oplus
  ok: plain invocation always applies abl_permissive (no OEM scope)
  ok: --oem bad_oem_name rejected with exit 2 + clear message
PASS: 083 abl-patcher --oem behavior
```

8/8 byte-identical against the 088 fixture matrix. 083 validates the
`--oem oplus` / `--oem oneplus` (deprecation alias) / no-OEM / bad-OEM
behavioural surface against the post-PR1 contract (no `--no-mode1`).

**crates/avb-parse** — descriptor walk + chain verdict:

```
$ bash tests/host/074_vbmeta_graft.sh
PASS: 074 vbmeta-graft
$ cmp tests/host/.last/074/list.txt  tests/host/goldens/074/list.txt  && echo identical
identical

$ bash tests/host/090_vbmeta_descriptor_hash.sh
PASS: 090 vbmeta descriptor hash
$ bash tests/host/091_vbmeta_graft_py.sh
PASS: 091 vbmeta graft (py wrapper roundtrip)
```

074 list output byte-identical to the captured C-tool golden; 090
exercises `gbl avb list-hash` against the perturbed-byname corpus; 091
exercises the `vbmeta-graft.py` wrapper round-trip (`gbl avb list`
re-walking a freshly-grafted partition).

**Aggregate** — all 5 representative golden directories byte-identical:

```
060: payload.bin, patched.efi
064: infiniti-EU-16.0.5.703.{bin,patched.efi}
074: list.txt
088: 8 .efi files across 4 ABL fixtures
089: manifest.bin/.txt, manifest2.bin/.txt, ok.txt, payload.bin
094: with-manifest.bin, dec-manifest.bin, no-manifest.bin
```

### 6. Cargo.lock dep review

```
$ wc -l Cargo.lock     # 422 lines, 55 packages total
$ grep -c '^name = ' Cargo.lock     # 55
```

Direct dependencies per crate (cargo tree --depth 1):

```
gbl (multicall)            avb-parse, gblp1, mode2-profile-core, patch-engine,
                           pe-utils, clap 4.6, sha2 0.10, crc32fast 1.5,
                           lzma-rs 0.3, libc 0.2 (unix-only)
gblp1                      sha2 0.10, crc32fast 1.5
mode2-profile-core         serde 1.0, sha2 0.10, toml 0.8
patch-engine               (no external deps)
pe-utils                   (no external deps)
avb-parse                  (no external deps)
```

No `cc` build-time dep — no crate carries a `build.rs`. Transitive 49
crates are all standard `clap`/`serde`/`sha2`/`toml`/`lzma-rs`
infrastructure (digest, generic-array, typenum, byteorder, anstyle/
anstream family for clap colour output, syn/quote/proc-macro2 for derive
macros). No surprise dep bumps, no unmaintained or yanked crates, no
unsafe-heavy packages added.

Notable: `windows-sys 0.61.2` is pulled in transitively by clap on the
Windows cross-build target only; it is the proximate cause of the test
084 dlltool gap documented below.

## Known gaps

### Test 084 Windows cross-build — pre-existing Dockerfile dlltool gap

The Windows cross-build target (`x86_64-pc-windows-gnu`) at Rust 1.85+
pulls in `windows-sys 0.61.2`, which invokes
`x86_64-w64-mingw32-dlltool` during compilation. The `docker/Dockerfile`
build image ships zig (which carries mingw headers and libc) but does
**not** install the GNU binutils mingw-w64 package that provides
`x86_64-w64-mingw32-dlltool`, so cargo aborts the Windows build with:

```
error: Error calling dlltool 'x86_64-w64-mingw32-dlltool':
       No such file or directory (os error 2)
error: could not compile `windows-sys` (lib) due to 1 previous error
```

This is a **pre-existing infrastructure gap**, not a Task 9 regression —
it surfaced because Task 8 bumped the docker image's `RUST_VER` from
1.78 to 1.85 (required by `clap_lex 1.1` / edition 2024), and the older
`windows-sys` that 1.78 resolved did not invoke dlltool.

**PR2 takes option (b)**: document the gap and SKIP cleanly. Task 10
extended `tests/host/084_cross_build.sh` so that, in addition to its
existing `cargo --version < 1.85` SKIP, it now also SKIPs when the
docker image lacks `x86_64-w64-mingw32-dlltool`. This keeps
`bash tests/runall.sh` green while preserving the test as soon as the
image grows mingw-w64.

**Follow-up**: add `apt-get install -y --no-install-recommends mingw-w64`
to `docker/Dockerfile` next to the existing zig provisioning block, then
rebuild the image (`docker build -t gbl-chainload-build:latest -f docker/Dockerfile .`).
The 084 SKIP guard auto-detects dlltool presence — no test change needed
once the image is refreshed. macOS cross-build (also exercised by 084)
already works on the current image via the zig wrapper scripts.

### Real-device fixtures

What's in-tree (correction: the original Task 2 MANIFEST under-counted these):

- **Stock OEM vbmeta**: `tests/images/vbmeta-infiniti-IN-16.0.7.201.img`
  — wired into 079 / 082-derive / 087 (mode-2 derive parity).
- **Stock recovery image**: `tests/images/recovery-infiniti-IN-16.0.7.201.img`
  — present in tree; NOT yet wired into a golden test.
- **Custom recovery image**: `tests/images/recovery-infiniti-OrangeFox.img`
  — present in tree; NOT yet wired into a golden test.
- **Grafted recovery output**: `tests/images/grafted-recovery.img`
  — wired into 074 / 090 / 091 (vbmeta-graft descriptor walk + self-graft
  round-trip).

Genuinely missing (would broaden coverage further):

- **vbmeta dumps from 203/703-era shipping ROMs** — would broaden the
  mode-2 derive coverage beyond the 16.0.7.201 dump.
- **Real post-install EFISP partition dump from a device** — none
  in-tree. The on-device EFISP-PE gate path (the one that aborted on
  blank EFISP per the `efisp_pe_gate_blocks_first_install` memory) is
  currently exercised only synthetically via `085_efisp_package.sh`'s
  end-to-end driver.

Follow-up opportunity (not blocking): wire the two recovery images
into an explicit `gbl avb graft` parity test — input = stock+OrangeFox
recovery pair + stock vbmeta footer; assertion = descriptor-walk
output matches a captured golden. Today 091 only exercises the
self-graft round-trip (grafted-recovery → grafted-recovery); a true
end-to-end graft from raw stock + custom recovery isn't covered.

## Reviewer notes (optional polish for follow-up)

Items observed during Task 10's sweep that did NOT block merge but are
worth a cleanup pass:

- `tools/RELEASE_README.md` was not inspected during Task 8's rewrite —
  may still reference the deleted C tools. Worth a glance during the
  next release cut.
- The two stale test fixtures fixed in checklist item 2 above are
  evidence that Task 8 didn't run `cargo test --workspace` after
  fixing `GBL_M2P_SIZE` and deleting `tools/mode2-profile/`. A "rerun
  cargo test --workspace after large refactors" item is worth adding
  to the team's review checklist.
- Test 084 is now triple-gated (docker present, image present, dlltool
  present, rust ≥ 1.85). Once mingw-w64 lands in the Dockerfile, the
  rust-version guard becomes the only SKIP path. The triple gating is
  defensible — each SKIP message names its prerequisite clearly.

## PR description draft (for `gh pr edit 44` when ready to push)

```
PR2: Rust tooling consolidation — 7 C tools → 1 `gbl` multicall + 5 crates

## What

Collapse the seven host-side C tools (abl-patcher, gbl-commit, gbl-pack,
gblp1-inspect, fv-unwrap, mode2-profile, vbmeta-graft) into a single
Rust multicall binary `gbl` with seven subcommands. Each former tool
becomes a 1:1 subcommand:

  gbl patch    ← abl-patcher  (no --no-mode1, post-PR1)
  gbl commit   ← gbl-commit   (with fadvise(DONTNEED) uncached read-back)
  gbl pack     ← gbl-pack     (with --manifest <bits>, PR1 Task 3)
  gbl inspect  ← gblp1-inspect (pretty-prints manifest, PR1 Task 4)
  gbl unwrap   ← fv-unwrap    (LZMA via lzma-rs, no liblzma link)
  gbl mode2 {derive,compile,build}  ← mode2-profile
  gbl avb {list,check,graft,list-hash}  ← vbmeta-graft

Backing crates (in-workspace, no FFI for the multicall):

  crates/pe-utils            PE32+ sanity + UTF-16 efisp marker scan
  crates/gblp1               GBLP1 v1 parse + pack + streaming SHA + CRC
  crates/mode2-profile-core  derive (vbmeta → struct) + compile/parse (TOML)
  crates/patch-engine        DynamicPatchLib host inversion (abl_permissive +
                             OEM oplus scope; retired patches host-only)
  crates/avb-parse           AvbParseLib inversion (descriptor walk + chain check)

The C tools they replaced (`tools/{abl-patcher,gbl-commit,gbl-pack,
gblp1-inspect,fv-unwrap,mode2-profile,vbmeta-graft}/`) and their
`Internal/*.{c,h}` sources are deleted. `tools/shared/gbl_staged_buffer.h`
is retained (firmware producer/consumer contract, unrelated).

The on-device firmware path is unchanged structurally — the same crates
are also linked into the EDK2 build as `aarch64-unknown-none` staticlibs
via FFI shims for `DynamicPatchLib`, `GblPayloadLib`, `AvbParseLib`, and
`Mode2ProfileLib`.

## Why

- One Rust toolchain replaces seven C make targets, three cross-compile
  scripts, and ad-hoc Python parity tools.
- Memory safety + std::path::Path + Result-based error handling at the
  host surface; the firmware staticlibs stay `no_std` and panic-free.
- `lzma-rs` replaces the `liblzma` system dep — fully reproducible
  cross-builds via zig (Windows/macOS) and `aarch64-linux-android`
  (recovery) from a single docker image.
- The `gbl-commit` rewrite picks up the fadvise(DONTNEED) uncached
  read-back that landed in main as `gbl_commit uncached verify` (issue
  #43 / commit 6d3adc6).

## Engine-rework contract preserved

- `gbl patch` has NO `--no-mode1` flag (PR1 Task 12 dropped it).
- `--oem oplus` is canonical; `--oem oneplus` is a deprecation alias.
- `gbl pack --manifest <bits>` and `gbl inspect` manifest pretty-print
  match the PR1 wire format exactly.

## Test plan / parity evidence

See `docs/superpowers/pr-evidence/2026-05-23-rust-consolidation-parity.md`
for the bit-level C-vs-Rust diff report. Highlights:

- `cargo build --workspace --locked` clean
- `cargo test --workspace --locked`: 127 tests, 0 failed
- `bash tests/runall.sh`: ALL TESTS PASS (test 084 SKIPs on docker image
  missing mingw-w64 — pre-existing gap, see parity report)
- `dist/gbl-chainload.efi`: 724,992 bytes (in band)
- 060 / 064 / 074 / 088 / 089 / 094 goldens all byte-identical to the
  frozen C-tool outputs in `tests/host/goldens/`.

## Known follow-ups

- Add `apt install mingw-w64` to `docker/Dockerfile` to re-enable test
  084's Windows cross-build leg. macOS leg already works via zig.
- Optional fixture additions tracked in
  `tests/host/goldens/MANIFEST` (operator-supplied `[user-provides]`).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

This draft is here rather than applied to PR #44 because the branch is
local to the worktree and the PR likely hasn't been opened on GitHub
yet — pushing and opening the PR is the user's call.

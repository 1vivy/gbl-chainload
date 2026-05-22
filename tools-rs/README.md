# tools-rs — Rust port of the gbl-chainload host/recovery tools (SPIKE)

Status: **pilot / proof-of-approach.** Ships nothing on-device yet. The C tools
under `tools/` remain authoritative until a port is reviewed *and* its
on-device parity is proven. This branch ports one tool — `gbl-commit` — to
establish the build, FFI, version, and test plumbing, and assesses the rest.

## Why Rust for this tooling

These tools parse untrusted, attacker- and OEM-controlled binary blobs (vbmeta
footers, FV/PE section streams, GBLP1 overlays). That parsing surface is exactly
where C's memory-unsafety bites (cf. the historical ABL-unwrap section-handler
bug). For a *security* tool, memory safety on the parse path is a concrete win,
not fashion. The IO/verify core (`gbl-commit`) benefits less from safety but is
the smallest, lowest-risk tool to pilot the pipeline with.

## What the pilot proves

- **Builds on the host** via `cargo` (`cargo build --release`).
- **Byte-parity with the EFI payload**: SHA-256 is the *existing* C
  `GblChainloadPkg/.../Sha256.c`, compiled and linked through `build.rs` (the
  `cc` crate) — not reimplemented. This is the pattern the parser ports reuse
  for `Crc32.c` and `AvbParseLib`.
- **Single-source version**: `GBL_TOOL_VERSION` is read from the top-level
  `VERSION` at build time, same contract as the C Makefiles.
- **Behavioural parity** with the C `gbl-commit`: `tests/host/092` runs both
  binaries through identical scenarios and asserts matching exit codes
  (0/1/2/3) and byte-identical output files.
- The uncached verify (`POSIX_FADV_DONTNEED` read-back) is preserved verbatim.

## Build

```sh
# host (native)
cargo build --release --manifest-path tools-rs/Cargo.toml
# -> tools-rs/target/release/gbl-commit

# convenience wrapper mirroring the C tools' make flow
make -C tools-rs            # host
make -C tools-rs android    # aarch64 static (needs the targets below)
```

### Cross-compiling for recovery (aarch64)

Use the same NDK r27 clang the C tools' `android` target uses, so the recovery
binary's environment (bionic) matches exactly. The Docker image wires the
`CC_`/`LINKER` env for this target:

```sh
rustup target add aarch64-linux-android
CC_aarch64_linux_android=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang \
CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$CC_aarch64_linux_android \
cargo build --release --target aarch64-linux-android \
  --manifest-path tools-rs/Cargo.toml
```

`build.rs` cross-compiles the shared `Sha256.c` automatically via the `cc`
crate, picking up the same `CC_aarch64_linux_android`. A fully static
`aarch64-unknown-linux-musl` build is a possible alternative but needs a musl
aarch64 cross-toolchain not currently in the image; the NDK path is the
provisioned, parity-matching one.

## Migration assessment — remaining tools

Ordered by include-dependency surface (your call): port cleanest-first, and
**link the existing C** for anything that defines a shared binary format or
algorithm, so parity can't drift.

| Tool | Shared deps | Rust approach | Effort | Risk |
|------|-------------|---------------|--------|------|
| `gbl-commit` | `Sha256` | **done** — FFI `Sha256.c` | — | low |
| `gblp1-inspect` | `gblp1.h`, `Sha256`, `Crc32` | FFI `Sha256.c`+`Crc32.c`; model GBLP1 with `#[repr(C)]` structs | small | low |
| `gbl-pack` | `PeSanity`, `gblp1.h`, `efisp_scan`, `gbl_mode2_profile`, `Sha256`, `Crc32` | FFI the C helpers; pack logic in safe Rust over `#[repr(C)]` layouts | medium | medium (writes the on-device GBLP1 overlay — needs golden-payload parity tests, cf. host test 060) |
| `mode2-profile` | vendored `tomlc99`, `AvbParseLib`, `AvbBigEndian`, `Sha256` | `toml` crate replaces `tomlc99`; FFI `AvbParseLib` (don't re-port AVB); derive/compile in Rust | larger | medium (device-validated; guard with the existing 076–082/087 parity tests) |

`fv-unwrap`, `abl-patcher`, `vbmeta-graft` are out of scope for this spike;
`fv-unwrap` additionally links `liblzma` (the Docker image already cross-builds
static liblzma per target, reusable from a `cc`/FFI Rust port).

## Recommendation

Incremental, never big-bang. Land `gbl-commit` first (this branch) to wire CI +
cross + vendoring, then `gblp1-inspect`, then `gbl-pack`, then `mode2-profile`,
each gated behind its existing host parity tests. Re-port validated parsers only
when there's a reason to touch them.

## Not done in this spike (follow-ups)

- Vendoring the built aarch64 binary into `zip/bin/` + `zip/bin/MANIFEST`.
- CI wiring (build + run `tests/host/092` with the toolchain present).
- The cross builds aren't *executed* here (no NDK/musl-cross in this sandbox);
  the commands and Docker layer are provided and the host build + parity test
  are green.

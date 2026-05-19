# Host-side tools — Windows/macOS cross-compilation + README (slice 4)

Status: design approved 2026-05-19.

This is the deferred mode-2 "slice 4" — *solidifying the host-side tools*. The
six host-side C tools currently build for the local Linux host and
cross-compile to aarch64-Android (for the recovery ZIP). This slice makes them
buildable for Windows and macOS too, documents all of them in one place, adds
one orchestration wrapper, and wires real cross-platform CI.

## 1. Goal & scope

**In scope:**

- Windows (x86_64) and macOS (x86_64 + arm64) cross-compilation for all six
  host-side C tools — `fv-unwrap`, `abl-patcher`, `gbl-pack`, `gbl-commit`,
  `vbmeta-graft`, `mode2-profile` — via a single cross-toolchain (`zig`) added
  to the docker build image.
- A consolidated `tools/README.md` covering every tool: purpose, CLI usage,
  build instructions, and a platform matrix.
- One Python orchestration wrapper, `scripts/efisp-package.py` — the
  off-device builder of a ready-to-flash EFISP payload.
- A GitHub Actions workflow that cross-builds the binaries and *runs* them on
  real `windows-latest` and `macos-latest` runners.

**Out of scope:**

- Vendoring or committing the Windows/macOS binaries (built on demand; the
  Android binaries are vendored only because the recovery ZIP consumes them —
  the desktop binaries have no in-repo consumer).
- Publishing GitHub *Releases* with attached binaries (the workflow uploads
  build artifacts; release automation is a later concern).
- A graft wrapper — `vbmeta-graft graft` is already a clean one-shot (see §6).
- Using Windows or macOS as a *build host* — the build host stays Linux+docker;
  Windows/macOS are cross-compile *targets* only.
- The Android build path — left exactly as-is (NDK + its docker-cross-built
  static liblzma already work).

## 2. The tools, and their portability

| Tool | External dep | Notes |
|---|---|---|
| `abl-patcher`   | none | uses `getopt_long` — supplied by every zig target libc |
| `gbl-pack`      | none | pure C |
| `gbl-commit`    | none | uses `<unistd.h>` — supplied by every zig target libc |
| `vbmeta-graft`  | none | pure C |
| `mode2-profile` | none | pure C + vendored `tomlc99` |
| `fv-unwrap`     | **liblzma** | the only tool needing a cross-built library |

A portability scan found **no `mmap`** and no other platform-only construct.
Five of six tools are dependency-free pure C; only `fv-unwrap` needs an
external library. `mode2-profile.py` is pure Python (3.11+) and runs on any
host unmodified — it is not compiled.

## 3. Components

### a. `zig` cross-toolchain in the docker image

A single pinned `zig` release is added to `docker/Dockerfile` — downloaded and
unpacked like the Android NDK already is, with its directory put on `PATH`.
`zig cc` then serves as the C compiler for every non-Android cross target. The
NDK and the Android build path are untouched.

`zig` is chosen because it is the one toolchain that cleanly cross-compiles
*both* Windows and macOS from a Linux host: it bundles the mingw-w64 headers
for `*-windows-gnu` and the macOS SDK headers for `*-macos`, with no
license-restricted SDK to provision.

### b. Per-tool `make` targets

Each tool's `Makefile` already has a default `host` target and an `android`
target. Three more are added, each a near-twin of the `android` target:

| Target | Compiler invocation | Output (in the tool dir) |
|---|---|---|
| `windows`    | `zig cc -target x86_64-windows-gnu` | `<tool>.exe` |
| `macos-x64`  | `zig cc -target x86_64-macos`        | `<tool>-macos-x64` |
| `macos-arm64`| `zig cc -target aarch64-macos`       | `<tool>-macos-arm64` |

The per-target `CFLAGS` mirror each tool's host `CFLAGS` — same `-D` defines,
`-I` include paths, `-std`. The cross targets keep `-Werror` where the host
build has it; any warning a target surfaces is fixed in the code portably, not
suppressed. (One known case to resolve at implementation time:
`gbl-commit`'s `-D_FILE_OFFSET_BITS=64` is a glibc-ism — if a target libc
rejects or ignores it, it is dropped for that target only; these tools operate
on small image files, so 64-bit offsets are not load-bearing.)

`mode2-profile`'s two-step build (warnings suppressed only for the vendored
`tomlc99` translation unit) is replicated for each cross target. Each
`Makefile`'s `clean` target is extended to remove the new artifacts.

The arch-specific files (`<tool>.exe`, `<tool>-macos-x64`,
`<tool>-macos-arm64`) live in the tool directory as intermediates — exactly as
`<tool>-android` does today; the build script (§3d) installs the final-named
artifacts into `dist/`.

### c. `fv-unwrap`'s liblzma — cross-built per target

`docker/Dockerfile` already cross-builds a static `liblzma.a` for aarch64-
Android. Three more static `liblzma.a` builds are added — for
`x86_64-windows-gnu`, `x86_64-macos`, and `aarch64-macos` — each built from
the same xz-utils source with `zig cc` as the configure `CC`
(`./configure --host=<triple> CC="zig cc -target <zig-triple>" --disable-shared
--enable-static --prefix=/opt/liblzma-<target>`). Three `ENV` variables
(`LIBLZMA_WINDOWS`, `LIBLZMA_MACOS_X64`, `LIBLZMA_MACOS_ARM64`) name the
install prefixes; `fv-unwrap`'s per-target `Makefile` rules link the matching
static archive, exactly as `fv-unwrap-android` links `LIBLZMA_ANDROID`.

### d. macOS universal binaries

A macOS user should get one file that runs on both Intel and Apple-Silicon
Macs. After the two per-arch macOS builds, the build script combines them with
`llvm-lipo` (from the `llvm` apt package, added to the Dockerfile) into a
single universal `<tool>` written to `dist/macos/`.

### e. `scripts/build-cross-tools.sh`

A new build script, sibling to `scripts/build-recovery-tools.sh`. Usage:

```
build-cross-tools.sh windows | macos | all
```

It runs the docker image and, for each of the six tools:

- `windows`: `make windows` → install `<tool>.exe` to `dist/windows/<tool>.exe`.
- `macos`: `make macos-x64` + `make macos-arm64`, then `llvm-lipo -create` the
  two into a universal `dist/macos/<tool>`.
- `all`: both of the above.

It writes a `SHA256SUMS` into each populated `dist/<os>/` directory. `dist/` is
already git-ignored — these binaries are built on demand, never committed.

### f. `scripts/efisp-package.py`

A cross-platform Python (3.11+) orchestration wrapper — the off-device
equivalent of what the install ZIP's `build_payload` does on a phone. It
chains four to five of the C tools into one ready-to-flash EFISP payload.

```
efisp-package.py --abl <abl.img> --mode {0,1,2} --efi <mode-N.efi>
                 [--stock-vbmeta <vbmeta.img>] [--oem <id>]
                 [--tools-dir <dir>] [--out <path>]
```

Pipeline (see §4 for the per-mode detail):

1. `fv-unwrap <abl.img>` → `extracted.efi`
2. `abl-patcher` → `patched.efi` (mode-correct flags)
3. mode-2 only: `mode2-profile derive` then `compile` → `profile.bin`
4. `gbl-pack` (with `--mode2-profile` for mode-2) → `payload.bin`
5. concatenate `<mode-N.efi> + payload.bin` → the output `installed.efi`

It locates the C tools via `--tools-dir`, defaulting to auto-detection: the
current platform's `dist/` directory, then the script's own directory, then
`PATH`. It validates every input *before* running any tool, fails fast with a
precise message, cleans up its scratch directory, and never leaves a partial
output. It only produces the artifact — flashing is the user's manual step
(`fastboot stage` + `oem boot-efi`, per the project's safety rule).

This is the only wrapper. `vbmeta-graft graft` is already a clean one-shot and
gets no wrapper (§6).

### g. `tools/README.md`

One consolidated file replacing the lone `tools/mode2-profile/README.md`
(whose content is folded in and the file deleted — one source of truth):

- **Intro** — what the host-side tools are and their role in the
  EFISP / GBLP1 / vbmeta workflow.
- **Building** — prerequisites (docker), and the build flavors:
  - *host* — `make -C tools/<t>` → the local Linux dev binary.
  - *android* — `scripts/build-recovery-tools.sh` → `dist/recovery/`.
  - *windows / macos* — `scripts/build-cross-tools.sh [windows|macos|all]` →
    `dist/windows/`, `dist/macos/`.
- **Per-tool sections** (×6) — purpose, CLI synopsis, key options, a worked
  example. `vbmeta-graft`'s section documents `graft` as the simple host path
  and `check` as the optional safety verification (and that `check` needs the
  device's main `vbmeta.img`, while `graft` does not).
- **`efisp-package.py`** — usage and a worked example per mode.
- **Platform matrix** — each tool × {Linux, Android, Windows, macOS}, plus a
  note that `mode2-profile.py` is the pure-Python alternative (any host with
  Python 3.11+, no build).

### h. GitHub Actions CI

A new workflow, `.github/workflows/host-tools.yml`, triggered on push/PR that
touches `tools/**`, `scripts/build-cross-tools.sh`, `docker/**`, or the
workflow file itself, plus manual `workflow_dispatch`:

- **`cross-build`** (`ubuntu-latest`) — builds the docker image, runs
  `build-cross-tools.sh all`, and uploads `dist/windows/` and `dist/macos/` as
  workflow artifacts.
- **`test-windows`** (`windows-latest`, needs `cross-build`) — downloads the
  Windows artifacts and smoke-tests each `.exe` on a real Windows host.
- **`test-macos`** (`macos-latest`, needs `cross-build`) — same, on a real
  Mac; the universal binary runs natively whatever the runner's arch.

The smoke test per tool is a usage/`--help` invocation, or a minimal real
operation on a checked-out repo fixture — the exact per-tool invocation is
pinned during planning (some tools exit non-zero on a bare no-arg call). This
gives genuine cross-platform *behaviour* coverage, beyond artifact-format
checks.

## 4. The `efisp-package.py` pipeline, per mode

The base EFI and the ABL patch set depend on the mode — `efisp-package.py`
mirrors the zip `install-common.sh` parametrization:

| `--mode` | base `--efi` | `abl-patcher` flags | profile |
|---|---|---|---|
| 0 | `mode-0.efi` | `--no-mode1` (universal only) | — |
| 1 | `mode-1.efi` | none (universal + mode_1) | — |
| 2 | `mode-2.efi` | `--oem <id> --no-mode1` (universal + oem) | derive+compile from `--stock-vbmeta` |

For `--mode 2`, `--stock-vbmeta` and `--oem` are required; for modes 0 and 1
they are rejected if supplied. The output defaults to
`dist/efisp-payload/<abl-basename>-mode<N>.efi`, overridable with `--out`.

## 5. Testing

- **`tests/host/084_cross_build.sh`** — runs `build-cross-tools.sh all`, then
  asserts each artifact is well-formed: every `dist/windows/*.exe` is a
  `PE32+ … x86-64` executable and every `dist/macos/*` is a `Mach-O universal`
  binary carrying both an `x86_64` and an `arm64` slice (checked with `file`
  and `llvm-lipo -archs`). It validates artifacts *exist and are well-formed*,
  not behaviour — the cross-built binaries cannot run on a Linux box.
  `SKIP`s cleanly when docker is unavailable. This doubles as what the CI
  `cross-build` job runs.
- **`efisp-package.py`** — a host test builds a payload from a fixture ABL for
  each of the three modes and asserts the output is a PE with a locatable
  GBLP1 overlay (reusing the existing `gbl-pack`/parser test fixtures).
- **The CI `test-windows` / `test-macos` jobs** — the real cross-platform
  *behaviour* coverage: the binaries actually execute on Windows and macOS.
- The existing per-tool host tests continue to cover each tool's logic on the
  native Linux binary, unchanged.

## 6. Why no graft wrapper

`vbmeta-graft` has three subcommands with distinct inputs:

- `graft --stock <stock-partition-img> --custom <custom-img> --part-size <N>
  --out <o>` — extracts the OEM-signed vbmeta blob from the *stock partition's
  own `AvbFooter`* and pastes it at the 4 KiB-rounded offset. It needs **no**
  device main `vbmeta.img`.
- `check <candidate-img> <main-vbmeta-img> <part>` — the *separate* suitability
  gate that verifies the candidate's key matches the device main vbmeta's
  chain descriptor. This is the one that needs `vbmeta.img`.
- `list <img>` — descriptor dump.

So plain `graft` is already the easy, low-ceremony host operation: two image
inputs and a partition size, no `vbmeta.img`, the user owning the risk of
having picked the right stock partition. There is nothing for a wrapper to
simplify — the stock partition input is irreducible (the OEM-signed blob
cannot be synthesized — see `docs/project/vbmeta-graft-vs-construct.md`).
`vbmeta-graft` cross-compiles with the other five tools; `tools/README.md`
documents `graft` as the host path and `check` as the optional precaution.

## 7. Decomposition

This is a single coherent slice, implementable as one plan:

1. **Dockerfile** — add `zig`, `llvm-lipo`, and the three cross-built static
   liblzma archives.
2. **Per-tool `make` targets** — `windows` / `macos-x64` / `macos-arm64` for
   all six tools.
3. **`scripts/build-cross-tools.sh`** + `tests/host/084_cross_build.sh`.
4. **`scripts/efisp-package.py`** + its host test.
5. **`tools/README.md`** — consolidated; delete `mode2-profile/README.md`.
6. **`.github/workflows/host-tools.yml`** — cross-build + run-on-real-OS CI.

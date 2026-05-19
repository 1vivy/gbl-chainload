# Host-side Tools — Windows/macOS Cross-Compilation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the six host-side C tools buildable for Windows and macOS, document them in one `tools/README.md`, add an `efisp-package.py` orchestration wrapper, and wire run-on-real-OS GitHub Actions CI.

**Architecture:** A single `zig` cross-toolchain is added to the docker build image; each tool's `Makefile` gains `windows`/`macos-x64`/`macos-arm64` targets via `zig cc -target …`. `fv-unwrap`'s `liblzma` dependency is satisfied by static `liblzma.a` archives cross-built per target in the Dockerfile. A `build-cross-tools.sh` script drives the docker build and `llvm-lipo`-merges the macOS arches into universal binaries. The Android (NDK) build path is left untouched.

**Tech Stack:** C (C99/C11), GNU Make, `zig cc`, Docker, xz-utils/liblzma, Python 3.11+, GitHub Actions, Bash.

**Spec:** `docs/superpowers/specs/2026-05-19-host-tools-cross-compile-design.md`

**Branch:** `host-tools-cross-compile` (off `main` — slice 4 is independent of the in-flight slice-5 PR #30).

**Test numbering note:** this branch is off `main`, where the highest host test is `081`. The in-flight PR #30 claims `082`/`083`. This plan uses `084` and `085` to avoid a collision when the branches merge.

---

### Task 1: Dockerfile — zig toolchain + cross-built liblzma

**Goal:** The docker build image gains the `zig` cross-compiler, a PATH-resolvable `llvm-lipo`, and three static `liblzma.a` archives (Windows + macOS x64/arm64).

**Files:**
- Modify: `docker/Dockerfile`

**Acceptance Criteria:**
- [ ] `docker build -t gbl-chainload-build:latest -f docker/Dockerfile .` succeeds.
- [ ] In the image: `zig version` prints a version; `llvm-lipo --version` runs.
- [ ] In the image: `/opt/liblzma-windows/lib/liblzma.a`, `/opt/liblzma-macos-x64/lib/liblzma.a`, and `/opt/liblzma-macos-arm64/lib/liblzma.a` all exist.
- [ ] The Android NDK block and `LIBLZMA_ANDROID` are untouched.

**Verify:**
```
docker build -t gbl-chainload-build:latest -f docker/Dockerfile . \
 && docker run --rm gbl-chainload-build:latest bash -c \
   'zig version && llvm-lipo --version | head -1 && ls -l /opt/liblzma-windows/lib/liblzma.a /opt/liblzma-macos-x64/lib/liblzma.a /opt/liblzma-macos-arm64/lib/liblzma.a'
```
Expected: a zig version, an `llvm-lipo` version line, and three `liblzma.a` listings.

**Steps:**

- [ ] **Step 1: Add the `zig` toolchain block**

After the existing `ENV LIBLZMA_ANDROID=/opt/liblzma-android` line (Dockerfile line 89), and before the `WORKDIR /work` line, insert:

```dockerfile
# --- zig toolchain for Windows/macOS cross-compilation ---
# zig cc cross-compiles the host-side tools to x86_64-windows-gnu and
# x86_64/aarch64-macos from this Linux image: it bundles the mingw-w64
# and macOS SDK headers, so there is no license-restricted SDK to
# provision. The Android path stays on the NDK above, untouched.
ARG ZIG_VER=0.13.0
RUN curl -fsSL -o /tmp/zig.tar.xz \
        "https://ziglang.org/download/${ZIG_VER}/zig-linux-x86_64-${ZIG_VER}.tar.xz" \
 && mkdir -p /opt/zig \
 && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
 && rm /tmp/zig.tar.xz
ENV PATH=/opt/zig:$PATH

# llvm-lipo (from the already-installed `llvm` apt package, version-
# suffixed under /usr/lib/llvm-*/bin) onto PATH for the macOS
# universal-binary merge step in build-cross-tools.sh.
RUN ln -sf "$(ls /usr/lib/llvm-*/bin/llvm-lipo | head -1)" /usr/local/bin/llvm-lipo
```

- [ ] **Step 2: Add the cross-built liblzma block**

Immediately after the `zig`/`llvm-lipo` block from Step 1, insert:

```dockerfile
# --- Static liblzma for the Windows/macOS cross targets ---
# fv-unwrap needs LZMA decode; zig's bundled target sysroots have no
# liblzma. Cross-build xz-utils once per target with zig cc as CC
# (llvm-ar/llvm-ranlib are object-format-agnostic). Same xz $(XZ_VER)
# as the Android block above.
RUN set -eux; \
    curl -fsSL -o /tmp/xz.tar.gz \
      "https://github.com/tukaani-project/xz/releases/download/v${XZ_VER}/xz-${XZ_VER}.tar.gz"; \
    for spec in \
        "x86_64-windows-gnu:x86_64-w64-mingw32:/opt/liblzma-windows" \
        "x86_64-macos:x86_64-apple-darwin:/opt/liblzma-macos-x64" \
        "aarch64-macos:aarch64-apple-darwin:/opt/liblzma-macos-arm64" ; do \
      ztgt="${spec%%:*}"; rest="${spec#*:}"; \
      host="${rest%%:*}"; prefix="${rest#*:}"; \
      rm -rf /tmp/xz && mkdir -p /tmp/xz; \
      tar -xf /tmp/xz.tar.gz -C /tmp/xz --strip-components=1; \
      cd /tmp/xz; \
      ./configure \
        --host="$host" \
        --prefix="$prefix" \
        --enable-static --disable-shared \
        --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
        --disable-lzma-links --disable-scripts --disable-doc \
        --disable-nls --disable-threads \
        CC="zig cc -target $ztgt" \
        AR=llvm-ar RANLIB=llvm-ranlib \
        CFLAGS="-O2"; \
      make -j"$(nproc)"; make install; \
      cd /; \
    done; \
    rm -rf /tmp/xz /tmp/xz.tar.gz
ENV LIBLZMA_WINDOWS=/opt/liblzma-windows
ENV LIBLZMA_MACOS_X64=/opt/liblzma-macos-x64
ENV LIBLZMA_MACOS_ARM64=/opt/liblzma-macos-arm64
```

Note: `XZ_VER` is the `ARG XZ_VER=5.6.3` already declared in the Android block above — it is in scope for the rest of this single build stage, so it does not need re-declaring.

- [ ] **Step 3: Build and verify**

Run the **Verify** command above. If the `zig` tarball URL 404s, the only likely cause is a version/naming change — `zig` ≤ 0.13.0 uses `zig-linux-x86_64-<ver>.tar.xz`; bump `ZIG_VER` to another ≤0.13.x release if needed. If a liblzma `./configure` mis-detects the cross compiler, the fix is almost always a missing `--host` recognition — confirm the `--host` triple is a standard one (`x86_64-w64-mingw32`, `x86_64-apple-darwin`, `aarch64-apple-darwin` all are).

- [ ] **Step 4: Commit**

```bash
git add docker/Dockerfile
git commit -m "build: add zig + cross-built liblzma to the docker image"
```

---

### Task 2: Windows/macOS make targets — the five dependency-free tools

**Goal:** `abl-patcher`, `gbl-pack`, `gbl-commit`, `vbmeta-graft`, and `mode2-profile` each gain `windows`, `macos-x64`, and `macos-arm64` make targets that cross-compile via `zig cc`.

**Files:**
- Modify: `tools/abl-patcher/Makefile`, `tools/gbl-pack/Makefile`, `tools/gbl-commit/Makefile`, `tools/vbmeta-graft/Makefile`, `tools/mode2-profile/Makefile`
- Modify: `.gitignore`

**Acceptance Criteria:**
- [ ] In the docker image, `make -C tools/<t> windows` produces `tools/<t>/<t>.exe` for each of the five tools.
- [ ] In the docker image, `make -C tools/<t> macos-x64` and `macos-arm64` produce `tools/<t>/<t>-macos-x64` / `-macos-arm64`.
- [ ] `file tools/<t>/<t>.exe` reports `PE32+ … x86-64`; the macOS outputs report `Mach-O … x86_64` / `arm64`.
- [ ] Each `Makefile`'s `clean` target removes the new artifacts.
- [ ] `.gitignore` ignores the new `*.exe` / `*-macos-*` artifacts.
- [ ] The existing `host` and `android` targets still build (`make -C tools/<t>` and `make -C tools/<t> android`).

**Verify:** (inside the docker image, from the repo root)
```
docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  for t in abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile; do
    make -C tools/$t clean
    make -C tools/$t windows && file tools/$t/$t.exe | grep -q "PE32+"
    make -C tools/$t macos-x64 && file tools/$t/$t-macos-x64 | grep -q "Mach-O"
    make -C tools/$t macos-arm64 && file tools/$t/$t-macos-arm64 | grep -q "Mach-O"
    echo "ok $t"
  done'
```
Expected: `ok abl-patcher` … `ok mode2-profile`, no errors.

**Steps:**

- [ ] **Step 1: `tools/gbl-pack/Makefile` — append cross targets**

After the `clean:` target, append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -DGBL_HOST_BUILD=1 -I$(GPL)
XSRCS   = gbl-pack.c pack.c $(PARSER_SRCS_NODEP) $(GPL)/Sha256.c

windows:     gbl-pack.exe
macos-x64:   gbl-pack-macos-x64
macos-arm64: gbl-pack-macos-arm64

gbl-pack.exe: $(XSRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(XSRCS)

gbl-pack-macos-x64: $(XSRCS)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(XSRCS)

gbl-pack-macos-arm64: $(XSRCS)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(XSRCS)
```

Then change the `clean:` recipe to:
```makefile
clean:
	rm -f gbl-pack gbl-pack-android gbl-pack.exe \
	      gbl-pack-macos-x64 gbl-pack-macos-arm64 Sha256.o
```

- [ ] **Step 2: `tools/gbl-commit/Makefile` — append cross targets**

After `clean:`, append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c99 -D_FILE_OFFSET_BITS=64 \
          -DGBL_HOST_BUILD=1 -I$(GPL)
XSRCS   = gbl-commit.c $(GPL)/Sha256.c

windows:     gbl-commit.exe
macos-x64:   gbl-commit-macos-x64
macos-arm64: gbl-commit-macos-arm64

gbl-commit.exe: $(XSRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ $(XSRCS)

gbl-commit-macos-x64: $(XSRCS)
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ $(XSRCS)

gbl-commit-macos-arm64: $(XSRCS)
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ $(XSRCS)
```

Change `clean:` to:
```makefile
clean:
	rm -f gbl-commit gbl-commit-android gbl-commit.exe \
	      gbl-commit-macos-x64 gbl-commit-macos-arm64
```

- [ ] **Step 3: `tools/abl-patcher/Makefile` — append cross targets**

After `clean:`, append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
          -D__HOST_BUILD__ -DGBL_MODE=1 \
          -I$(PROJ)/GblChainloadPkg/Include/Library -I$(DPL)/Internal

windows:     abl-patcher.exe
macos-x64:   abl-patcher-macos-x64
macos-arm64: abl-patcher-macos-arm64

abl-patcher.exe: $(SRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $^ -o $@

abl-patcher-macos-x64: $(SRCS)
	zig cc -target x86_64-macos $(XCFLAGS) $^ -o $@

abl-patcher-macos-arm64: $(SRCS)
	zig cc -target aarch64-macos $(XCFLAGS) $^ -o $@
```

Change `clean:` to:
```makefile
clean:
	rm -f abl-patcher abl-patcher-android abl-patcher.exe \
	      abl-patcher-macos-x64 abl-patcher-macos-arm64 *.o
```

- [ ] **Step 4: `tools/vbmeta-graft/Makefile` — append cross targets**

After `clean:`, append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

windows:     vbmeta-graft.exe
macos-x64:   vbmeta-graft-macos-x64
macos-arm64: vbmeta-graft-macos-arm64

vbmeta-graft.exe: $(SRCS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) $^ -o $@

vbmeta-graft-macos-x64: $(SRCS)
	zig cc -target x86_64-macos $(XCFLAGS) $^ -o $@

vbmeta-graft-macos-arm64: $(SRCS)
	zig cc -target aarch64-macos $(XCFLAGS) $^ -o $@
```

Change `clean:` to:
```makefile
clean:
	rm -f vbmeta-graft vbmeta-graft-android vbmeta-graft.exe \
	      vbmeta-graft-macos-x64 vbmeta-graft-macos-arm64
```

- [ ] **Step 5: `tools/mode2-profile/Makefile` — append cross targets**

`mode2-profile` builds in two steps so the vendored `tomlc99` translation unit can be compiled with warnings suppressed (`-w`). Each cross target is written out explicitly — no canned-recipe `define`, which is finicky to get right. After `clean:`, append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
XCFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 -I$(GPL)

windows:     mode2-profile.exe
macos-x64:   mode2-profile-macos-x64
macos-arm64: mode2-profile-macos-arm64

mode2-profile.exe: $(SRCS) $(HDRS)
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -c -o m2p-win.o mode2-profile.c
	zig cc -target x86_64-windows-gnu -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-win.o vendor/tomlc99/toml.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -c -o sha256-win.o $(GPL)/Sha256.c
	zig cc -target x86_64-windows-gnu $(XCFLAGS) -o $@ \
	    m2p-win.o toml-win.o sha256-win.o

mode2-profile-macos-x64: $(SRCS) $(HDRS)
	zig cc -target x86_64-macos $(XCFLAGS) -c -o m2p-macx64.o mode2-profile.c
	zig cc -target x86_64-macos -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-macx64.o vendor/tomlc99/toml.c
	zig cc -target x86_64-macos $(XCFLAGS) -c -o sha256-macx64.o $(GPL)/Sha256.c
	zig cc -target x86_64-macos $(XCFLAGS) -o $@ \
	    m2p-macx64.o toml-macx64.o sha256-macx64.o

mode2-profile-macos-arm64: $(SRCS) $(HDRS)
	zig cc -target aarch64-macos $(XCFLAGS) -c -o m2p-macarm64.o mode2-profile.c
	zig cc -target aarch64-macos -w -O2 -std=c11 -DGBL_HOST_BUILD=1 \
	    -c -o toml-macarm64.o vendor/tomlc99/toml.c
	zig cc -target aarch64-macos $(XCFLAGS) -c -o sha256-macarm64.o $(GPL)/Sha256.c
	zig cc -target aarch64-macos $(XCFLAGS) -o $@ \
	    m2p-macarm64.o toml-macarm64.o sha256-macarm64.o
```

Change `clean:` to:
```makefile
clean:
	rm -f mode2-profile mode2-profile-android mode2-profile.o toml.o sha256.o \
	      mode2-profile-android.o toml-android.o sha256-android.o \
	      mode2-profile.exe mode2-profile-macos-x64 mode2-profile-macos-arm64 \
	      m2p-*.o toml-*.o sha256-*.o
```

- [ ] **Step 6: `.gitignore` — ignore the new artifacts**

Append to `.gitignore`:
```
# host-tool cross-compile artifacts
tools/*/*.exe
tools/*/*-macos-x64
tools/*/*-macos-arm64
```

- [ ] **Step 7: Build and verify**

Run the **Verify** command above. If `zig cc` surfaces a `-Werror` warning (gbl-pack / gbl-commit / mode2-profile) that the native build does not, fix it in the C source portably — do not delete `-Werror` from `XCFLAGS`.

- [ ] **Step 8: Commit**

```bash
git add tools/abl-patcher/Makefile tools/gbl-pack/Makefile tools/gbl-commit/Makefile \
        tools/vbmeta-graft/Makefile tools/mode2-profile/Makefile .gitignore
git commit -m "build: windows/macos make targets for the dependency-free tools"
```

---

### Task 3: `fv-unwrap` Windows/macOS make targets (liblzma-linked)

**Goal:** `fv-unwrap` gains `windows`/`macos-x64`/`macos-arm64` targets, each linking the matching cross-built static `liblzma.a` from Task 1.

**Files:**
- Modify: `tools/fv-unwrap/Makefile`

**Acceptance Criteria:**
- [ ] In the docker image, `make -C tools/fv-unwrap windows` produces `tools/fv-unwrap/fv-unwrap.exe` (`file` → `PE32+ … x86-64`).
- [ ] `macos-x64` / `macos-arm64` produce `Mach-O` binaries for the respective arch.
- [ ] The new targets link `$(LIBLZMA_WINDOWS|MACOS_X64|MACOS_ARM64)/lib/liblzma.a` statically.
- [ ] `clean` removes the new artifacts; the `host` and `android` targets still build.

**Verify:** (inside the docker image)
```
docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  make -C tools/fv-unwrap clean
  make -C tools/fv-unwrap windows     && file tools/fv-unwrap/fv-unwrap.exe | grep -q "PE32+"
  make -C tools/fv-unwrap macos-x64   && file tools/fv-unwrap/fv-unwrap-macos-x64 | grep -q "Mach-O"
  make -C tools/fv-unwrap macos-arm64 && file tools/fv-unwrap/fv-unwrap-macos-arm64 | grep -q "Mach-O"
  make -C tools/fv-unwrap android     && file tools/fv-unwrap/fv-unwrap-android | grep -q "ELF"
  echo "ok fv-unwrap"'
```
Expected: `ok fv-unwrap`.

**Steps:**

- [ ] **Step 1: Append the cross targets to `tools/fv-unwrap/Makefile`**

After the existing `fv-unwrap-android:` rule (end of file), append:

```makefile
# --- Windows / macOS cross-compile (zig cc) ---
# Links the static liblzma cross-compiled per target by docker/Dockerfile
# into $(LIBLZMA_WINDOWS|MACOS_X64|MACOS_ARM64) — the same -llzma role as
# the host (-llzma) and android ($(LIBLZMA_ANDROID)) builds.
XCFLAGS = -O2 -Wall -Wextra -std=c11

windows:     fv-unwrap.exe
macos-x64:   fv-unwrap-macos-x64
macos-arm64: fv-unwrap-macos-arm64

fv-unwrap.exe: fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a
	zig cc -target x86_64-windows-gnu $(XCFLAGS) \
	    -I$(LIBLZMA_WINDOWS)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_WINDOWS)/lib/liblzma.a

fv-unwrap-macos-x64: fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a
	zig cc -target x86_64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_X64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_X64)/lib/liblzma.a

fv-unwrap-macos-arm64: fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a
	zig cc -target aarch64-macos $(XCFLAGS) \
	    -I$(LIBLZMA_MACOS_ARM64)/include -o $@ \
	    fv-unwrap.c $(LIBLZMA_MACOS_ARM64)/lib/liblzma.a
```

Change the `clean:` recipe to:
```makefile
clean:
	rm -f fv-unwrap fv-unwrap-android fv-unwrap.exe \
	      fv-unwrap-macos-x64 fv-unwrap-macos-arm64 *.o
```

- [ ] **Step 2: Build and verify**

Run the **Verify** command above. The `LIBLZMA_*` variables are Dockerfile `ENV`s (Task 1) — they resolve only inside the image, which is where these targets are built.

- [ ] **Step 3: Commit**

```bash
git add tools/fv-unwrap/Makefile
git commit -m "build: windows/macos make targets for fv-unwrap (liblzma-linked)"
```

---

### Task 4: `build-cross-tools.sh` + the `084` cross-build test

**Goal:** A `scripts/build-cross-tools.sh` driver produces `dist/windows/*.exe` and macOS universal binaries in `dist/macos/`; `tests/host/084` verifies all six artifacts are well-formed.

**Files:**
- Create: `scripts/build-cross-tools.sh`
- Create: `tests/host/084_cross_build.sh`

**Acceptance Criteria:**
- [ ] `build-cross-tools.sh windows` populates `dist/windows/<tool>.exe` for all six tools + a `SHA256SUMS`.
- [ ] `build-cross-tools.sh macos` populates `dist/macos/<tool>` (universal: x86_64 + arm64) for all six + a `SHA256SUMS`.
- [ ] `build-cross-tools.sh all` does both; an invalid arg exits non-zero with a usage message.
- [ ] `tests/host/084_cross_build.sh` PASSes: every `dist/windows/*.exe` is `PE32+ … x86-64`, every `dist/macos/*` is a `Mach-O universal` with both an `x86_64` and an `arm64` slice. It `SKIP`s cleanly when docker or the image is unavailable.

**Verify:** `bash tests/host/084_cross_build.sh` → `PASS: 084 cross-build` (or a `SKIP:` line)

**Steps:**

- [ ] **Step 1: Write `scripts/build-cross-tools.sh`**

```bash
#!/usr/bin/env bash
# scripts/build-cross-tools.sh — cross-compile the six host-side tools for
# Windows and/or macOS inside the docker build image. Outputs to
# dist/windows/ (<tool>.exe) and dist/macos/ (universal <tool>).
#
#   build-cross-tools.sh windows | macos | all
#
# Sibling of build-recovery-tools.sh (which builds the aarch64 Android
# tools). dist/ is git-ignored — these binaries are built on demand.
set -euo pipefail
cd "$(dirname "$0")/.."

OS="${1:-}"
case "$OS" in
  windows|macos|all) ;;
  *) echo "usage: $0 windows|macos|all" >&2; exit 2 ;;
esac

# WSL/Docker-Desktop credential-helper quirk: an empty DOCKER_CONFIG dir
# avoids the desktop.exe credstore lookup that fails under WSL.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$(mktemp -d)}"

TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile"

docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  OS="'"$OS"'"
  TOOLS="'"$TOOLS"'"
  if [ "$OS" = windows ] || [ "$OS" = all ]; then
    mkdir -p dist/windows
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t windows
      install -Dm755 tools/$t/$t.exe dist/windows/$t.exe
    done
    ( cd dist/windows && sha256sum *.exe > SHA256SUMS )
  fi
  if [ "$OS" = macos ] || [ "$OS" = all ]; then
    mkdir -p dist/macos
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t macos-x64
      make -C tools/$t macos-arm64
      llvm-lipo -create -output dist/macos/$t \
        tools/$t/$t-macos-x64 tools/$t/$t-macos-arm64
    done
    ( cd dist/macos && sha256sum $TOOLS > SHA256SUMS )
  fi
'

echo "==> cross-build done"
[ -d dist/windows ] && ls -la dist/windows
[ -d dist/macos ]   && ls -la dist/macos
exit 0
```

Make it executable: `chmod +x scripts/build-cross-tools.sh`.

- [ ] **Step 2: Write `tests/host/084_cross_build.sh`**

```bash
#!/usr/bin/env bash
# tests/host/084_cross_build.sh — the six host-side tools cross-compile to
# Windows and macOS and the artifacts are well-formed PE32+ / Mach-O
# universal binaries. Artifact-format check only — the cross binaries
# cannot run on a Linux box (real-OS behaviour is the CI job's concern).
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v docker >/dev/null 2>&1 \
  || { echo "SKIP: 084 — docker not available"; exit 0; }
docker image inspect gbl-chainload-build:latest >/dev/null 2>&1 \
  || { echo "SKIP: 084 — gbl-chainload-build:latest image not built"; exit 0; }

bash scripts/build-cross-tools.sh all

TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile"
for t in $TOOLS; do
  win="dist/windows/$t.exe"
  [ -f "$win" ] || { echo "FAIL: $win not produced"; exit 1; }
  fw=$(file -b "$win")
  case "$fw" in
    *PE32+*x86-64*) ;;
    *) echo "FAIL: $win is not PE32+ x86-64: $fw"; exit 1 ;;
  esac

  mac="dist/macos/$t"
  [ -f "$mac" ] || { echo "FAIL: $mac not produced"; exit 1; }
  fm=$(file -b "$mac")
  case "$fm" in
    *"Mach-O universal"*) ;;
    *) echo "FAIL: $mac is not a Mach-O universal binary: $fm"; exit 1 ;;
  esac
  case "$fm" in *x86_64*) ;; *) echo "FAIL: $mac missing x86_64 slice"; exit 1 ;; esac
  case "$fm" in *arm64*)  ;; *) echo "FAIL: $mac missing arm64 slice";  exit 1 ;; esac
done

echo "PASS: 084 cross-build"
```

- [ ] **Step 3: Verify**

Run: `bash tests/host/084_cross_build.sh`
Expected: `PASS: 084 cross-build` (requires the Task-1 docker image; otherwise a `SKIP:` line).

- [ ] **Step 4: Commit**

```bash
git add scripts/build-cross-tools.sh tests/host/084_cross_build.sh
git commit -m "build: build-cross-tools.sh + 084 cross-build test"
```

---

### Task 5: `efisp-package.py` orchestration wrapper + host test

**Goal:** A cross-platform Python wrapper, `scripts/efisp-package.py`, chains the host-side tools into one ready-to-flash EFISP payload; a host test exercises it for each mode.

**Files:**
- Create: `scripts/efisp-package.py`
- Create: `tests/host/085_efisp_package.sh`

**Acceptance Criteria:**
- [ ] `efisp-package.py --abl <img> --mode 1 --efi <mode-1.efi>` runs `fv-unwrap → abl-patcher → gbl-pack → concat` and writes a PE output with a locatable GBLP1 cached-ABL overlay.
- [ ] `--mode 0` adds `--no-mode1` to `abl-patcher`; `--mode 2` requires `--stock-vbmeta` + `--oem`, runs `mode2-profile derive`+`compile`, and passes `--mode2-profile` to `gbl-pack`.
- [ ] Missing/invalid inputs (`--mode 2` without `--stock-vbmeta`, `--stock-vbmeta` given for mode 0/1, missing tool, unreadable file) abort non-zero with a precise message *before* any tool runs; the scratch dir is cleaned up; no partial output.
- [ ] Tools are located via `--tools-dir`, else auto-detect (`dist/<platform>/`, the script's dir, then `PATH`); a `.exe` suffix is honored on Windows.
- [ ] `tests/host/085_efisp_package.sh` PASSes (or `SKIP`s when no fixture ABL is present).

**Verify:** `bash tests/host/085_efisp_package.sh` → `PASS: 085 efisp package` (or a `SKIP:` line)

**Steps:**

- [ ] **Step 1: Write `scripts/efisp-package.py`**

```python
#!/usr/bin/env python3
"""efisp-package.py — build a ready-to-flash EFISP payload off-device.

Chains the host-side tools into a single `mode-N.efi + GBLP1 overlay`
image, the off-device equivalent of the install ZIP's build_payload:

    fv-unwrap  <abl.img>            -> extracted.efi
    abl-patcher (mode-correct flags) -> patched.efi
    mode2-profile derive+compile     -> profile.bin     (mode 2 only)
    gbl-pack    (overlay)            -> payload.bin
    cat mode-N.efi payload.bin       -> <out>

The output is produced only; flashing is the user's manual step
(`fastboot stage` + `oem boot-efi`).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = ("fv-unwrap", "abl-patcher", "gbl-pack", "mode2-profile")


def die(msg):
    sys.stderr.write(f"efisp-package: error: {msg}\n")
    sys.exit(1)


def platform_dist_dir():
    """The dist/ subdir holding this platform's built tools, or None."""
    sub = {"win32": "windows", "darwin": "macos"}.get(sys.platform)
    if not sub:
        return None    # a Linux host build lives in tools/<t>/, not dist/
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, "dist", sub)


def find_tool(name, tools_dir):
    """Locate a tool binary: --tools-dir, the platform dist/ dir, the
    script's own dir, then PATH."""
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = []
    if tools_dir:
        candidates.append(os.path.join(tools_dir, exe))
    pdd = platform_dist_dir()
    if pdd:
        candidates.append(os.path.join(pdd, exe))
    candidates.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), exe))
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    found = shutil.which(exe)
    if found:
        return found
    die(f"tool '{name}' not found "
        f"(looked in --tools-dir, dist/<platform>/, the script dir, PATH)")


def run(argv, label):
    """Run a tool; abort with its output on failure."""
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        sys.stderr.write(res.stderr)
        die(f"{label} failed (exit {res.returncode})")


def main():
    ap = argparse.ArgumentParser(
        prog="efisp-package.py",
        description="Build a ready-to-flash EFISP payload off-device.")
    ap.add_argument("--abl", required=True, help="dumped ABL partition image")
    ap.add_argument("--mode", required=True, choices=("0", "1", "2"))
    ap.add_argument("--efi", required=True, help="base mode-N.efi")
    ap.add_argument("--stock-vbmeta", help="stock vbmeta image (mode 2 only)")
    ap.add_argument("--oem", help="OEM id for abl-patcher --oem (mode 2 only)")
    ap.add_argument("--tools-dir", help="directory holding the host-side tool binaries")
    ap.add_argument("--out", help="output path "
                    "(default: dist/efisp-payload/<abl>-mode<N>.efi)")
    args = ap.parse_args()

    # --- pre-flight: every gate fires before any tool runs ---
    for f in (args.abl, args.efi):
        if not os.path.isfile(f):
            die(f"input not found: {f}")
    if args.mode == "2":
        if not args.stock_vbmeta or not args.oem:
            die("--mode 2 requires --stock-vbmeta and --oem")
        if not os.path.isfile(args.stock_vbmeta):
            die(f"input not found: {args.stock_vbmeta}")
    else:
        if args.stock_vbmeta or args.oem:
            die("--stock-vbmeta / --oem are only valid for --mode 2")

    fv     = find_tool("fv-unwrap", args.tools_dir)
    patch  = find_tool("abl-patcher", args.tools_dir)
    pack   = find_tool("gbl-pack", args.tools_dir)
    m2p    = find_tool("mode2-profile", args.tools_dir) if args.mode == "2" else None

    out = args.out or os.path.join(
        "dist", "efisp-payload",
        f"{os.path.splitext(os.path.basename(args.abl))[0]}-mode{args.mode}.efi")

    tmp = tempfile.mkdtemp(prefix="efisp-package.")
    try:
        extracted = os.path.join(tmp, "extracted.efi")
        patched   = os.path.join(tmp, "patched.efi")
        payload   = os.path.join(tmp, "payload.bin")

        # 1. unwrap the ABL PE out of the partition image
        run([fv, args.abl, extracted], "fv-unwrap")

        # 2. patch — mode-correct abl-patcher flags
        patch_argv = [patch, "--in", extracted, "--out", patched]
        if args.mode == "0":
            patch_argv.append("--no-mode1")
        elif args.mode == "2":
            patch_argv += ["--oem", args.oem, "--no-mode1"]
        run(patch_argv, "abl-patcher")

        # 3. mode 2: derive + compile the mode2 profile
        pack_extra = []
        if args.mode == "2":
            toml = os.path.join(tmp, "profile.toml")
            pbin = os.path.join(tmp, "profile.bin")
            run([m2p, "derive", args.stock_vbmeta, "-o", toml], "mode2-profile derive")
            run([m2p, "compile", toml, "-o", pbin], "mode2-profile compile")
            pack_extra = ["--mode2-profile", pbin]

        # 4. pack the GBLP1 overlay
        run([pack, "--cached-abl", patched, "--source", args.abl,
             "--extracted", extracted, *pack_extra, "--out", payload],
            "gbl-pack")

        # 5. concatenate base EFI + overlay -> the output payload
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "wb") as o:
            with open(args.efi, "rb") as f:
                shutil.copyfileobj(f, o)
            with open(payload, "rb") as f:
                shutil.copyfileobj(f, o)
    except BaseException:
        if os.path.isfile(out):
            os.unlink(out)        # no partial output
        raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"efisp-package: wrote {out}")


if __name__ == "__main__":
    main()
```

Make it executable: `chmod +x scripts/efisp-package.py`.

- [ ] **Step 2: Write `tests/host/085_efisp_package.sh`**

```bash
#!/usr/bin/env bash
# tests/host/085_efisp_package.sh — efisp-package.py chains the host-side
# tools into a mode-N.efi + GBLP1 overlay the EDK2 parser can locate.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Build the host tools and the parser harness this test needs.
make -s -C tools/fv-unwrap
make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/085
mkdir -p "$OUT/tools"

# efisp-package.py locates tools via --tools-dir / dist/<platform>/ /
# script-dir / PATH — a Linux host build (tools/<t>/<t>) is in none of
# those, so stage the three needed binaries into one dir and pass it.
cp tools/fv-unwrap/fv-unwrap tools/abl-patcher/abl-patcher \
   tools/gbl-pack/gbl-pack "$OUT/tools/"

# An fv-unwrap input is a raw ABL partition (LZMA-FV wrapped). Use the
# first tests/images fixture if present; otherwise the test SKIPs.
ABL=$(ls tests/images/*.img 2>/dev/null | head -1 || true)
[ -n "$ABL" ] || { echo "SKIP: 085 — no tests/images/*.img fixture present"; exit 0; }

# A throwaway base EFI: efisp-package.py just concatenates it, so any
# small file with a PE 'MZ' header is enough for the structural check.
printf 'MZ' > "$OUT/base.efi"
head -c 4096 /dev/zero >> "$OUT/base.efi"

# mode 1 — plain abl-patcher, no profile.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode1.efi" \
  >"$OUT/m1.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 1 failed"; cat "$OUT/m1.log"; exit 1; }
"$H" find-cached-abl "$OUT/mode1.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-1 output has no locatable cached-ABL overlay"; exit 1; }

# mode 0 — abl-patcher --no-mode1.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode0.efi" \
  >"$OUT/m0.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 failed"; cat "$OUT/m0.log"; exit 1; }
"$H" find-cached-abl "$OUT/mode0.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0 output has no locatable cached-ABL overlay"; exit 1; }

# pre-flight gate: mode 2 without --stock-vbmeta must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 2 --efi "$OUT/base.efi" --out "$OUT/bad.efi" \
  >/dev/null 2>&1 \
  && { echo "FAIL: mode 2 accepted without --stock-vbmeta"; exit 1; } || true

# pre-flight gate: --stock-vbmeta on mode 1 must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" --stock-vbmeta "$ABL" \
  --out "$OUT/bad.efi" >/dev/null 2>&1 \
  && { echo "FAIL: --stock-vbmeta accepted on mode 1"; exit 1; } || true

echo "PASS: 085 efisp package"
```

- [ ] **Step 3: Verify**

Run: `bash tests/host/085_efisp_package.sh`
Expected: `PASS: 085 efisp package` (or `SKIP:` when no `tests/images/*.img` fixture exists).
If the fv-unwrap step fails on the chosen fixture (a fixture that is not a real LZMA-FV-wrapped ABL), pick a known-good ABL fixture instead — the test only needs one image `fv-unwrap` accepts.

- [ ] **Step 4: Commit**

```bash
git add scripts/efisp-package.py tests/host/085_efisp_package.sh
git commit -m "feat: efisp-package.py — off-device EFISP-payload builder"
```

---

### Task 6: Consolidated `tools/README.md`

**Goal:** One `tools/README.md` documents every host-side tool — purpose, CLI usage, build flavors, platform matrix — and the per-tool `tools/mode2-profile/README.md` is folded in and deleted.

**Files:**
- Create: `tools/README.md`
- Delete: `tools/mode2-profile/README.md`

**Acceptance Criteria:**
- [ ] `tools/README.md` has: an intro; a **Building** section covering host (`make -C tools/<t>`), android (`scripts/build-recovery-tools.sh`), and windows/macos (`scripts/build-cross-tools.sh [windows|macos|all]`); a per-tool section for all six tools; a section for `scripts/efisp-package.py`; and a platform matrix.
- [ ] Each per-tool section has the tool's exact CLI synopsis and at least one worked example.
- [ ] `vbmeta-graft`'s section documents `graft` (the simple host path, no `vbmeta.img`) and `check` (the optional safety step, which needs the device main `vbmeta.img`).
- [ ] `tools/mode2-profile/README.md` is deleted; nothing else links to it.
- [ ] The doc is internally consistent — every command shown is one this slice actually provides.

**Verify:** `test -f tools/README.md && ! test -e tools/mode2-profile/README.md && grep -q build-cross-tools tools/README.md && echo OK`
Expected: `OK`

**Steps:**

- [ ] **Step 1: Read the sources to be accurate**

Read each tool's `.c` for its exact usage string, and read `tools/mode2-profile/README.md` for the content to fold in. The exact synopses to document:
- `fv-unwrap <partition.bin> <output.efi>`
- `abl-patcher --in <abl.bin> [--out <patched.bin>] [--oem <id>] [--no-mode1] [--check-anchors-only]`
- `gbl-pack --out <out> [--cached-abl <efi>] [--source <img>] [--extracted <efi>] [--mode2-profile <bin>]`
- `gbl-commit --src <file> --dst <path> [...]`
- `vbmeta-graft list <img>` · `vbmeta-graft check <candidate-img> <main-vbmeta-img> <part>` · `vbmeta-graft graft --stock <s> --custom <c> --part-size <N> --out <o>`
- `mode2-profile derive <vbmeta.img> -o <profile.toml>` · `mode2-profile compile <profile.toml> -o <profile.bin>`
- `efisp-package.py --abl <img> --mode {0,1,2} --efi <mode-N.efi> [--stock-vbmeta <img>] [--oem <id>] [--tools-dir <dir>] [--out <path>]`

- [ ] **Step 2: Write `tools/README.md`**

Write the file with these exact top-level headings and content:

````markdown
# gbl-chainload host-side tools

These tools build the GBLP1 / EFISP payloads and AVB artifacts that
gbl-chainload consumes. Five are dependency-free C; `fv-unwrap` links
`liblzma`. `mode2-profile` also ships as a pure-Python script.

## Building

All builds run inside the `gbl-chainload-build` docker image
(`docker build -t gbl-chainload-build:latest -f docker/Dockerfile .`).

- **Host (Linux dev binary):** `make -C tools/<tool>` → `tools/<tool>/<tool>`.
- **Android (aarch64, for the recovery ZIP):**
  `scripts/build-recovery-tools.sh` → `dist/recovery/`.
- **Windows / macOS:** `scripts/build-cross-tools.sh windows|macos|all` →
  `dist/windows/<tool>.exe`, `dist/macos/<tool>` (universal x86_64+arm64).

## Tools

### fv-unwrap
[purpose: extracts the ABL PE32+ image out of an LZMA-FV-wrapped partition]
[synopsis + worked example, from Step 1]

### abl-patcher
[purpose: applies the DynamicPatchLib patch scopes to an ABL PE]
[synopsis incl. --oem / --no-mode1 / --check-anchors-only + example]

### gbl-pack
[purpose: builds the GBLP1 overlay container]
[synopsis + example]

### gbl-commit
[purpose: verified write of a payload onto a partition/EFISP]
[synopsis + example]

### vbmeta-graft
[purpose: list / check / graft AVB vbmeta]
[document `graft` as the simple host path — stock partition + custom image,
no `vbmeta.img` needed — and `check` as the optional suitability gate that
*does* need the device main `vbmeta.img`]

### mode2-profile
[purpose: derive a mode-2 profile from stock vbmeta; compile it to the
120-byte binary. Fold in the content of the old mode2-profile/README.md.
Note the C tool and the pure-Python `mode2-profile.py` (Python 3.11+)
produce identical output.]

## scripts/efisp-package.py

[purpose: chains fv-unwrap → abl-patcher → gbl-pack (+ mode2-profile for
mode 2) → concat into one ready-to-flash mode-N.efi payload. A worked
example per mode 0/1/2.]

## Platform matrix

| Tool          | Linux | Android | Windows | macOS |
|---------------|-------|---------|---------|-------|
| fv-unwrap     | ✓     | ✓       | ✓       | ✓     |
| abl-patcher   | ✓     | ✓       | ✓       | ✓     |
| gbl-pack      | ✓     | ✓       | ✓       | ✓     |
| gbl-commit    | ✓     | ✓       | ✓       | ✓     |
| vbmeta-graft  | ✓     | ✓       | ✓       | ✓     |
| mode2-profile | ✓     | ✓       | ✓       | ✓     |
| mode2-profile.py | ✓  | —       | ✓       | ✓     |

`mode2-profile.py` needs only Python 3.11+ and runs unmodified on any host.
````

Replace each `[...]` placeholder with real prose, the exact synopsis from
Step 1, and a worked example — the bracketed lines are authoring
instructions, not literal content. Every command shown must be one this
repo actually provides.

- [ ] **Step 3: Delete the folded-in per-tool README**

```bash
git rm tools/mode2-profile/README.md
```
Then `grep -rn "mode2-profile/README" .` — if anything references it, repoint it at `tools/README.md`.

- [ ] **Step 4: Verify**

Run the **Verify** command above. Read the finished `tools/README.md` once top-to-bottom and confirm no `[...]` authoring placeholder survives.

- [ ] **Step 5: Commit**

```bash
git add tools/README.md
git commit -m "docs: consolidated tools/README.md; fold in mode2-profile README"
```

---

### Task 7: GitHub Actions CI — cross-build + run on real Windows/macOS

**Goal:** A `.github/workflows/host-tools.yml` workflow cross-builds the tools in the docker image and runs each binary on real `windows-latest` and `macos-latest` runners.

**Files:**
- Create: `.github/workflows/host-tools.yml`

**Acceptance Criteria:**
- [ ] The workflow triggers on push/PR touching `tools/**`, `scripts/build-cross-tools.sh`, `docker/**`, or the workflow file, and on `workflow_dispatch`.
- [ ] The `cross-build` job (ubuntu) builds the docker image, runs `build-cross-tools.sh all`, and uploads `dist/windows` + `dist/macos` as artifacts.
- [ ] The `test-windows` (windows-latest) and `test-macos` (macos-latest) jobs download the artifacts and run each of the six binaries, asserting each emits its usage banner (proving the binary executes on the target OS).
- [ ] The workflow YAML is valid (`actionlint` clean, or GitHub accepts it).

**Verify:** `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/host-tools.yml')); print('YAML OK')"` → `YAML OK`. (Full validation is the workflow running on push.)

**Steps:**

- [ ] **Step 1: Write `.github/workflows/host-tools.yml`**

```yaml
name: host-tools

on:
  push:
    paths:
      - 'tools/**'
      - 'scripts/build-cross-tools.sh'
      - 'docker/**'
      - '.github/workflows/host-tools.yml'
  pull_request:
    paths:
      - 'tools/**'
      - 'scripts/build-cross-tools.sh'
      - 'docker/**'
      - '.github/workflows/host-tools.yml'
  workflow_dispatch:

jobs:
  cross-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: false
      - name: Build the docker image
        run: docker build -t gbl-chainload-build:latest -f docker/Dockerfile .
      - name: Cross-build the host-side tools
        run: bash scripts/build-cross-tools.sh all
      - name: Upload Windows binaries
        uses: actions/upload-artifact@v4
        with:
          name: tools-windows
          path: dist/windows
      - name: Upload macOS binaries
        uses: actions/upload-artifact@v4
        with:
          name: tools-macos
          path: dist/macos

  test-windows:
    needs: cross-build
    runs-on: windows-latest
    steps:
      - uses: actions/download-artifact@v4
        with:
          name: tools-windows
          path: dist/windows
      - name: Smoke-test each .exe
        shell: bash
        run: |
          set -u
          fail=0
          for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile; do
            bin="dist/windows/$t.exe"
            out=$("$bin" 2>&1 || true)
            if printf '%s' "$out" | grep -qiE 'usage|--in|derive'; then
              echo "ok  $t"
            else
              echo "FAIL $t — no usage banner; output was: $out"
              fail=1
            fi
          done
          exit $fail

  test-macos:
    needs: cross-build
    runs-on: macos-latest
    steps:
      - uses: actions/download-artifact@v4
        with:
          name: tools-macos
          path: dist/macos
      - name: Smoke-test each universal binary
        shell: bash
        run: |
          set -u
          fail=0
          for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile; do
            bin="dist/macos/$t"
            chmod +x "$bin"
            out=$("$bin" 2>&1 || true)
            if printf '%s' "$out" | grep -qiE 'usage|--in|derive'; then
              echo "ok  $t"
            else
              echo "FAIL $t — no usage banner; output was: $out"
              fail=1
            fi
          done
          exit $fail
```

The smoke test runs each tool with no arguments and greps the output for a
usage-banner token (`usage` / `--in` / `derive`) — most tools print usage and
exit non-zero on a bare call, so the exit code is ignored (`|| true`); what
is asserted is that the binary *executed and produced its banner* on the
target OS. Exact-behaviour coverage stays with the Linux host tests.

- [ ] **Step 2: Verify the YAML parses**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/host-tools.yml')); print('YAML OK')"`
Expected: `YAML OK`. If `actionlint` is available, run it too and resolve any finding.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/host-tools.yml
git commit -m "ci: cross-build host tools + run on real Windows/macOS runners"
```

---

## Final verification

```bash
# host tests unaffected by this slice still pass
bash tests/host/084_cross_build.sh        # PASS or SKIP
bash tests/host/085_efisp_package.sh      # PASS or SKIP
# the docker image rebuilds with the new toolchain
docker build -t gbl-chainload-build:latest -f docker/Dockerfile .
# both flavors assemble
bash scripts/build-cross-tools.sh all
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/host-tools.yml'))"
```

Expected: `084` and `085` `PASS:` (or a clean `SKIP:` where docker / a
fixture is absent); the docker build succeeds; `build-cross-tools.sh all`
populates `dist/windows/` and `dist/macos/`; the workflow YAML parses.

The real cross-platform behaviour check is the GitHub Actions workflow on
push — `test-windows` and `test-macos` run the binaries on real runners.

## Follow-ups (out of scope)

- Vendoring/committing the Windows/macOS binaries; publishing GitHub
  Releases with attached binaries.
- A broader `images/`-drop host orchestration beyond `efisp-package.py`.
- macOS or Windows as a *build host* (the build host stays Linux+docker).

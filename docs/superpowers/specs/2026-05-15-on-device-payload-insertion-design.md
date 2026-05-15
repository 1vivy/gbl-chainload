# On-device payload insertion for gbl-chainload — design

Status: design (pending implementation)
Date: 2026-05-15
Predecessor branch: `feature/objectives-implementation`
Target branch: separate feature branch + PR against `main`
Related research brief: `docs/project/on-device-insertion-research-brief.md`

## TL;DR

This PR adds on-device generation of a cached patched ABL payload, replacing
the build-time `GBL_CACHE_ABL_v1` static-embed currently on
`feature/objectives-implementation`.

- Insertion target: **appended overlay** on the EFISP raw partition. The
  installed artifact is the concatenation of the gbl-chainload PE and a
  `GBLP1` container, written to `/dev/block/by-name/efisp` with `dd`. EFISP
  is not a filesystem on supported targets (`mount -t vfat` rejects it);
  the appended-overlay model treats it as raw bytes.
- Container format: a versioned TLV (`GBLP1` magic) carrying one
  `cached_abl` entry today and reserving space for `mode2_profile` later.
  Header CRC-32 + per-entry SHA-256 + GBLP1END footer.
- Runtime: `GblPayloadLib` finds the overlay either through a configuration
  table installed by our edk2-fork's `fastboot oem boot-efi` handler (test
  path) or by raw BlockIO read of the EFISP partition (production path).
  Single parser, three integrity gates (header, per-entry SHA, PE sanity).
- Boot flow: unified single binary. BootFlow tries cached payload first,
  falls back to the existing dynamic-patch path, falls back to FastbootLib
  (the existing structural fallback in `Entry.c`).
- Delivery: cross-compiled aarch64-Android tools (`fv-unwrap`, `abl-patcher`,
  `gbl-pack`, `gbl-commit`) bundled into a TWRP installer ZIP at
  `dist/gbl-chainload-installer.zip`, which orchestrates extract → patch
  → pack → concat → SHA-verified `dd` → loader-ABL restore.

## Goals

1. Generate a per-OTA cached patched ABL on-device, after the user flashes
   an OTA from custom recovery, without building EDK2 on-device.
2. Preserve the existing agent-safe test path (`fastboot stage` +
   `oem boot-efi`) with byte-for-byte parity to the production install.
3. Keep mode-1 fakelock behavior unchanged. Hooks installed via
   `ProtocolHookLib::InstallAll` between LoadImage and StartImage as today.
4. Land the EFI-side, host-side packer, recovery-side cross-compiled tools,
   and TWRP installer ZIP as a single PR.

## Non-goals

- Mode-2 profile placement and contract (already decided to live at
  `/sdcard/gbl-chainload_profile.xml`, built by `zip/mode2-profile/`,
  separate concern; we reserve a GBLP1 type code 0x0010 for future use only).
- Recovery-graft delivery (already in flight on `feature/objectives-implementation`
  via `zip/recovery-graft/`).
- Authenticode / signed-payload story. SHA-256 + the unlocked-bootloader
  threat model are sufficient for v1.
- Bricking-proof atomic EFISP write. `dd` is single-shot; the
  pre-install `/sdcard/efisp.bak` backup is the recovery medium for an
  interrupted write.

## Background

### What `feature/objectives-implementation` already ships

- `GblChainloadPkg/Library/CachedAblLib/` reads a build-time embedded
  `GBL_CACHE_ABL_v1` container produced by
  `scripts/generate-cached-abl-header.py` from a host-prepared patched ABL.
- `BootFlow.c` consults `CachedAbl_CopyPe` on the cached path; falls back
  to dynamic patch when no cache is built in.
- `tools/abl-patcher` and `tools/fv-unwrap` are existing host C tools.
- `zip/{abl-loader-restore,gbl-chainload,mode2-profile,recovery-graft}/`
  skeletons exist.
- `c49f1a8 patch: allow missing EFISP recursion target` made `patch1`
  allow-on-failure across the board.

### What we replace

- `CachedAblLib`, `CachedAblLayout.h`, `CachedAblLib.h` — replaced by
  `GblPayloadLib` reading the appended overlay at runtime.
- `scripts/generate-cached-abl-header.py` — replaced by `tools/gbl-pack`,
  callable from host (CI) and from recovery (cross-compiled aarch64).
- `tests/053_cache_abl_lint.sh` — replaced by host tests 060–066.
- `--cache-abl <path>` build flag in `scripts/build.sh` and
  `scripts/build-inside-docker.sh` — gone. The EFI no longer accepts a
  build-time payload.

### What survives unchanged

- `tools/abl-patcher.c` and `tools/fv-unwrap.c` source files — gain an
  `android` Makefile target only.
- `Entry.c` and the FastbootLib fallback structure (`BootFlowChainLoad`
  returns → `EnterFastboot` → `FastbootInitialize`).
- `ProtocolHookLib` and the LoadImage-then-hooks-then-StartImage ordering
  from `feature/hook-lifecycle-safety`.
- `LogFsLib` (mounts the separate `logfs` partition; nothing changes).
- The UTF-16 LE `efisp` recursion-pattern semantics in
  `DynamicPatchLib::universal::ApplyEfispRecursion`. Gate moved from
  blanket allow-on-failure to "absence of efisp bytes" semantics; see
  Patch1 policy below.

## Architecture

```
┌─ EFI (single binary, no build-time payload) ─────────────────────┐
│  GblPayloadLib  (NEW): locate overlay bytes, parse GBLP1,        │
│                  per-entry SHA-256 verify, cached_abl PE sanity. │
│                  Returns (Pe, PeSize) or EFI_LOAD_ERROR.         │
│                  Knows nothing about hooks, dynamic patches,     │
│                  or the boot dance.                              │
│  BootFlow.c     (rewritten): unified Tier 1 → Tier 2 → return.   │
│  Entry.c        (UNCHANGED): EnterFastboot fallback is structural.│
│  ProtocolHookLib (UNCHANGED): vital for mode-1 fakelock and      │
│                  future mode-2 hooks.                            │
└──────────────────────────────────────────────────────────────────┘
                              ▲
                              │ raw BlockIO read OR config table from
                              │ FastbootLib boot-efi handler
┌─ Recovery ZIP (zip/gbl-chainload/) ─────────────────────────────┐
│  Premise:  user is in custom recovery, post-OTA. New OTA ABL is │
│            on abl_<inactive_slot>. Active slot still old.       │
│  Pre-flight, then single vol-down 5s abort prompt, then:        │
│   1. read abl_<inactive>                                         │
│   2. fv-unwrap → extracted PE                                    │
│   3. abl-patcher → patched PE                                    │
│   4. gbl-pack → /tmp/payload.bin (GBLP1 container)              │
│   5. concat installed-base-EFI + payload.bin → installed.efi    │
│   6. dd /dev/block/by-name/efisp → /sdcard/efisp.bak (backup)   │
│   7. dd installed.efi → efisp; SHA-verify; restore on mismatch  │
│   8. dd /sdcard/backup_abl.img → abl_<inactive>                 │
└──────────────────────────────────────────────────────────────────┘
                              ▲
                              │ same C code, host build
┌─ Host (CI-gated) ───────────────────────────────────────────────┐
│  All tools dual-target host (gcc/clang) + android (NDK r27+).   │
│  tools/abl-patcher: existing + new `android` target.            │
│  tools/fv-unwrap:   existing + new `android` target.            │
│  tools/gbl-pack:    NEW — packs GBLP1 from inputs.              │
│  tools/gbl-commit:  NEW — POSIX raw-block-device dd + SHA       │
│                     verify + EFISP backup. Same code on host    │
│                     (writes to file) and android (writes to     │
│                     /dev/block/by-name/efisp).                  │
│  GblPayloadLib parser: pure-logic .c compiles under EDK2 + host.│
│  tests/host: roundtrip, lint, parser fuzz, BlockIO smoke.       │
└──────────────────────────────────────────────────────────────────┘
```

## GBLP1 v1 byte layout

Endian: little-endian throughout.

```
┌────────────────────────────────────────────────────────────┐
│ Header (28 bytes, 8-byte aligned)                          │
├────────────────────────────────────────────────────────────┤
│  0 +8  magic         "GBLP1\0\0\0"                          │
│  8 +2  version       0x0001                                │
│ 10 +2  header_size   28                                    │
│ 12 +4  flags         bit0=1 (LE marker); 1..31 reserved=0  │
│ 16 +4  total_size    entire container in bytes             │
│ 20 +4  entry_count   ≥ 1                                   │
│ 24 +4  header_crc32  CRC-32 over bytes [0..24)             │
├────────────────────────────────────────────────────────────┤
│ Entry table (entry_count × 48 bytes)                       │
├────────────────────────────────────────────────────────────┤
│  0 +2  type          0x0001 cached_abl  (REQUIRED, ≥1×)    │
│                      0x0002 source_meta  (opt, ignored)    │
│                      0x0010 mode2_profile  (RESERVED)      │
│                      other  parser MUST ignore             │
│  2 +2  flags         must be 0 in v1                       │
│  4 +4  payload_offset  absolute offset from container start │
│  8 +4  payload_size                                        │
│ 12 +4  reserved      must be 0                             │
│ 16 +32 sha256        SHA-256 of payload_size bytes at off  │
├────────────────────────────────────────────────────────────┤
│ Payload region                                             │
│   Starts at 28 + entry_count*48, padded UP to 16 B.        │
│   Each payload aligned to 16 B; gaps zero.                 │
│   cached_abl payload = patched PE bytes verbatim.          │
├────────────────────────────────────────────────────────────┤
│ Footer (8 bytes)                                           │
│   total_size-8: "GBLP1END"                                 │
└────────────────────────────────────────────────────────────┘

Container size cap: 16 MiB (validated in parser).
```

### Type codes

```
0x0001        cached_abl     REQUIRED, exactly one in v1
0x0002        source_meta    OPTIONAL, parser ignores (gbl-pack always emits)
0x0010        mode2_profile  RESERVED for future PR; v1 parser ignores
0x0003-0x000F reserved for future runtime-recognized types
0x0011-0x00FF reserved for future runtime-recognized types
0x0100-0xFFFF reserved for vendor / test / experimental types
```

### `source_meta` payload schema (binary, runtime ignores)

```
u32  source_abl_size       u8 source_abl_sha256[32]      (raw bytes)
u32  extracted_pe_size     u8 extracted_pe_sha256[32]
u32  patched_pe_size       u8 patched_pe_sha256[32]
u32  packer_version_len    + ASCII bytes
u32  timestamp_iso8601_len + ASCII bytes
```

`ota_fingerprint` is intentionally NOT recorded: at recovery time `getprop
ro.build.fingerprint` returns the OLD OTA fingerprint, which would
mislead. The `source_abl_sha256` already uniquely identifies the ABL
bytes; OTA correlation is `sha256sum` against an OTA archive when needed.

### Runtime validation order

Any failure → log a single grep-able category line, return
`EFI_LOAD_ERROR`, do not LoadImage:

1. `len(bytes) ≥ 28 + 48 + 8` (header + 1 entry + footer)
2. `magic == "GBLP1\0\0\0"`, `version == 1`, `header_size == 28`,
   `flags & 0x1 == 0x1`, `total_size <= 16 MiB`, `total_size <= len(bytes)`
3. `entry_count ≥ 1` and `28 + entry_count*48 + 8 ≤ total_size`
4. `header_crc32` matches `crc32(bytes[0..24))`
5. footer `bytes[total_size-8..total_size) == "GBLP1END"`
6. for each entry:
   - `type ≠ 0`, `flags == 0`, `reserved == 0`
   - `payload_offset` is 16-byte aligned and inside payload region
   - `payload_offset + payload_size ≤ total_size - 8`
   - `sha256(bytes[payload_offset..payload_offset+payload_size))` matches
     entry's recorded `sha256`
7. find entry of type `0x0001` (cached_abl) — exactly one required
8. PE sanity on its bytes:
   - DOS magic `MZ` at offset 0
   - PE\0\0 at `e_lfanew`, with `e_lfanew` sane
   - `Machine == 0xAA64` (AArch64)
   - `Subsystem == 10` (EFI_APPLICATION)
   - `AddressOfEntryPoint` inside `.text` section bounds
9. allocate buffer, copy cached_abl bytes, return `(Pe, PeSize)`

### Failure-category log lines

```
gbl-payload: source=<staged-buffer|efisp-blockio> base=0x... size=...
gbl-payload: cannot locate overlay bytes (no config table, BlockIO read failed)
gbl-payload: bad magic
gbl-payload: unsupported version <N>
gbl-payload: total_size mismatch (header=N file=M)
gbl-payload: header_crc32 mismatch
gbl-payload: footer mismatch
gbl-payload: entry <i> sha256 mismatch
gbl-payload: no cached_abl entry
gbl-payload: cached_abl PE insane (<which check>)
```

### Packer-side gate (host-only, runtime ignores)

`gbl-pack` refuses to emit a container whose `cached_abl` payload contains
the UTF-16 LE bytes `e\x00f\x00i\x00s\x00p\x00`. Defense-in-depth after
patch1's site-table-driven zeroing — surfaces a "patch1 site list missing
this ABL variant" error at packer time rather than at boot.

The same byte-scan helper is shared with EDK2's `DynamicPatchLib` via
`tools/shared/efisp_scan.h` — see Patch1 policy.

## EDK2 integration

### `GblPayloadLib` (NEW)

```
GblChainloadPkg/
├── Include/Library/
│   └── GblPayloadLib.h
└── Library/GblPayloadLib/
    ├── GblPayloadLib.inf
    ├── PayloadParse.c        ← pure logic; compiles under EDK2 + host
    ├── PayloadParse.h        ← internal
    ├── Sha256.c              ← reuse OpenSslLib in EDK2; libcrypto host
    ├── Crc32.c               ← pure logic
    ├── PeSanity.c            ← AArch64/EFI_APPLICATION/.text bound
    ├── LocateOverlay.c       ← config-table check + BlockIO raw read
    └── EfispBlockIo.c        ← EDK2-only: GetBlkIOHandles(L"efisp"),
                                ReadDisk for first ~16 MiB
```

Public API:

```c
// Parse the appended overlay (test path: configuration table installed by
// FastbootLib's boot-efi handler; production path: raw read of EFISP via
// BlockIO). Validates GBLP1 header, per-entry SHA-256, finds the cached_abl
// entry, sanity-checks the PE, returns the bytes ready for LoadImage.
//
// On any failure returns an EFI_ERROR status with a single GBL_INFO log
// line in the failure-category vocabulary. Allocates the returned buffer
// with AllocatePool; caller frees on failure paths only — successful
// LoadImage takes ownership per UEFI semantics.
EFI_STATUS
EFIAPI
GblPayload_LoadCachedAbl (
  IN  EFI_HANDLE  ImageHandle,
  OUT VOID      **Pe,
  OUT UINT32     *PeSize
  );

// Log structured provenance from source_meta entry, if present. Same shape
// as the existing CachedAbl_LogMetadata. Called once at boot. Never fails.
VOID
EFIAPI
GblPayload_LogProvenance (
  IN EFI_HANDLE  ImageHandle
  );
```

### Overlay-source resolution

```c
STATIC EFI_STATUS
LocateOverlayBytes (OUT VOID **Bytes, OUT UINTN *Size)
{
  // 1. Test path: configuration table set by boot-efi handler.
  for (UINTN I = 0; I < gST->NumberOfTableEntries; I++) {
    if (CompareGuid (&gST->ConfigurationTable[I].VendorGuid,
                     &gGblStagedBufferGuid)) {
      GBL_STAGED_BUFFER_TABLE *T = gST->ConfigurationTable[I].VendorTable;
      if (T->Magic == SIGNATURE_32 ('G','B','L','S') && T->Version == 1) {
        *Bytes = (VOID *)(UINTN)T->Base;
        *Size  = T->Size;
        GBL_INFO ("gbl-payload: source=staged-buffer base=0x%lx size=%u\n",
                  T->Base, *Size);
        return EFI_SUCCESS;
      }
    }
  }

  // 2. Production path: raw read of EFISP partition.
  return ReadEfispRawBytes (Bytes, Size);
}
```

`ReadEfispRawBytes` reuses the same pattern as `LogFsLib::Mount.c`:
`GetBlkIOHandles(L"efisp")` to get the partition handle, `HandleProtocol`
for `EFI_BLOCK_IO_PROTOCOL`, `ReadBlocks` for the first ~16 MiB into an
`AllocatePool` buffer (or two-pass: 64 KiB header read → parse PE optional
header to find PE end → read PE_end + GBLP1 total_size bytes in a single
follow-up `ReadBlocks`).

### `BootFlow.c` (rewritten — unified, no build-flag fork)

```c
EFI_STATUS
EFIAPI
BootFlowChainLoad (VOID)
{
  EFI_STATUS  Status;
  VOID       *Pe = NULL;
  UINT32      PeSize = 0;
  EFI_HANDLE  AblImage = NULL;
  CHAR8      *Origin = "<none>";

  GblPayload_LogProvenance (gImageHandle);

  // Tier 1: cached ABL via appended overlay (config table or EFISP raw).
  Status = GblPayload_LoadCachedAbl (gImageHandle, &Pe, &PeSize);
  if (!EFI_ERROR (Status)) {
    Origin = "cached";
  } else {
    GBL_INFO ("BootFlow: cached unavailable (%r), trying dynamic patch\n",
              Status);

    // Tier 2: extract+patch live abl_<slot> (current behavior).
    Status = DynamicPatch_RunOnSlotAbl (&Pe, &PeSize);
    if (EFI_ERROR (Status)) {
      GBL_INFO ("BootFlow: dynamic patch failed (%r), returning\n", Status);
      return Status;     // Tier 3: Entry.c → EnterFastboot
    }
    Origin = "dynamic";
  }

  GBL_INFO ("BootFlow: loaded ABL via %a (size=%u)\n", Origin, PeSize);

  // Hook-lifecycle order preserved per feature/hook-lifecycle-safety:
  //   LoadImage → ProtocolHookLib::InstallAll → StartImage.
  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Pe, PeSize, &AblImage);
  FreePool (Pe);
  if (EFI_ERROR (Status)) {
    GBL_INFO ("BootFlow: LoadImage failed: %r\n", Status);
    return Status;
  }

  ProtocolHookLib_InstallAll ();
  LogFsClose ();

  Status = gBS->StartImage (AblImage, NULL, NULL);
  GBL_INFO ("BootFlow: StartImage returned %r — falling through\n", Status);
  return Status;
}
```

### `FastbootCmds.c` change in our edk2 fork

In `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`, the existing
`oem boot-efi` handler gains a small pre-LoadImage configuration-table
install:

```c
// New header in same file:
typedef struct {
  UINT32                Magic;     // SIGNATURE_32('G','B','L','S')
  UINT32                Version;   // 1
  EFI_PHYSICAL_ADDRESS  Base;      // staged buffer physical address
  UINTN                 Size;      // staged buffer size in bytes
} GBL_STAGED_BUFFER_TABLE;

EFI_GUID gGblStagedBufferGuid = {
  0x... /* a fresh GUID — to be generated at implementation time */
};

STATIC GBL_STAGED_BUFFER_TABLE gGblStagedBuffer;

// Inside the existing `oem boot-efi` handler, immediately before the
// gBS->LoadImage call:
gGblStagedBuffer.Magic   = SIGNATURE_32 ('G','B','L','S');
gGblStagedBuffer.Version = 1;
gGblStagedBuffer.Base    = (EFI_PHYSICAL_ADDRESS)(UINTN)stage_buffer;
gGblStagedBuffer.Size    = stage_buffer_size;
gBS->InstallConfigurationTable (&gGblStagedBufferGuid, &gGblStagedBuffer);
```

~15 lines. Backwards-compatible: EFIs that don't look for the table
ignore it. Any future overlay-aware EFI gets the staged buffer pointer
for free.

### Patch1 policy

Refines `c49f1a8 patch: allow missing EFISP recursion target` from
"always allow" to **"absence of UTF-16 LE `efisp` bytes is the
invariant"**:

- **Cache path (Tier 1):** `gbl-pack` runs `tools/shared/efisp_scan.h` at
  packer time on the patched PE bytes. If any `efisp` bytes remain after
  patches, `gbl-pack` refuses to emit. Cached_abl bytes that reach the
  EFI are by construction efisp-clean. Runtime does not recheck.
- **Dynamic path (Tier 2):** `DynamicPatch_RunOnSlotAbl` runs the same
  byte scan on its output PE before returning. If any `efisp` bytes
  remain, returns `EFI_LOAD_ERROR`, drops to Tier 3 (FastbootLib).
- **Tier 3 (no patches applied):** patches missing means either the live
  ABL has no efisp loader vector (safe — no recursion possible) or
  patch1's site list is stale (unsafe). Tier 3's destination is
  FastbootLib, where the user reaches recovery.

Single helper, used by both DynamicPatchLib (runtime, dynamic path) and
`tools/gbl-pack` (host, cache path), via `tools/shared/efisp_scan.h`.

## Recovery toolchain

```
tools/
├── abl-patcher/
│   ├── abl-patcher.c        (existing — single source, host + android)
│   └── Makefile             (existing host target + new `android` target)
├── fv-unwrap/
│   ├── fv-unwrap.c          (existing — single source, host + android)
│   └── Makefile             (existing host target + new `android` target)
├── gbl-pack/                ← NEW
│   ├── gbl-pack.c           (host + android)
│   ├── pack.c               (pure-logic; shared with EDK2 parser tests)
│   └── Makefile
├── gbl-commit/              ← NEW
│   ├── gbl-commit.c         (POSIX, host + android)
│   └── Makefile
└── shared/                  ← NEW (header-only)
    ├── gblp1.h              (struct GBLP1 header/entry; shared with EDK2)
    ├── patch_signatures.h   (re-exported by GblChainloadPkg/Library/
                              DynamicPatchLib/{mode_1,oem,universal}/
                              Signatures.h via a stdint shim — single
                              authoritative source)
    └── efisp_scan.h         (UTF-16 efisp byte-scan helper; shared
                              between DynamicPatchLib and gbl-pack)
```

### `tools/gbl-pack`

```
gbl-pack \
  --cached-abl <patched.efi> \
  --source     <raw_abl_partition.img> \
  --extracted  <unwrapped.efi> \
  --out        <gbl-payload.bin>
```

Validates: cached-abl bytes do NOT contain UTF-16 LE `efisp` (via
`tools/shared/efisp_scan.h`); cached-abl PE sane (Machine=AArch64,
Subsystem=10); container ≤ 16 MiB; SHAs computed and stored in
`source_meta` entry. Refuses to emit on any failure.

Optionally also accepts `--base-efi <gbl-chainload.efi>` and writes the
concatenated installable blob (`base_efi || gbl-payload.bin`) to an output
path of the caller's choosing — used by the recovery ZIP to skip a
separate concat step.

### `tools/gbl-commit`

```
gbl-commit \
  --src <installed.efi> \
  --dst <block-device-or-file> \
  --backup <backup-path>      [optional; if set, dd dst → backup before write]
  --verify                    [optional; re-read dst, sha256 vs src, restore on mismatch]
```

POSIX file/block ops:

```c
if (backup) {
    dd dst → backup, fsync;
}
fd_dst = open(dst, O_WRONLY|O_LARGEFILE);
write_all(fd_dst, src_bytes); fsync(fd_dst); close(fd_dst);
if (verify) {
    fd_dst = open(dst, O_RDONLY|O_LARGEFILE);
    sha256(read_all(fd_dst, src_size)) == sha256(src_bytes)?
    if mismatch && backup:
        dd backup → dst, fsync;
        return EXIT_VERIFY_FAILED_RESTORED;
}
sync();
```

Same code on host (writes any path) and Android (writes
`/dev/block/by-name/efisp` after TWRP startup mounts the device tree).

### Patch-signature sharing

`tools/shared/patch_signatures.h` is the single authoritative source.
`GblChainloadPkg/Library/DynamicPatchLib/{mode_1,oem,universal}/Signatures.h`
become thin `#include "../../tools/shared/patch_signatures.h"` files
(with a 10-LOC stdint shim so the same header compiles under both EDK2
and tools build environments). One place to update when a new ABL
signature is added. Test 065 enforces parity.

### NDK in docker

```
docker/Dockerfile (extended):
  + RUN curl -sL <NDK r27 url> | unzip -d /opt/android-ndk-r27 ...
  + ENV ANDROID_NDK=/opt/android-ndk-r27

tools/*/Makefile (each):
  android: CC      = $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
  android: CFLAGS += -static -O2 -Wall -Wextra
  android: $(BIN)-android
```

NDK r27 (Android 16 / API 35 sysroot) gives aarch64 bionic-static
binaries that work in TWRP without any libc.so dependency.

### `scripts/build-recovery-tools.sh`

```sh
#!/usr/bin/env bash
set -euo pipefail
docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit; do
    make -C tools/$t android
    install -Dm755 tools/$t/$t-android dist/recovery/$t
  done
  sha256sum dist/recovery/* > dist/recovery/SHA256SUMS
'
```

## Recovery ZIP — `zip/gbl-chainload/`

```
zip/gbl-chainload/
├── META-INF/com/google/android/
│   ├── update-binary         ← orchestration shell script (below)
│   └── updater-script        ← stub: "# scripted via update-binary"
├── bin/
│   ├── fv-unwrap             ← from dist/recovery/ (cross-compiled)
│   ├── abl-patcher
│   ├── gbl-pack
│   └── gbl-commit
├── base/
│   └── gbl-chainload.efi     ← stripped base EFI (no payload appended)
├── README.txt                ← user-facing prerequisites + recovery notes
└── SHA256SUMS

dist/gbl-chainload-installer.zip   ← built by scripts/build-recovery-zip.sh
```

### `update-binary`

```sh
#!/sbin/sh
OUTFD=$2; ZIP=$3
ui_print() { echo -e "ui_print $1\nui_print" >&"$OUTFD"; }
abort()    { ui_print "ABORT: $1"; exit 1; }

ui_print "gbl-chainload installer"
ui_print "======================="

mkdir -p /tmp/gbl
unzip -o "$ZIP" -d /tmp/gbl >/dev/null
chmod 755 /tmp/gbl/bin/*

# --- pre-flight (fail loudly, no rollback work needed) -----------------
SLOT_SUFFIX=$(getprop ro.boot.slot_suffix)
ACTIVE=${SLOT_SUFFIX#_}
case "$ACTIVE" in a) INACTIVE=b ;; b) INACTIVE=a ;;
                  *) abort "no slot suffix; not an A/B device?" ;; esac

ABL_INACTIVE=/dev/block/by-name/abl_$INACTIVE
[ -r "$ABL_INACTIVE" ]               || abort "cannot read $ABL_INACTIVE"
[ -f /sdcard/backup_abl.img ]        || abort "/sdcard/backup_abl.img missing"
[ -b /dev/block/by-name/efisp ]      || abort "/dev/block/by-name/efisp missing"

# Sanity: confirm EFISP currently has a PE we can recognize.
HEAD2=$(dd if=/dev/block/by-name/efisp bs=1 count=2 2>/dev/null | xxd -p)
[ "$HEAD2" = "4d5a" ] || abort "EFISP does not currently look like a PE (first 2B != MZ)"

# --- single abort prompt at the top -----------------------------------
ui_print "About to:"
ui_print "  1. read $ABL_INACTIVE  (new OTA ABL)"
ui_print "  2. fv-unwrap + patch + pack -> GBLP1 overlay"
ui_print "  3. concat with base EFI -> installed.efi"
ui_print "  4. backup current EFISP -> /sdcard/efisp.bak"
ui_print "  5. dd installed.efi -> /dev/block/by-name/efisp"
ui_print "  6. SHA-verify EFISP; restore from backup on mismatch"
ui_print "  7. dd /sdcard/backup_abl.img -> $ABL_INACTIVE"
ui_print ""
ui_print "Vol-DOWN within 5s to ABORT. Any other key (or timeout) continues."
KEY=$(timeout 5 getevent -lqc 5 2>/dev/null \
        | grep -m1 -oE 'KEY_(VOLUMEUP|VOLUMEDOWN)' || true)
[ "$KEY" = "KEY_VOLUMEDOWN" ] && abort "user aborted (vol-down)"

# --- non-interactive remainder ----------------------------------------
ui_print "[1/7] reading $ABL_INACTIVE"
dd if=$ABL_INACTIVE of=/tmp/gbl/abl_inactive.img bs=1M 2>/dev/null

ui_print "[2/7] fv-unwrap + abl-patcher + gbl-pack"
/tmp/gbl/bin/fv-unwrap /tmp/gbl/abl_inactive.img /tmp/gbl/extracted.efi \
  || abort "fv-unwrap failed (new ABL format?)"
/tmp/gbl/bin/abl-patcher /tmp/gbl/extracted.efi /tmp/gbl/patched.efi \
  || abort "abl-patcher failed (no matching signatures?)"
/tmp/gbl/bin/gbl-pack \
  --cached-abl /tmp/gbl/patched.efi \
  --source     /tmp/gbl/abl_inactive.img \
  --extracted  /tmp/gbl/extracted.efi \
  --out        /tmp/gbl/payload.bin \
  || abort "gbl-pack failed (efisp-scan or sanity gate?)"

ui_print "[3/7] concat base EFI + payload -> installed.efi"
cat /tmp/gbl/base/gbl-chainload.efi /tmp/gbl/payload.bin \
  > /tmp/gbl/installed.efi

ui_print "[4/7] backup current EFISP -> /sdcard/efisp.bak"
EFISP_SIZE=$(blockdev --getsize64 /dev/block/by-name/efisp)
dd if=/dev/block/by-name/efisp of=/sdcard/efisp.bak bs=1M \
  count=$((EFISP_SIZE / 1048576)) 2>/dev/null
sync

ui_print "[5/7] dd installed.efi -> EFISP"
/tmp/gbl/bin/gbl-commit \
  --src /tmp/gbl/installed.efi \
  --dst /dev/block/by-name/efisp \
  --backup /sdcard/efisp.bak \
  --verify \
  || abort "EFISP write/verify failed (backup restored)"

ui_print "[6/7] verify done by gbl-commit --verify"

ui_print "[7/7] restore loader ABL: /sdcard/backup_abl.img -> $ABL_INACTIVE"
dd if=/sdcard/backup_abl.img of=$ABL_INACTIVE bs=1M conv=fsync 2>/dev/null \
  || abort "abl restore failed (gbl-chainload may not load on next boot)"
sync

ui_print ""
ui_print "DONE. Reboot to use cached gbl-chainload."
ui_print "On boot failure: hold Vol-Up at boot to reach FastbootLib."
exit 0
```

### `README.txt` (bundled)

```
gbl-chainload-installer.zip
===========================

Prerequisites:
  - /sdcard/backup_abl.img must exist (a previously-saved working ABL
    that loads gbl-chainload from EFISP).
  - You are running custom recovery (TWRP).
  - You have either:
      a) just flashed an OTA from custom recovery, OR
      b) rebooted to recovery before letting system-OTA's mid-boot
         finalization run.

Install:
  In TWRP: Install -> gbl-chainload-installer.zip -> swipe.
  At the abort prompt, vol-DOWN within 5s to abort, anything else continues.

This ZIP writes:
  - /dev/block/by-name/efisp          (gbl-chainload + cached ABL overlay)
  - /sdcard/efisp.bak                 (pre-write backup of EFISP)
  - /dev/block/by-name/abl_<inactive> (loader ABL restored from
                                       /sdcard/backup_abl.img)

The EFISP and ABL writes are user-driven (you, in TWRP, swiping to
install this ZIP). The agent-side fastboot-flash hard-deny in this
project's CLAUDE.md does not gate this user action.

Recovery:
  - If EFISP write fails verification, gbl-commit automatically restores
    /sdcard/efisp.bak to EFISP and aborts. Same state you started in.
  - If a future boot fails, hold Vol-Up at the gbl-chainload window to
    reach FastbootLib (current in-use chainload binary is whatever was
    last installed; it always provides this fallback).
  - If gbl-chainload itself fails to load (corrupted EFISP), hold Vol-Up
    at ABL boot to reach the OEM's native fastboot menu, then either:
      a) `fastboot reboot recovery` and re-run this ZIP, or
      b) `dd /sdcard/efisp.bak → /dev/block/by-name/efisp` from TWRP shell.
```

### `scripts/build-recovery-zip.sh`

```sh
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

[ -d dist/recovery ] || scripts/build-recovery-tools.sh

cp dist/recovery/{fv-unwrap,abl-patcher,gbl-pack,gbl-commit} \
   zip/gbl-chainload/bin/
cp dist/gbl-chainload.efi zip/gbl-chainload/base/

sha256sum zip/gbl-chainload/bin/* zip/gbl-chainload/base/* \
  > zip/gbl-chainload/SHA256SUMS

(cd zip/gbl-chainload && zip -qr "$OLDPWD/dist/gbl-chainload-installer.zip" .)
echo "==> dist/gbl-chainload-installer.zip"
ls -l dist/gbl-chainload-installer.zip
```

## Failure / rollback model

| Scenario | Detection | Action |
|---|---|---|
| Power loss mid `gbl-pack` | `/tmp/gbl/payload.bin` partial or absent | re-run ZIP |
| Power loss mid EFISP backup (step 4) | `/sdcard/efisp.bak` size != EFISP size | re-run ZIP; pre-flight detects partial backup and aborts before step 5 |
| Power loss mid EFISP write (step 5) | `gbl-commit --verify` SHA mismatch | gbl-commit auto-restores `/sdcard/efisp.bak` → exit non-zero → ZIP aborts |
| Power loss mid backup-ABL restore (step 7) | abl_<inactive> partial-write | bootloader's slot ABL is corrupt → either auto-fallback to other slot (if A/B-bootloader is forgiving) or boot fails → hold Vol-Up at ABL → OEM fastboot → re-run ZIP from recovery |
| Bad container hash at runtime | `verify_sha256()` per entry in GblPayloadLib | log `gbl-payload: entry <i> sha256 mismatch` → return EFI_LOAD_ERROR → BootFlow Tier 2 → DynamicPatch on live abl_<slot> → if also fails, EnterFastboot |
| GBLP1 magic absent (post-PE bytes corrupt or absent) | scan finds no magic | log `gbl-payload: bad magic` (or "cannot locate overlay bytes") → Tier 2 fallback as above |
| EFISP not a PE before install | pre-flight `head -c2 != "MZ"` | abort before any write |
| Backup ABL missing at install | `[ -f /sdcard/backup_abl.img ]` pre-flight fails | abort before any read |
| New ABL with no patch1 site match | `DynamicPatch_RunOnSlotAbl` post-patch efisp-scan fails | Tier 2 returns EFI_LOAD_ERROR → Tier 3 EnterFastboot → user reaches recovery, can re-run ZIP with updated patcher or restore from `/sdcard/efisp.bak` |

## Test architecture

Three layers, each gated on the layer below.

### Layer 1 — Host (CI-gated, agent-runnable)

Runs in <60 s on every PR.

```
tests/host/
├── 060_pack_roundtrip.sh           pack→parse, byte-for-byte and SHA agreement
├── 061_parser_fuzz.sh              corruption at known positions returns right error
├── 062_efisp_scan_gate.sh          gbl-pack refuses poisoned PE; same helper
                                    works in EDK2 host harness
├── 063_pe_sanity.sh                Machine/Subsystem/entry-point gates
├── 064_e2e_fixtures.sh             fv-unwrap → abl-patcher → gbl-pack → parser
                                    end-to-end on tests/images/pe/* fixtures
├── 065_patch_sig_parity.sh         tools/shared/patch_signatures.h ==
                                    DynamicPatchLib/*/Signatures.h
├── 066_dd_commit_atomic.sh         loop-back raw block file, gbl-commit, SIGKILL
                                    mid-write, verify pre-existing file unchanged
                                    (or new file fully written + verified)
├── 067_blockio_reader_smoke.sh     synthetic raw block image (PE + GBLP1
                                    appended), exercise GblPayloadLib parser
                                    via host harness against the synthetic bytes
├── 068_config_table_override.sh    host harness simulating LocateOverlayBytes:
                                    config table set → reads from buffer; absent
                                    → falls to BlockIO source
└── helpers/
    ├── parser_harness              C binary linking GblPayloadLib parse.c
                                    directly (same bytes the EFI sees)
    └── poison-pe                   deterministic PE corruption tool
```

`parser_harness` is the load-bearing piece: `GblPayloadLib/PayloadParse.c`
compiled host-side via the stdint shim, exposing a `gblp1_parse(bytes,
size, out_pe_offset, out_pe_size)` entry point. Tests 060/061/063/064/067
all drive it. Identical bytes in CI as on device — the only thing host
doesn't exercise is the EDK2-only BlockIO IO wrapper (~30 LOC).

### Layer 2 — Agent on-device (SAFE — `fastboot stage` only)

```sh
# Per EFI/BootFlow change. Agent-runnable, no persistent state.
cat dist/gbl-chainload.efi tests/host/fixtures/golden-payload.bin \
  > /tmp/test-installed.efi
fastboot stage /tmp/test-installed.efi
fastboot oem boot-efi

# Expected UefiLog.txt:
gbl-payload: source=staged-buffer base=0x... size=...
BootFlow: loaded ABL via cached (size=NNNN)
# ... boot continues with cached ABL
```

This validates the full Tier 1 path **including** the GBLP1 parser, SHA
checks, and PE sanity, against the byte-for-byte identical concat'd EFI
that would be `dd`-ed to EFISP in production. The only difference vs
production is the source: configuration-table-from-FastbootLib instead of
BlockIO-from-EFISP. Both eventually call the same parser.

### Layer 3 — User on-device (incremental, persistent writes — USER-RUN)

| # | Step | Persistent write? | What it proves | When |
|---|---|---|---|---|
| 1 | TWRP shell: `dd /tmp/test-installed.efi → /dev/block/by-name/efisp; sha256 verify; reboot; observe `source=efisp-blockio` log | EFISP only | Production BlockIO read path on real device | Once per supported device, before promoting cache-abl install ZIP to users |
| 2 | Boot Android, confirm KM `SET_ROT` and normal-boot AVB green | None | Cached + hooks installed correctly; mode-1 fakelock survives the cache path | Same as #1 |
| 3 | Run `gbl-chainload-installer.zip` from TWRP after a real OTA | EFISP + abl_inactive | Full-install ZIP works end-to-end including loader-ABL restore | Per OTA, per device |
| 4 | Multiple OTA cycles | EFISP refresh per cycle | Cache flow is operationally durable | Long-term |

Step 1 is the **only** thing Layer 2 cannot validate (the BlockIO source
swap). Layer 2 validates everything else with full byte parity.

### Test fixtures

- Existing: `tests/images/pe/{infiniti-EU-16.0.5.703,infiniti-IN-16.0.7.201,fairlady-CN-16.0.7.200}.efi`
- New: `tests/host/fixtures/poisoned-pe.efi` — known-good PE with
  `e\x00f\x00i\x00s\x00p\x00\x00\x00` injected at three offsets, used by 062
- New: `tests/host/fixtures/golden-payload.bin` — deterministic GBLP1 from
  `infiniti-EU-16.0.5.703.efi`, regenerated on demand via
  `make -C tests/host fixtures`
- New: `tests/host/fixtures/synthetic-efisp.img` — small file containing a
  known PE + a known GBLP1 overlay, for 067

## File plan

### Added files

```
GblChainloadPkg/Include/Library/GblPayloadLib.h
GblChainloadPkg/Library/GblPayloadLib/
    GblPayloadLib.inf
    PayloadParse.c          (pure logic; host + EDK2)
    PayloadParse.h
    Sha256.c                (pure logic + libcrypto host shim)
    Crc32.c                 (pure logic)
    PeSanity.c              (pure logic)
    LocateOverlay.c         (config-table check, then EFISP BlockIO)
    EfispBlockIo.c          (EDK2-only IO wrapper)

tools/gbl-pack/
    gbl-pack.c
    pack.c                  (pure-logic packer; shared with EDK2 parser tests)
    Makefile                (host + android targets)
tools/gbl-commit/
    gbl-commit.c
    Makefile
tools/shared/
    gblp1.h                 (header/entry struct, shared with EDK2)
    patch_signatures.h      (single source for patch sites)
    efisp_scan.h            (UTF-16 efisp byte-scan helper)

zip/gbl-chainload/META-INF/com/google/android/{update-binary,updater-script}
zip/gbl-chainload/README.txt

scripts/build-recovery-tools.sh
scripts/build-recovery-zip.sh

tests/host/060_pack_roundtrip.sh
tests/host/061_parser_fuzz.sh
tests/host/062_efisp_scan_gate.sh
tests/host/063_pe_sanity.sh
tests/host/064_e2e_fixtures.sh
tests/host/065_patch_sig_parity.sh
tests/host/066_dd_commit_atomic.sh
tests/host/067_blockio_reader_smoke.sh
tests/host/068_config_table_override.sh
tests/host/helpers/{parser_harness.c, poison-pe.c}
tests/host/Makefile
tests/host/fixtures/{poisoned-pe.efi, golden-payload.bin, synthetic-efisp.img}

docs/project/payload-container-v1.md  (format reference, type table, examples)
```

### Modified files

```
GblChainloadPkg/Application/GblChainload/BootFlow.c
    rewrite to unified Tier 1 → Tier 2 → return shape (above)

GblChainloadPkg/Application/GblChainload/GblChainload.inf
    drop CachedAblLib link, add GblPayloadLib link

GblChainloadPkg/GblChainloadPkg.dsc
    register GblPayloadLib, drop CachedAblLib

GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchEngine.c
    add post-patch efisp byte-scan gate; return EFI_LOAD_ERROR if any
    efisp bytes remain

GblChainloadPkg/Library/DynamicPatchLib/{mode_1,oem,universal}/Signatures.h
    convert to thin #include of tools/shared/patch_signatures.h

edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c  (our edk2 fork)
    boot-efi handler: install gGblStagedBufferGuid configuration table
    pointing at the staged buffer before LoadImage. ~15 LOC.

scripts/build.sh
    drop --cache-abl flag (no longer applicable)

scripts/build-inside-docker.sh
    drop GBL_HAS_CACHED_ABL handling (no build-time payload)

docker/Dockerfile
    add NDK r27 install + ANDROID_NDK env var

tools/abl-patcher/Makefile
    add `android` cross-compile target

tools/fv-unwrap/Makefile
    add `android` cross-compile target

tests/runall.sh
    register tests/host/06{0..8}

docs/project/current-state.md
    update cache-ABL line to reflect on-device generation

docs/project/decisions.md
    update OTA / cache-ABL delivery model section to reflect appended
    overlay + on-device generation

docs/project/next-milestone.md
    update objective 2 acceptance criteria
```

### Deleted files

```
GblChainloadPkg/Include/Library/CachedAblLayout.h
GblChainloadPkg/Include/Library/CachedAblLib.h
GblChainloadPkg/Library/CachedAblLib/CachedAblLib.c
GblChainloadPkg/Library/CachedAblLib/CachedAblLib.inf
scripts/generate-cached-abl-header.py
tests/053_cache_abl_lint.sh
```

## Sub-stages (single PR)

1. **Format + EDK2 parser + tests** (no boot-flow integration yet).
   Land `tools/shared/gblp1.h`, `GblPayloadLib`, host packer, host CI tests
   060–068. Bench: `parser_harness` reads `golden-payload.bin` correctly;
   all host tests green.
2. **BootFlow unification + CachedAblLib teardown.** Rewrite BootFlow.c
   to the unified Tier-1/2/3 shape. Delete `CachedAblLib`, headers, generator
   script, lint test. Bench: `mode-1.efi` build passes, agent-stage test
   shows "loaded ABL via dynamic" as today (no overlay yet → Tier 1 misses,
   Tier 2 takes over).
3. **FastbootCmds.c configuration-table install + cross-compile toolchain.**
   Add NDK to docker, `android` Makefile targets, `tools/gbl-commit`,
   `scripts/build-recovery-tools.sh`. Add the FastbootCmds.c boot-efi
   change. Bench: agent-stage test of a concat'd `gbl-chainload.efi +
   golden-payload.bin` shows "source=staged-buffer" and "loaded ABL via
   cached" in UefiLog.
4. **Recovery ZIP.** Land `zip/gbl-chainload/`, `scripts/build-recovery-zip.sh`,
   bundle base EFI + cross-compiled tools. Bench: ZIP unzip-and-`update-binary`
   on a connected device produces an EFISP write whose SHA matches the
   ZIP's bundled EFI + freshly-packed payload.

## Migration from `feature/objectives-implementation`

Branch `feature/on-device-payload-insertion` from `feature/objectives-implementation`
HEAD. Sub-stage commits land in order on that branch. PR against `main`
includes the diff to `main` (which incorporates whatever has merged from
`feature/objectives-implementation` by then).

If `feature/objectives-implementation` is still open when this PR is ready,
rebase onto its tip and resolve trivial overlap on `BootFlow.c` (the
CachedAblLib integration is what we're tearing out anyway).

## Open questions (deferred to follow-up PRs)

- **mode-2 profile binary schema.** Type code 0x0010 reserved in this PR's
  GBLP1 v1; concrete schema (which fields, which optional, failure
  semantics) lands when mode-2 implementation begins. The
  `/sdcard/gbl-chainload_profile.xml` parking convention from
  `decisions.md` remains authoritative until then.
- **OVMF-based EDK2 reader test.** Layer 3 step 1 is the gate today.
  Adding an OVMF harness that loads a synthetic EFISP image and exercises
  `EfispBlockIo.c` end-to-end is desirable but out of scope. Add when
  production bugs in the BlockIO reader path emerge.
- **EFISP capacity per device.** No proactive free-space gate on EFISP;
  `dd` errors on full-partition write surface naturally. If real-world
  capacity issues emerge, add a diff/patch tool against the previous
  installed EFI rather than re-writing the whole partition.

## Caveats

- The "EFISP is not vfat-mountable from Linux" finding (verified by
  `mount -t vfat /dev/block/by-name/efisp -> Invalid argument` on infiniti
  on 2026-05-15) drove the appended-overlay design. If a future device
  has FAT-formatted EFISP, the design still works — `dd` to a raw block
  device is format-agnostic.
- Raw `dd` to EFISP is single-shot, not atomic. Mitigated by the
  pre-write `/sdcard/efisp.bak` backup and `gbl-commit --verify` SHA
  check + automatic backup-restore on mismatch.
- `BootESP` in our edk2 fork is generic ESP-fallback infrastructure for
  USB/external storage and is NOT how ABL loads gbl-chainload. Don't
  conflate the two.
- The configuration-table mechanism in `boot-efi` is backwards-compatible
  but does add a small new contract between FastbootLib and any
  overlay-aware EFI. Document the GUID in
  `docs/project/payload-container-v1.md` so a future EFI can opt in.

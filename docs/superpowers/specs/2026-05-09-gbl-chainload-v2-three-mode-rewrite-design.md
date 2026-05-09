# gbl-chainload v2 — three-mode taxonomy, scan-based patch engine, ABL embed

**Date**: 2026-05-09
**Status**: Design approved by 1vivy; ready for implementation plan.
**Working directory at design time**: `/home/vivy/gbl-chainload` (becomes `dirty/` reference once new repo is created).

## Context

`mode-fakelocked` (current branch HEAD `cc8a48e`) proved the protocol-hook fakelock works on canoe with stock images: `vb-fakelock | READ_CONFIG | is_unlocked 1->0` mutates the VerifiedBoot view, ABL builds KM `0x208 SET_BOOT_STATE` with `isUnlocked=0, color=0` and the real OEM pubkey digest, and Oplus cmdline ends `oplusboot.verifiedbootstate=green`. AVB still reports orange on custom recovery (`OK_NOT_SIGNED`), so we shipped `patch9-avb-locked-recoverable-continue` to split libavb's `allow_verification_error` from the AVB-result-to-boot-state decision.

Two real problems forced the rebuild:

1. **`patch9` host-side hits, on-device misses (just confirmed 2026-05-09).** The current engine has a hard `0xBE000` PE-size gate and fixed offsets (`oneplus_canoe.c:597-633`). Host fixture is `LinuxLoader_infiniti.efi`; canoe's actual on-device ABL drifts. Same source, different binary, fixed-offset patch fails.
2. **Mode sprawl.** `MODE_DEBUG`, `MODE_TEMPLATE`, `FAKELOCKED`, `FAKELOCKED_DEBUG`, `MODE_1`, `MINIMAL`, `AUTO_DEBUG_MODE` all coexist in `Entry.c:55-81`. Knobs proliferated without taxonomy. EBS-mutate, UDT helper, `oem pull-logfs`, Toggle-Primary-OS, shell-boot — all dead-ends still in the tree.

This iteration ships:

1. A new repo (`gh repo create gbl-chainload --private -c`); the existing tree at `/home/vivy/gbl-chainload` becomes a read-only `dirty/` reference.
2. Patch engine v2: scan-based, stateless, dual-mode (build-time and runtime, single C source).
3. ABL embed (v1 feature): build-time pre-patched ABL embedded in GBL `.efi` as a cache; runtime scan+patch is the canonical path and runs whenever the cache key (input ABL hash) doesn't match. Designed to survive major OTAs that change the GBL load mechanism, and to enable a future KSUN-style flashable redistribute.
4. Three modes (mode-1, mode-2, mode-3) on a shared spine + universal baseline.
5. Three orthogonal build flags (`GBL_AUTO`, `GBL_DEBUG`, `GBL_VERBOSE`).
6. Fresh edk2 fork (D2): fork upstream at the recovery-escape-controls parent, cherry-pick only patches we want, no Toggle-Primary-OS / logfs-fastboot / shell-boot. Eventually a vector for booting desktop Linux from USB.
7. Cleanup of every dead-end.

## Architecture overview

```
   GBL .efi (mode-N)
        │
        ├─ CommonEarlyInit (LogFs, DeviceInfo, EnumeratePartitions)
        │
        ├─ key-window (AUTO toggle)
        │     ├─ AUTO=0 → timeout chain-loads silently (production)
        │     └─ AUTO=1 → timeout enters FastbootLib, awaits `oem escape`
        │
        ▼
   BootFlowChainLoad
        │
        ├─ AblUnwrap_LoadFromPartition  →  Pe, PeSize
        │
        ├─ TryEmbeddedAbl
        │     └─ if SHA256(Pe) == gEmbeddedAblSourceHash:
        │           use embedded directly, skip patch step
        │
        ├─ DynamicPatch_Apply (universal patches, then mode-specific)
        │
        ├─ ProtocolHook_InstallUniversalBaseline
        │
        ├─ ProtocolHook_InstallModeOverlay (1|2|3)
        │
        └─ LoadImage + StartImage
              │
              ▼
        patched ABL boot
```

The spine is shared. Mode selection switches only:
- which `InstallModeOverlay` runs after universal,
- which patches register beyond universal,
- (mode-2 only) which TA-payload mutator is wired into Qseecom/SPSS hooks.

## Patch engine v2

### Goals

- Stateless, scope-tagged patches usable from build-time tooling and runtime chainload via the same C source.
- No PE-size gates, no fixed offsets. Anchor-pattern scans only.
- CI gate: anchor uniqueness across all known fixtures.
- Borrow patches from `/home/vivy/gbl_root_canoe` as additional regression fixtures.

### Patch declaration

```c
typedef enum {
  PATCH_OK,
  PATCH_MISS,
  PATCH_AMBIGUOUS,        // multiple matches in scan domain — treat as miss
} PATCH_OUTCOME;

typedef enum {
  SCOPE_UNIVERSAL,        // expected to apply to every supported PE
  SCOPE_OEM_ONEPLUS,      // applies to OnePlus/Oppo Phoenix family
  SCOPE_MODE_1,           // mode-1 only
  // future: SCOPE_OEM_<other-oem>, SCOPE_MODE_2, SCOPE_MODE_3
} PATCH_SCOPE;

typedef PATCH_OUTCOME (*PATCH_APPLY)(UINT8 *Buf, UINT32 Size);

typedef struct {
  CONST CHAR8  *Name;
  PATCH_SCOPE   Scope;
  BOOLEAN       Mandatory;
  PATCH_APPLY   Apply;
} PATCH_DESC;
```

`Apply` is **stateless** — no PE-size gate, no fixed offsets, no globals. It calls `ScanFor` / `ScanForBoundedSection` and rewrites anchor-relative.

### Scan helpers

```c
typedef enum {
  SCAN_FOUND,             // exactly one match
  SCAN_NOT_FOUND,
  SCAN_AMBIGUOUS,         // >1 match — caller must declare a longer pattern
} SCAN_RESULT;

SCAN_RESULT
ScanFor (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,        // NULL = exact match
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );

SCAN_RESULT
ScanForBoundedSection (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  BOOLEAN      ExecOnly,             // restrict to PE executable sections
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );
```

`ScanFor` always scans the entire buffer (no `memmem`-style first-match) so ambiguity is detected. Patches must declare patterns long enough (16-32 bytes typical for AArch64 anchor sequences) to be unique across all supported fixtures.

### Patch authoring shape

```c
STATIC PATCH_OUTCOME
ApplyAvbRecoverableContinue (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (
        Buf, Size, /*ExecOnly=*/TRUE,
        kAvbResultDecisionAnchorPattern,
        kAvbResultDecisionAnchorMask,
        sizeof (kAvbResultDecisionAnchorPattern),
        &AnchorOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  // Rewrite anchor-relative.
  WriteInstrU32 (Buf, AnchorOff + 0x00, 0x52800038U);   // mov w24,#1
  if (!RewriteCbz (Buf,
                   AnchorOff + kRecoveryGateDelta,
                   /*reg=*/24,
                   AnchorOff + kRecoveryLockedOkDelta)) {
    return PATCH_MISS;
  }
  if (!RewriteCbz (Buf,
                   AnchorOff + kCommonGateDelta,
                   /*reg=*/24,
                   AnchorOff + kCommonOkRollbackDelta)) {
    return PATCH_MISS;
  }
  return PATCH_OK;
}
```

### Dual-mode invocation

The runtime path is unchanged in shape:

```c
DynamicPatch_Apply (Pe, PeSize, &Result);   // existing API
```

The build-time path is a new host binary `tools/abl-patcher`:

```bash
# Apply all universal + mode-1 patches to a host ABL fixture, write patched output.
tools/abl-patcher \
  --in images/canoe/A.07_2024_02_05/abl_a.bin \
  --mode 1 \
  --out build/embed/canoe-A.07_2024_02_05-mode-1.bin

# Anchor-uniqueness CI check (no rewrites; just verify each anchor matches once
# across each fixture).
tools/abl-patcher --check-anchors-only \
  --fixtures images/canoe/*/abl_*.bin \
              images/infiniti/*/LinuxLoader_*.efi \
              /home/vivy/gbl_root_canoe/images/*.efi
```

`tools/abl-patcher` is a host-runnable C program at `tools/abl-patcher/abl-patcher.c`. It links the same `PatchEngine.o`, `ScanLib.o`, and per-OEM patch object files used by the GBL `.efi` build. **No Python reimplementation; no risk of host/runtime divergence.**

### Borrowed-patch regression set

`/home/vivy/gbl_root_canoe` contains a working scan-based patch engine and patch set (per session note 2026-05-08, gbl_root_canoe's `patchlib.h` is the byte-level scan-and-replace reference). During implementation, enumerate gbl_root_canoe's patch list and import each viable patch as:

1. A patch fixture in `tests/fixtures/patches-gbl-root-canoe/` (input PE + expected output PE).
2. A regression test that runs `tools/abl-patcher` against the input and asserts byte-equivalence with the expected output.

This catches silent semantic drift between gbl_root_canoe's scan helpers and ours, and gives engine v2 a real corpus to exercise beyond patches 1, 7, 9.

### Stop-lines

- `tools/abl-patcher --check-anchors-only` failing on any fixture → CI build fails. Anchors must be unique on every supported fixture.
- A patch declared `SCOPE_UNIVERSAL` with `Mandatory=TRUE` that misses any fixture → CI build fails. (Universal mandatory must apply.)
- Borrowed gbl_root_canoe regression test failure → CI build fails.

## ABL embed track

### Why ship in v1

Two use cases the project lead called out:

1. **OTA survival.** Major Android upgrades (A16 → A17, etc.) may remove or change the GBL load mechanism in the new ABL. Embedding a known-good older ABL in our `.efi` lets users keep booting via gbl-chainload after such an upgrade.
2. **KSUN-manager-style redistribution.** A future module/zip applies gbl-chainload + an embedded patched ABL onto any flashed OTA, similar to how KSUN preserves root or how custom recoveries are reflashed.

### Build artifact

```c
// In GblChainloadPkg/Library/AblEmbedLib/GeneratedEmbed.c (generated by build).
// Empty if no embed; populated when scripts/build.sh --embed <abl-source-image>.
extern CONST UINT8  gEmbeddedAbl[];
extern CONST UINT32 gEmbeddedAblSize;
extern CONST UINT8  gEmbeddedAblSourceHash[32];   // SHA256 of pre-patch source
```

`scripts/build.sh --embed images/canoe/A.07_2024_02_05/abl_a.bin --mode 1` runs `tools/abl-patcher` to produce a patched PE, hashes the input source, generates `GeneratedEmbed.c`, and includes it in the build. Without `--embed`, the symbols are zero-length and the runtime path always scans+patches the active-slot ABL.

### Runtime priority

The embed is a build-time cache keyed by the input ABL's hash. The runtime scan+patch path is canonical and always present; the embed only short-circuits it when the cache key matches.

```text
BootFlow:
  AblUnwrap_LoadFromPartition  →  Pe, PeSize

  // Cache check.
  if (gEmbeddedAblSize > 0
      && SHA256(Pe, PeSize) == gEmbeddedAblSourceHash):
    // Active-slot ABL exactly matches the input we pre-patched at build time.
    // Use embedded directly; the build-time path already produced this PE.
    Free(Pe); Pe = (VOID*)gEmbeddedAbl; PeSize = gEmbeddedAblSize;
    LogFsLog("BootFlow: using embedded pre-patched ABL (hash match)");
    goto LoadImage;

  // Canonical path — always available, runs whenever cache misses or is absent.
  DynamicPatch_Apply(Pe, PeSize, &Result);
  ...
```

Embed is **opt-in** at build time. v1 default builds ship without embed; release builds targeting specific OTAs build with embed.

### Out of scope for v1

Full FV-replacement (a tiny FV inside GBL `.efi` with patched ABL at top, loaded via UEFI's FV path) is a parallel research thread documented in `docs/re/gbl-load-mechanism.md` (Ghidra-annotate canoe's GBL load entry). May fold into v2 if it yields cleaner ergonomics than the `LoadImage`-from-blob approach.

## Three-mode taxonomy

```
   layer attacked            mode-1            mode-2              mode-3
 ──────────────────────   ──────────────   ──────────────────   ───────────────
 QCOM_VERIFIEDBOOT        MUTATE (fake-    PASS-THROUGH         PASS-THROUGH
   READ_CONFIG /            locked view)   (ABL stays honest)   (ABL stays honest)
   VBDeviceInit
 QCOM_VERIFIEDBOOT        SWALLOW          SWALLOW              SWALLOW
   WRITE_CONFIG /          (universal)     (universal)          (universal)
   VBDeviceResetState
 SCM TZ_BLOW_SW_FUSE      DROP             DROP                 DROP
   (and rollback-fuse     (universal)     (universal)          (universal)
   SIPs)
 OplusSec QSEE 0x0A       DROP             DROP                 DROP
   write_rpmb_boot_info   (universal)     (universal)          (universal)
 libavb recoverable-      PATCH-PRESENT   no-op                no-op
   continue split         (mode-1 only)   (ABL is honest)      (ABL is honest)
 KM cmd 0x201 SET_ROT     no-op           SPOOF (profile)      no-op
 KM cmd 0x207 SET_VERSION no-op           SPOOF (profile)      no-op
 KM cmd 0x208 SET_BOOT    no-op           SPOOF (profile)      no-op
   _STATE
 SPSS Share               no-op           SPOOF (profile,      no-op
   KeyMintInfo                            vtable verify gate)
 OplusSec 0x09 read_      no-op           SPOOF (profile)      no-op
   rpmb_boot_info
 oplusreserve1 BlockIO    ENUMERATE        ENUMERATE            ENUMERATE
   writes                  (doc only)      (doc only)           (doc only)
```

Universal-baseline rules apply to every mode unconditionally.

### Why patch9 is mode-1 only

Mode-2 and mode-3 leave ABL honest: `IsUnlocked()` returns TRUE, libavb gets `AllowVerificationError=TRUE`, and recoverable AVB results naturally produce populated `SlotData`. Patch9 is needed only when ABL is fakelocked at the VerifiedBoot view — i.e. mode-1. Patch9 lives at `GblChainloadPkg/Library/DynamicPatchLib/mode_1/mode_1.c` (not `universal/`).

### Why fuse-drop is universal

Persistent and destructive. Once a TZ soft-fuse blows, it stays blown; re-flashability disappears. Modes 1, 2, and 3 all hold a "no persistent state" contract — letting a fuse blow on any mode silently destroys escape-routes. Drop it always.

### EFISP-mode "no-persistence" contract

A user can revert by deleting the GBL `.efi` from EFISP. To preserve that escape-route, gbl-chainload's universal baseline refuses to persist any state mutation that survives without the `.efi`:

- VB `WRITE_CONFIG` and `VBDeviceResetState` swallowed (no RPMB write).
- OplusSec `0x0A` `write_rpmb_boot_info` dropped (no Phoenix RPMB write via QSEE).
- TZ soft-fuse blow dropped.
- Anti-rollback bumps (TZ_UPDATE_ROLLBACK_VERSION_ID, TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID) classified in `docs/re/scm-fuse-classification.md`; flip to DROP only with classification row in place.

The unidentified Oplus fastboot-gate mechanism is enumerated in `docs/re/oplusreserve1-write-paths.md` (Ghidra cross-refs only this iteration; BlockIO write filter follows in a sub-iteration).

### Mode-1 — protocol-hook fakelock

ABL sees a locked DeviceInfo and builds KM/SPSS itself off that view. Smallest spoof surface. Today's `mode-fakelocked` migrates onto the new spine.

**Hook overlay (atop universal):**
- `VBRwDeviceState(READ_CONFIG)` mutating wrapper — call original; clear `is_unlocked` and `is_unlock_critical` in returned buffer.
- `VBDeviceInit` — clear flags pre and post.
- All other VB slots pass-through (logged structured under `GBL_VERBOSE=1`).

**Patches:** universal-only + `patch9-avb-locked-recoverable-continue` (mandatory, scoped `SCOPE_MODE_1`).

**Stop-line:** `InstallVerifiedBootMode1Overlay` fails closed if `VBRwDeviceState` or `VBDeviceInit` is NULL on the located protocol → chainload aborts → fall through to FastbootLib.

### Mode-2 — TA-payload spoof, ABL honest

ABL sees `is_unlocked=TRUE`. Spoofs land at QSEE/SPSS boundaries via per-OTA profile.

Project lead framing: "everything but ABL is locked". KM HAL, Keymint, SPSS / secure co-processor (Widevine and similar) see locked-equivalent boot state; AVF/PVMFW continue to use the device's real UDS/FRS — no DICE chain mutation.

**Hook overlay (atop universal):**
- Qseecom `SendCmd` wrapper dispatching by `(Handle, FirstWord)`:
  - KM `0x201` SET_ROT — substitute profile RoT digest in *outgoing* send buffer pre-call.
  - KM `0x207` SET_VERSION — substitute profile-encoded version + SPL pre-call.
  - KM `0x208` SET_BOOT_STATE — substitute IsUnlocked=0, profile pubkey, color=0 pre-call.
  - OplusSec `0x09` read_rpmb_boot_info — substitute profile bytes in *response* buffer post-call.
- SPSS `ShareKeyMintInfo` overlay — verify vtable shape (see below); on verify, substitute the packed `{KMSetRotReq, KMSetBootStateReq, KMSetVbhReq}` triple from profile.
- VBSendRot pass-through. RoT injection happens at KM `0x201`.

**SPSS vtable verify (concrete check):**

At `InstallProtocolHookMode2Overlay` time, locate `QCOM_SPSS_PROTOCOL` and fingerprint *every* slot in the protocol vtable (not a hand-picked subset — covering all slots eliminates the "we missed the slot that shifted" risk):

```c
QCOM_SPSS_PROTOCOL *Spss; gBS->LocateProtocol(...);
UINT32 ExpectedFingerprint = SPSS_VTABLE_FINGERPRINT_CANOE;  // build-time constant
UINT32 H = 0;

// For each function pointer in the vtable, hash the first 8 ARM64 instructions
// (8 × 4 = 32 bytes) of the prologue and fold into a running CRC32.
// Iterating all slots avoids the "we sampled too few" failure mode.
for (slot in Spss->[every function pointer]) {
  H = Crc32Step(H, HashFirstNInstructions(slot, /*N=*/8));
}

if (H != ExpectedFingerprint) return EFI_NOT_READY;  // fail closed
```

Hashing function-prologue *bytes* tolerates load-base relocation (no absolute addresses appear in the first 8 ARM64 instructions of a typical EFIAPI prologue) while still proving the vtable hasn't shifted. Expected fingerprint is produced at build time by `tools/abl-patcher --extract-spss-fingerprint --in <LinuxLoader_canoe.efi>` over the same slot iteration logic, so build-time and runtime are bit-identical by construction.

**Patches:** universal-only. No mode-2-specific patches in v1.

**Stop-line:** any of (Qseecom protocol missing, SPSS protocol missing, SPSS vtable shape mismatch) → fail closed → chainload aborts.

### Mode-2 profile structure (typed C structs, not opaque blobs)

Profiles are typed C struct values. Mode-2 hook code reads field values and constructs wire bytes at runtime using QcomModulePkg's own `KMSetRotReq` / `KMSetBootStateReq` / etc. struct definitions (sourced from `edk2/QcomModulePkg/Library/avb/KeymasterClient.h` or equivalent header).

```c
// GblChainloadPkg/Include/Library/Mode2ProfileLib.h
typedef struct {
  // KM 0x201 SET_ROT
  UINT8   RotPubkeyDigest[32];          // SHA256(stock_avb_pubkey || 0x00)
  // KM 0x207 SET_VERSION
  UINT32  OsVersion;                    // e.g. 0x40000
  UINT32  OsSpl;                        // e.g. 0x9A4 (security patch level)
  // KM 0x208 SET_BOOT_STATE
  UINT8   BootStatePubkey[32];          // matches RotPubkeyDigest in normal flow
  UINT32  BootStateUnlocked;            // 0
  UINT32  BootStateColor;               // 0 = GREEN
  // OplusSec 0x09 response (variable; sized by profile)
  UINT8   OplusBootInfoBytes[OPLUS_BOOT_INFO_LEN_MAX];
  UINT32  OplusBootInfoLen;
  // SPSS triple — packed by mode-2 hook code at runtime
  UINT8   SpssVbh[32];
  // device + OTA tags for build-time consistency check
  CHAR8   DeviceTag[16];                // e.g. "canoe"
  CHAR8   OtaTag[64];                   // e.g. "A.07_2024_02_05"
} MODE_2_PROFILE;

// profiles/canoe/A.07_2024_02_05/profile.h (gitignored, generated)
extern CONST MODE_2_PROFILE  gMode2Profile;
```

#### Profile production pipeline

```
images/canoe/A.07_2024_02_05/        (gitignored — stock OTA partition images)
   abl_a.bin
   boot.img
   recovery.img
   dtbo.img
   vbmeta.img

logs/.../UefiLogSaved*.txt           (saved bootloader_log captures showing
                                       KM 0x208, SPSS, OplusSec 0x09 lines —
                                       e.g. mode-fakelocked-debug-no-ebs)

           │
           ▼
scripts/extract-mode2-profile.py
           │
           ▼
profiles/canoe/A.07_2024_02_05/
   manifest.json     (tracked: source OTA hashes, capture method,
                       per-field SHA256s, capture date)
   profile.h         (gitignored: generated typed-struct values)
```

Mode-2 build embeds `profile.h` and emits a `_Static_assert` that the compile-time `GBL_MODE_2_DEVICE` / `GBL_MODE_2_OTA` strings match the profile's `DeviceTag` / `OtaTag`. Mismatch → compile fails. No fallback or default profile.

The existing 2026-05-08 `mode-fakelocked-debug-no-ebs` capture (commit `cc8a48e`, `logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e/`) is the source for profile #1; no new device run needed before mode-2 implementation.

### Mode-3 — minimal, leaf-survival gamble

Universal baseline only. No mode-specific overlay. The fuse-drop (universal) is mode-3's central trick.

**Patches:** universal only.

**Validation goal:** verify that the KM root cert chain still validates after the fuse drop on a stock-signed boot (no custom recovery yet). If yes, mode-3 is a viable seed for a follow-up userspace teesim/RKP sub-iteration. If no, mode-3 is shelved; mode-2 stays the live carryover. We can iterate minimal patches on top during exploration.

## Build flags

Three orthogonal flags:

| Flag | Value | Semantics |
|---|---|---|
| `GBL_AUTO` | `0` | Timeout chain-loads silently (production default). VolUp during key window is moot — same outcome. |
| `GBL_AUTO` | `1` | Timeout enters FastbootLib, awaits `oem escape` (host-driven test default). VolUp forces immediate chainload. |
| `GBL_DEBUG` | `0` | Screen output: nothing unless key interrupt or fatal error. Logfs / `EFI_DEBUG` always logs regardless. Production default. |
| `GBL_DEBUG` | `1` | Screen output: every patch outcome, hook install, policy decision printed to screen as it happens. Useful for live device demos. |
| `GBL_VERBOSE` | `0` | Only mode-required hooks installed. |
| `GBL_VERBOSE` | `1` | Full observation suite installed (qseecom-decoder, scm-decoder, ebs, spss-observer, KM-decoder, OplusSec-decoder). Logs every TZ call we don't hook in a structured-readable form. |

These compose. Production user build: `GBL_MODE=1 GBL_AUTO=0 GBL_DEBUG=0 GBL_VERBOSE=0` — silent boot, only critical hooks. Dev capture build: `GBL_MODE=1 GBL_AUTO=1 GBL_DEBUG=1 GBL_VERBOSE=1` — host-driven, screen+log everything, full observation.

`scripts/build.sh --mode {1|2|3} [--auto] [--debug] [--verbose] [--embed <abl.bin>] [--profile <dev>/<ota>]` produces `dist/mode-N{-auto}{-debug}{-verbose}{-embed}.efi`.

Default `runall` builds the four most-used artifacts:

| artifact | flags |
|---|---|
| `dist/mode-1.efi` | `GBL_MODE=1` (production) |
| `dist/mode-2.efi` | `GBL_MODE=2 --profile canoe/A.07_2024_02_05` (production) |
| `dist/mode-1-dev.efi` | `GBL_MODE=1 --auto --debug --verbose` (host-driven dev capture) |
| `dist/observe.efi` | `--auto --debug --verbose` no-mode (pure observation, no mutations) |

Other combinations land via explicit `scripts/build.sh` invocations.

## Repo bootstrap

### New repo

1. `gh repo create gbl-chainload --private -c` (clones locally after creation).
2. **Path collision note:** there is already `/home/vivy/gbl-chainload`. Either rename the old to `/home/vivy/gbl-chainload-dirty/` first, or run `gh repo create` from a different parent dir and clone elsewhere. Pre-implementation step.
3. First commits in order:
   - `LICENSE`, `README.md`, `.gitignore` (gitignores: `dist/`, `Build/`, `images/`, `profiles/*/profile.h`, `profiles/*/[!manifest]*.bin`, `logs/`, `.re-notes-staging/`).
   - `edk2/` submodule (see edk2 D2 below).
   - `GblChainloadPkg/` skeleton (Application/, Library/, Include/, .dsc).
   - `tools/abl-patcher/` skeleton.
   - `scripts/`, `tests/`, `docs/`, `docker/`.
4. **Carry forward** `.re-notes/sessions/` and any RE doc worth keeping by **copying files**, not by importing git history. The old repo's commit history stays in `dirty/`; the new repo starts clean.

Old `/home/vivy/gbl-chainload` becomes read-only `dirty/` reference. We never `git clone --no-local` from it.

### edk2 fork D2 (fresh fork)

1. Identify upstream EDK-II tag at the parent of old fork's `2c4c2d629c FastbootLib: add recovery escape controls`.
2. `gh repo create edk2-gbl-chainload --private -c` (or fork upstream EDK-II directly, depending on URL preference).
3. Cherry-pick from old fork:
   - `2c4c2d629c FastbootLib: add recovery escape controls`.
   - Any other QcomModulePkg patches the cleanup pass identifies as keep-worthy (enumerated during implementation; deferred to writing-plans).
4. Do **not** carry: TogglePrimaryOS, logfs-fastboot exfil (`oem pull-logfs`), shell-boot escape, EBS-mutate scaffolding, UDT-helper-related infra, any patches that depend on those.
5. Pin new gbl-chainload's `edk2/` submodule to the fresh fork.

The clean foundation enables a downstream goal called out by the project lead: this fork eventually becomes the vector for booting desktop Linux from USB. Carrying minimal divergence from upstream now keeps that future tractable.

## Cleanup pass

The new repo skips importing every dead-end. No commit removing the dead-ends is needed because they never enter the new tree:

1. **EBS-mutate scaffolding** — no `EbsMutate.{c,h}`, no `GBL_DEBUG_EBS_MUTATE` knob, no `tests/044_bootargs_rewrite_harness.sh`.
2. **UDT helper** — no `Patch8 (udt-helper)`, no `GBL_DEBUG_UDT_HELPER`, no `tests/043_update_device_tree_callsite_anchor.sh`, no `docs/re/update-device-tree-callsite-helper.md`.
3. **`oem pull-logfs`** — no `scripts/pull-logfs.sh`. `scripts/test-device-{automatic,manual}.sh` rely solely on kernel-side `bootloader_log` capture.
4. **Toggle Primary OS** — edk2 D2 enforces this.
5. **Shell-boot escape** — edk2 D2 enforces this.
6. **Seven-knob mode sprawl** — replaced by `GBL_MODE` (1/2/3) + `GBL_AUTO=0|1` + `GBL_DEBUG=0|1` + `GBL_VERBOSE=0|1`.
7. **`--debug-variant` matrix** (`patch-only`, `no-ebs`, `ebs-wrapper-only`, `ebs-fdt-probe`, `ebs-scan`, `ebs-no-bootconfig`, `ebs-no-close`, `udt-helper`) — gone. Anyone needing finer slicing branches off.

## Validation strategy

### Host (CI green from new repo `main` HEAD)

- `tests/runall.sh` green: build smoke, signature lint, trampoline validator, dynamic patch harness with anchor-uniqueness on canoe + infiniti + gbl_root_canoe fixtures, mode taxonomy lint.
- `tools/abl-patcher --check-anchors-only` exits 0 across full fixture set.
- gbl_root_canoe regression patches produce byte-equivalent output via our engine.
- `tests/047_mode2_profile_lint.sh` validates manifest + struct-field integrity for every profile.
- `tests/048_mode2_spss_vtable_lint.sh` cross-checks SPSS vtable fingerprint constants vs canoe LinuxLoader fixture.

### Device (canoe)

**Mode-1:**
- `dist/mode-1.efi` boots stock-signed image. `bootloader_log` shows green/locked cmdline, KM 0x208 with `isUnlocked=0` + real pubkey, oplus cmdline `oplusboot.verifiedbootstate=green`, and (key new evidence) `DynamicPatch: patch9-avb-locked-recoverable-continue OK`.
- `dist/mode-1-dev.efi` produces the same boot path with full hook traces and screen output for live demo.

**Mode-2:**
- Build `--mode 2 --profile canoe/A.07_2024_02_05` from the existing 2026-05-08 capture.
- Boot custom recovery (TWRP-style). Confirm: KM 0x208 wire bytes match profile (sniff via verbose log), `androidboot.verifiedbootstate=green`, `dumpsys keymint` root cert matches stock baseline.

**Mode-3:**
- Boot stock-signed image with `dist/mode-3.efi`. `bootloader_log` shows `scm-sip | smcid=0x02000801(TZ_BLOW_SW_FUSE_ID) | DROPPED`. Pull KM root cert chain; verify against stock baseline.
- If green, attempt custom-recovery boot. KM HAL is expected to fail (no TA spoofing); that failure is the green-light to start userspace teesim/RKP sub-iteration.

**Embed (any mode):**
- `scripts/build.sh --mode 1 --embed images/canoe/A.07_2024_02_05/abl_a.bin` produces `dist/mode-1-embed.efi`.
- Cache hit: boot device with active-slot ABL == embed source. `bootloader_log` shows `BootFlow: using embedded pre-patched ABL (hash match)`. Mode-1 hooks install and behavior matches the no-embed mode-1 build.
- Cache miss: boot device with active-slot ABL != embed source. `bootloader_log` shows the runtime scan+patch path running and producing the same patched PE the no-embed build would.
- Determinism: the patched PE produced by the embed path and the patched PE produced by the runtime path on the same input must be byte-identical (same C source, same patch logic). `tests/050_embed_determinism.sh` asserts this on the host.

## Stop-lines (whole iteration)

- Do not import EBS-mutate, UDT helper, `oem pull-logfs`, Toggle-Primary-OS, or shell-boot. Retrieve from `dirty/` if a future iteration justifies them.
- Do not let any mode artifact skip the universal baseline.
- Do not let `GBL_DEBUG=0` or `GBL_VERBOSE=0` artifacts install observation hooks beyond the per-mode minimum.
- Do not let mode-2 fall back to a generic / cross-device / cross-OTA profile. Compile-time refusal.
- Do not flip an additional SCM SIP from LOG-ONLY to DROP without a row in `docs/re/scm-fuse-classification.md`.
- Do not push mode-2 device tests on a device whose profile hasn't been built from a captured saved bootloader_log capture for that device + OTA.
- Do not push mode-3 + custom-recovery until a userspace teesim/RKP sub-iteration exists.
- Do not delete `.re-notes/sessions/` — durable RE record. Carry forward by file copy.

## Out of scope (v1)

- Full FV-replacement ABL embed (B3 — parallel research thread, not v1).
- Userspace teesim / RKP / Widevine RKP shims (mode-3 follow-up).
- Mode-2 secretkeeper 0x404 spoofing (4 KB device-binding blob; full RE pending).
- Mode-2 Mink IPC Transport B mutation (RKP/BCC; observation-only).
- BlockIO write filter for `oplusreserve1` / `persist` / `devinfo` partitions (enumerate in `docs/re/oplusreserve1-write-paths.md`; sub-iteration adds the filter).
- Mode-2 AVB partition-read façade (`patch11`) — TA-payload spoof at KM/SPSS layer covers KM HAL bring-up.
- Multi-device matrix beyond canoe + infiniti.
- Reviving any of: EBS-mutate, UDT helper, `oem pull-logfs`, Toggle-Primary-OS, shell-boot escape.
- Rebasing onto a release-clean history (gated on release-readiness).

## Implementation outline

Detailed agent dispatch deferred to the writing-plans phase. Sequence sketch:

1. **Foundation** — new repo, fresh edk2 fork, `tools/abl-patcher` host binary skeleton, gbl_root_canoe patches imported as fixtures, anchor-uniqueness CI.
2. **Patch engine v2** — scan-based engine, port patches 1, 7, 9 as anchor-relative. Validate against canoe + infiniti + gbl_root_canoe fixtures.
3. **Universal baseline overlay** — VB WRITE/Reset swallow, SCM TZ_BLOW_SW_FUSE drop, OplusSec 0x0A drop.
4. **Mode-1 overlay** — port today's fakelocked VB READ/Init mutations + patch9 (mode-1 scope).
5. **ABL embed lib** — `--embed` build flag, runtime hash-match priority, `tools/abl-patcher` pre-patched output.
6. **Mode-2 overlay + profile system** — typed-struct profile, `extract-mode2-profile.py`, Qseecom + SPSS overlays, vtable fingerprint extraction tool.
7. **Mode-3 overlay** — empty (universal-only) + `docs/re/scm-fuse-classification.md`.
8. **RPMB enumeration** — `docs/re/oplusreserve1-write-paths.md` (Ghidra cross-refs, no code lands).
9. **GBL load mechanism RE** — `docs/re/gbl-load-mechanism.md`, parallel exploration; outcome may fold into v2 of the rewrite.
10. **Build matrix + CI matrix** — full `scripts/build.sh` argv handling; `tests/runall.sh` covers the four most-used variants.

End-state checklist (for verifier):

- [ ] New repo at `gh:1vivy/gbl-chainload` with `main` green from HEAD.
- [ ] Fresh edk2 fork repinned, no Toggle-Primary-OS / logfs-fastboot / shell-boot.
- [ ] `tools/abl-patcher --check-anchors-only` green on full fixture set.
- [ ] mode-1.efi: stock + custom-recovery boots produce green/locked cmdline + green KM 0x208 + patch9 OK.
- [ ] mode-2.efi: custom-recovery boot produces green/locked cmdline + KM root cert matches stock baseline.
- [ ] mode-3.efi: stock boot retains valid KM root cert chain after fuse drop.
- [ ] `--embed <abl.bin>` builds; runtime uses embedded when active-slot matches source hash, otherwise falls back to runtime patch.
- [ ] `docs/re/scm-fuse-classification.md` and `docs/re/oplusreserve1-write-paths.md` exist, reviewed.
- [ ] `docs/re/gbl-load-mechanism.md` exists with Ghidra-anchored entry points (parallel track).
- [ ] No EBS-mutate / UDT helper / pull-logfs / Toggle-Primary-OS / shell-boot code anywhere.

## Open items deferred to writing-plans

- Enumerate gbl_root_canoe patches to import as regression fixtures. (Need to read `/home/vivy/gbl_root_canoe`'s patch sources during plan writing.)
- Enumerate edk2 fork patches to cherry-pick beyond `2c4c2d629c`. (Need to walk old fork's commit log.)
- Resolve `/home/vivy/gbl-chainload` path collision before `gh repo create -c`.
- Concrete SPSS vtable slot list (which slots to fingerprint, in what order) — depends on canoe LinuxLoader Ghidra inspection.
- Choice of canoe LinuxLoader fixture path under `images/canoe/A.07_2024_02_05/` (whether to use `abl_a.bin` extracted from stock OTA, or a different artifact).

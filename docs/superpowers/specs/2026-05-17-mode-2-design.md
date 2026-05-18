# Mode-2 design

Status: design approved 2026-05-17. Implementation decomposes into the PR
slices in §8.

## 1. Goal & scope

Mode-2 is the **custom-ROM-user mode**. Unlike mode-1 (which lies to ABL about
lock state so ABL itself builds locked KeyMaster payloads), mode-2 keeps **ABL
honest** — ABL sees the real unlocked/orange state. gbl-chainload instead
rewrites the TA payloads ABL sends to the secure world, so TZ KeyMaster and the
SPU latch a coherent **locked / green** verified-boot state.

In scope — rewrite, from a per-OEM/per-OTA profile:

- `SET_ROT` (`0x201`)
- `SET_VERSION` (`0x207`)
- `SET_BOOT_STATE` (`0x208`)
- `SET_VBH` (`0x211`)

at the `QseecomSendCmd` boundary, and the same RoT/BootState/Vbh values mirrored
through the SPSS `ShareKeyMintInfo` struct to the SPU.

Success bar — **key attestation passes**.

Best-effort, not promised — Widevine, Soter, default-RKP. The canoe reference
build's reverse-engineering notes show the ABL-stage QSEE/SPSS spoof reliably
produces clean key attestation but does not by itself satisfy Widevine/RKP
provisioning, which happens at Android runtime past the ABL handoff. Mode-2
gives those their best chance by latching a coherent boot state but makes no
guarantee.

Non-goals:

- No `QCOM_VERIFIEDBOOT_PROTOCOL` device-state mutation — ABL stays honest.
- `0x218 FBE_SET_SEED` and `0x219 GENERATE_FRS_AND_UDS` request/response
  buffers are **never mutated** — they carry the FBE class-key seed and the
  per-device UDS/FRS secrets respectively.
- **No DICE Mode patch in v1.** Researched and deferred — see §7.

## 2. EFI runtime (`GBL_MODE == 2`)

A new mode overlay, mirroring the existing `Mode1Overlay`:

- New `GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.{c,h}`, gated
  `#if (GBL_MODE == 2)`. Holds the profile-driven rewrite policy functions.
- `QseecomHook.c` — a `GBL_MODE == 2` block in `HookedSendCmd` that rewrites
  the four KM send buffers in-place from the loaded profile **before**
  forwarding to TZ. Reuses the existing `KmDecodeKnownCmd` cmd-id parsing.
  Mode-1's `Mode1Policy_ShouldDropQseeOplusSec` path is mode-1-only and is not
  invoked in mode-2.
- `SpssHook.c` — a `GBL_MODE == 2` block in `HookedShareKeyMintInfo` that
  rewrites the packed `{RootOfTrust, BootInfo, Vbh}` struct from the profile
  before forwarding to the SPU.
- `InstallAll.c` — in mode-2, the Qseecom and SPSS hooks become **required**
  (as VerifiedBoot/Qseecom are required in mode-1). A mode-2 build that cannot
  install them aborts chain-load and falls through to FastbootLib.
- `VerifiedBootHook.c` — mode-2 does **not** invoke any `Mode1Policy_*`
  function; the verified-boot device-state passes through unmodified.
- Universal baseline ships unchanged (TZ soft-fuse drop, `oplusreserve1`
  reserve-token preservation, SCM and BlockIo hooks).

Patch engine — **retained** in mode-2 builds. `DynamicPatchLib` with the
universal patch set stays in, and `BootFlow.c` keeps the standard tier chain:
Tier-1 (cached ABL overlay) → Tier-2 (dynamic patch) → Tier-3 (FastbootLib).
The universal patches — in particular the efisp-recursion guard that prevents
a patched ABL from re-entering the EFISP load path — remain the fallback when
no cached overlay is present. Mode-2 adds **no** OEM-scoped or `mode_2`-scoped
patch directories; OEM-specific ABL patching is handled host-side (see §5/§6).

Determinism — the profile is a fixed compiled binary, so every rewritten
payload is byte-identical across boots and across normal/recovery boots. This
satisfies the KeyMaster hidden-set invariant (the `0x208` triple must not
change between boots or `/data` wipes) for free: there is no recomputation and
no time/random input.

## 3. Profile — format & delivery

The profile carries **only AVB spoof values**. It does not carry OEM/device
identity — device identification for OEM-patch selection is a separate ZIP-side
concern (§6).

Authoring form — `gbl-chainload_profile.xml`, human-editable. Fields:

- `is_unlocked` — `0`.
- `color` — `0` (GREEN).
- AVB public key — the helper (§5) derives from it:
  - `rot_digest = SHA256(pubkey ‖ 0x00)` — the `SET_ROT` digest.
  - `pubkey_digest = SHA256(pubkey)` — the `SET_BOOT_STATE` PublicKey field.
- `system_version` — bootloader-domain encoding
  `(Major << 14) | (Minor << 7) | SubMinor`.
- `system_spl` — bootloader-domain encoding
  `(Day << 11) | ((Year - 2000) << 4) | Month`.
- `vbh` — the vbmeta digest (`SET_VBH`).

Encodings match the canoe reference build's `keymaster_overrides.py`
encoders exactly; feeding TZ a value in the wrong domain produces a malformed
CSR boot-patch-level.

Runtime form — GBLP1 type `0x0010` (`mode2_profile`). A typed-struct binary TLV
entry, compiled from the XML, appended to the installed `gbl-chainload.efi` on
the EFISP raw partition alongside the existing `0x0001` (`cached_abl`) entry.
The type code is already reserved in `docs/project/decisions.md` ("Cache-ABL
container format"). `GblPayloadLib` is extended to expose the `0x0010` entry to
`BootFlow.c`, which hands the parsed profile to `Mode2Overlay`.

When the XML does not exist, the vbmeta→profile helper (§5) auto-populates it
from `stock_vbmeta.img`.

## 4. Profile-failure behavior

If there is no `0x0010` entry, or the entry fails validation (container CRC,
per-entry SHA-256, schema, or version) at boot:

- gbl-chainload **continues with an honest boot** — the QSEE/SPSS rewrite
  policy is not applied. An honest boot is self-consistent (no spoof anywhere)
  and carries no split-brain / FBE hazard; attestation simply fails.
- The failure is logged.
- The fastboot screen shows a **red warning line** modeled on the existing
  `AVB WARNING - ...` line, e.g.
  `MODE-2 PROFILE MISSING — booting honest, attestation will fail`.

A partial or corrupt spoof is the actual hazard (it can leave KeyMaster
half-set, which risks FBE/metadata-decryption corruption); a clean honest boot
and a clean full spoof are both safe. Hence: missing or invalid → honest boot,
never a partial application. Auto-continue; no forced drop into FastbootLib
(the user can still reach FastbootLib through the normal `Escape` path).

## 5. Tooling

- **vbmeta→profile helper** — generalizes the canoe reference
  `tools/ota_to_overrides.py`: parse a `vbmeta.img`, extract the AVB public
  key, compute `rot_digest` and `pubkey_digest`, decode OS version and SPL into
  the bootloader-domain encodings, and emit or update
  `gbl-chainload_profile.xml`. Shipped **both** as a host-side Python script and
  as a device-side aarch64-Android static binary, alongside the existing
  `fv-unwrap` / `abl-patcher` / `gbl-pack` / `gbl-commit` recovery toolchain
  (NDK r27, `scripts/build-recovery-tools.sh`).
- **XML → GBLP1 `0x0010` compiler** — added to `tools/gbl-pack`, which already
  builds the `0x0001` entry. Produces the typed-struct binary profile entry.
- **`scripts/build.sh`** — accept `--mode 2` (artifact `dist/mode-2[flags].efi`,
  `GBL_MODE=2` passed into the container build).
- **`images/`-drop orchestration** — a single script for the common case: the
  user drops the stock `abl` and `vbmeta` images into `images/` and runs it.
  The script builds the mode-2 EFI, runs `fv-unwrap` + `abl-patcher` to produce
  the cached ABL, runs the vbmeta→profile helper, packs the `0x0001` +
  `0x0010` entries via `gbl-pack`, and emits a ready-to-flash
  `gbl-chainload.efi` + GBLP1 overlay.

## 6. Mode-2 ZIP

A **separate** mode-2 ZIP, distinct from the mode-1 gbl-chainload /
recovery-graft ZIP, layered on top of the cache-ABL work.

- Reads `build.prop` to identify the OEM and device. This identification is
  used **only** to select which OEM-addition ABL patches `abl-patcher` applies
  when building the cached ABL. It is not stored in the profile and is not a
  profile-identity check.
- Builds/validates the mode-2 profile. Profile staleness is detected by
  re-deriving the expected spoof values from the on-device stock vbmeta and
  comparing; a stale or missing profile fails closed with explicit user
  instructions.
- Drives the EFISP write of the EFI + GBLP1 overlay as a user-confirmed step
  (no autonomous non-HLOS flashing).

## 7. Hazards, deferrals & non-goals

- **One-time FBE transition wipe.** Enabling mode-2 changes the KeyMaster Root
  of Trust that the existing `/data` encryption is bound to; the first boot
  under mode-2 will fail to unwrap FBE keys and trigger a `/data` format. This
  is expected and acceptable (the test device's `/data` is not load-bearing),
  but the ZIP and docs must warn loudly. After the format, fresh FBE keys are
  created on the first normal boot and bind to the spoofed state, so subsequent
  boots are stable.
- **FBE_SET_SEED (`0x218`) — under separate investigation.** Whether the FBE
  class-key seed, or its interaction with the spoofed KM boot state, is what
  breaks FBE key wrapping is being investigated as a separate task; findings
  will land in `docs/project/fbe-set-seed-investigation.md`. Mode-2 does not
  mutate `0x218` regardless.
- **DICE Mode debug-poison — deferred.** `AvbPopulateBccParams.c` sets the DICE
  mode to `kDiceModeDebug` on the unlocked path at two sites — `PopulateBccParams`
  (`:147`) and `SetDummyBccParams` (`:16`). This poisons the BCC/DICE chain that
  feeds RKP. The `bcc_params->Mode` field is set in ABL code and does not travel
  over the `0x219` wire, so it cannot be fixed by a protocol hook — it requires
  an ABL code patch (`kDiceModeDebug` → `kDiceModeMaintenance` at both sites),
  which would live in the host `abl-patcher`. Left out of v1; the exact patch is
  recorded here so it can be added if attestation needs it.
- **Anti-rollback / version-set SCM SmcIds — deferred audit.** The canoe
  reference `scm_hook.h` flags that ABL fires SCM calls beyond
  `TZ_BLOW_SW_FUSE` that mutate TZ-side persistent state. Enumerating them is a
  recorded follow-up research task, not a v1 deliverable.
- **No autonomous non-HLOS flashing.** The EFISP write stays a user-driven ZIP
  step, consistent with the project safety boundary.

## 8. Implementation decomposition (PR slices)

One design, landed as separate feature branches / PRs against `main`:

1. **Profile format** — XML schema + GBLP1 `0x0010` typed-struct layout +
   `GblPayloadLib` reader extension to expose the `0x0010` entry.
2. **Mode-2 EFI runtime** — `Mode2Overlay.{c,h}`, the `GBL_MODE == 2` rewrite
   blocks in `QseecomHook.c` and `SpssHook.c`, `InstallAll.c` required-hook
   handling, `--mode 2` in `build.sh`, and the FastbootLib red warning line.
3. **Profile tooling** — the vbmeta→profile helper (host Python + device
   aarch64 binary) and the XML→`0x0010` compiler in `gbl-pack`.
4. **`images/`-drop build orchestration** — the single-command bundle builder.
5. **Mode-2 ZIP** — `build.prop` OEM-patch selection, profile validation and
   staleness detection, user-driven EFISP write.

Slice 2 depends on slice 1 (the profile struct). Slices 3–5 depend on slice 1
(the format) and slice 2 (the runtime contract).

## Appendix — ABL→secure-world surface map

Swept from `QcomModulePkg` (`KeymasterClient.c`, `AvbPopulateBccParams.c`,
`VerifiedBoot.c`, all `IsUnlocked()` call sites) and the canoe reference hooks.

| Boundary | Call | Mode-2 v1 disposition |
|---|---|---|
| QSEECOM → KeyMaster TA | `0x201 SET_ROT` | rewrite from profile (hook) |
| | `0x207 SET_VERSION` | rewrite from profile (hook) |
| | `0x208 SET_BOOT_STATE` | rewrite from profile (hook) |
| | `0x211 SET_VBH` | rewrite from profile (hook) |
| | `0x219 GENERATE_FRS_AND_UDS` | observe only — real per-device secrets |
| | `0x218 FBE_SET_SEED` | observe only — FBE class-key seed |
| | `0x204 MILESTONE_CALL`, `0x202`/`0x203` | observe only |
| SPSS → SPU | `ShareKeyMintInfo` | rewrite RoT/BootState/Vbh mirror (hook) |
| SCM → TZ | `TZ_BLOW_SW_FUSE 0x02000801` | drop — universal baseline already does |
| | `TZ_INFO_GET_SECURE_STATE 0x02000604` | observe only |
| | anti-rollback / version-set SmcIds | deferred audit (§7) |
| ABL internal code | DICE `Mode` (`AvbPopulateBccParams.c`, 2 sites) | deferred (§7) |

All other `IsUnlocked()`-gated divergences either feed a buffer the QSEE/SPSS
hooks already rewrite (`VerifiedBoot.c` device-state, `KeymasterClient.c` RoT
computation) or are deliberately left honest — notably `VerifiedBoot.c:1364`
`AllowVerificationError = IsUnlocked()`, which an unlocked ABL needs in order to
keep booting custom images.

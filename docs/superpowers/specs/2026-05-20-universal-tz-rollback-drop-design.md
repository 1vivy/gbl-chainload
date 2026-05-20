# Universal TZ rollback-bump drop — design

**Date:** 2026-05-20
**Scope:** Extend `UniversalBaseline.c` to drop the two TZ-side anti-rollback
SCM SIPs in addition to `TZ_BLOW_SW_FUSE_ID`. Universal hook (applies to every
`GBL_MODE`).

## 1. Problem

ABL fires one of two SCM SIPs near the end of verified boot to advance the
RPMB-backed anti-rollback floor for the current OS image:

| SmcId | Path | Payload | Effect |
|---|---|---|---|
| `0x0200011E` | `TZ_UPDATE_ROLLBACK_VERSION_ID` (legacy, KM MajorVersion ≤ 5) | `os_version`/`os_patchlevel` blob | Monotonic-max write to RPMB-stored floor. |
| `0x32000110` | `TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID` (KM MajorVersion > 5, slot-aware — what infiniti fires) | Same, but per-slot | Same, per-slot. |

Once TZ has committed a floor, attempting to boot an OS image with a lower
`os_version`/`os_patchlevel` causes the Keymaster TA to refuse key operations
on subsequent boots. The Settings / recovery UX on Qcom-stack Android funnels
that failure mode toward a userdata factory reset — which is the
`/data`-wipe scenario the project safety boundary exists to avoid.

Both SmcIds are already decoded and flagged `DROP-CANDIDATE(NOT-DROPPED)` in
`ScmHook.c:361-377`. This design promotes the scaffold to live drops.

## 2. Out of scope

- **KM 0x207 SET_VERSION** is left intact. It writes `os_version`/`os_patchlevel`
  into the Keymaster keyblob's `hw_enforced` set, not into TZ rollback storage.
  Mismatches there return `KM_ERROR_KEY_REQUIRES_UPGRADE` (-62) which Keystore
  handles silently via `upgradeKey()` — recoverable, no `/data` wipe risk.
  Dropping 0x207 would break every keystore-backed key for the boot session
  (apps, fingerprint, Strongbox) without adding rollback protection.
- **KM SET_BOOT_STATE (0x208) hidden-set fields** — already constant across
  boots per `[[infiniti_km_hidden_set_invariant]]`; unrelated channel.
- **Mode-2 profile rewrite path** — orthogonal. The drops happen before
  mode-specific overlays see the SmcId.

## 3. Design

### 3.1 `UniversalBaseline.{c,h}` — extend the drop list

Add two `#define`s mirroring the names in `ScmHook.c` and turn
`UniversalPolicy_ShouldDropScmSip` from a single-case check into a small
switch:

```c
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID        0x02000801U
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER    0x0200011EU
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB 0x32000110U

BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  switch (SmcId) {
    case SCM_SIP_TZ_BLOW_SW_FUSE_ID:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID) | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x"
                "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
                " | DROPPED (universal)\n",
                SmcId);
      return TRUE;
  }
  return FALSE;
}
```

Return value strategy mirrors the existing `TZ_BLOW_SW_FUSE` drop: set
`FakeStatus = EFI_SUCCESS` and let the caller short-circuit without forwarding
to TZ. `Results[]` is left untouched by the SCM hook wrapper (it never wrote
to `Results` in the drop branch — see `ScmHook.c:480-486`). For these SmcIds
ABL only inspects the EFI status; no out-parameter is required.

Update the header comment block in `UniversalBaseline.c` and the doc-comment
in `UniversalBaseline.h` to reflect the broader contract (now covers
soft-fuse blow **and** rollback bumps).

### 3.2 `ScmHook.c` — collapse the decoder annotation

The two rollback cases in `DecodeSipSmcId` currently log
`DROP-CANDIDATE(NOT-DROPPED)`. Update the log strings to
`DROPPED (universal)` to match the existing `TZ_BLOW_SW_FUSE` decoder note —
purely cosmetic; the decoder runs *after* the universal short-circuit so
these cases will not fire on the drop path, but mirroring the
`BLOW_SW_FUSE` precedent keeps the file self-consistent.

No change to the SmcId constants or the dispatcher.

### 3.3 Mode-2 design doc — close the deferred-audit row

In `docs/superpowers/specs/2026-05-17-mode-2-design.md` §7 + appendix table,
flip the row:

```
| anti-rollback / version-set SmcIds | deferred audit (§7) |
```
to
```
| anti-rollback / version-set SmcIds | drop universally — UniversalBaseline.c |
```

and replace the §7 paragraph with a one-line cross-reference to this design.

## 4. Risk

- **Re-flashability**: by design improved — never advance floor.
- **Stock-image fallback**: a user who flashes back to stock and unloads
  gbl-chainload will boot a stock ABL that fires the SmcIds normally. From
  that point the floor advances per stock policy. No worse than baseline.
- **KM keyblob upgradeKey()** flow: unaffected (0x207 untouched).
- **Attestation cert content**: unaffected (the os_version reported in
  attestation comes from KM's own state, populated via 0x207).
- **Logging**: one extra `GBL_INFO` line per boot (the rollback drop fires
  once per boot, possibly twice on A/B if both legacy + new SmcIds are
  attempted in sequence — unlikely, but harmless).

## 5. Verification

On-device test on infiniti (only device available, per memory):

1. Build with the change, stage + `oem boot-efi` per the project safety
   boundary.
2. Confirm via on-screen log that the `DROPPED (universal)` line appears for
   `0x32000110` (infiniti's path — KM MajorVersion=3.0.3 but the runtime
   uses the AB-aware SmcId per gbl_root reference).
3. Confirm boot completes to Android with the existing mode-1 (locked-look)
   and mode-2 (spoof) flows still passing.
4. Boot Android, exercise keystore (e.g., unlock with fingerprint, run an
   app that uses Strongbox-backed keys), confirm no regression.
5. Reboot, confirm same behavior is stable across multiple boots and
   `/data` survives.

No host-side unit test surface; the change is a SmcId table extension and
the existing decoder already exercises both paths in observation mode.

## 6. Decomposition

One small PR against `main`:

- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.{c,h}` — switch +
  two new SmcIds.
- `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` — flip the two log
  annotations from `DROP-CANDIDATE(NOT-DROPPED)` to `DROPPED (universal)`.
- `docs/superpowers/specs/2026-05-17-mode-2-design.md` — close the
  deferred-audit row.
- New doc: this file.

# patch7 (orange-screen) — enable for mode-2

Status: design approved 2026-05-20.

## 1. Goal & scope

In mode-2, gbl-chainload keeps ABL **honest** about the unlock state. An honest
unlocked ABL renders the orange-state warning screen and a 5-second boot-delay
gate on every boot. Patch7 (`ApplyOrangeScreen` in
`GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c`) already
implements the byte-level rewrite that silences this warning by turning the
CBZ guard at the warning block into an unconditional `B`. The patch has been
archived (`#ifdef GBL_PATCH7_ENABLED`) because mode-1 fakelocks the
verified-boot protocol view, so ABL never reaches the warning code path and
patch7 is dead at runtime.

This change unarchives patch7 and brings it into the OEM-scoped patch set as
an unconditional entry, so the mode-2 build pipeline applies it to the cached
ABL. It also widens the mode-2 ZIP's OEM detect to cover the full oplus
family (OnePlus / Oppo / Realme).

In scope:

- Unarchive `kOemOneplusPatches[]` so patch7 is registered unconditionally.
- Widen `zip/modes/mode-2-install.sh` OEM detect to accept `oneplus`,
  `oppo`, `oplus`, and `realme`.
- Update host tests to reflect patch7's live status.
- On-device verification on the infiniti test phone (mode-2).

Non-goals:

- No per-patch mode mask, no new `--mode` flag for `abl-patcher`, no new
  scope enum value. The existing scope / `--oem` mechanism is sufficient.
- No Realme/Oppo anchor verification. We have no Realme/Oppo build.prop or
  ABL fixtures in `logs/` or `tests/`. Patch7 returns `PATCH_MISS` cleanly
  if the anchor isn't found and is non-mandatory, so failure is safe.
- No mode-1 behavioral changes (patch7 in mode-1 has no observable effect
  under fakelock — see §3).
- No DICE-mode or other deferred mode-2 items from
  [`2026-05-17-mode-2-design.md`](2026-05-17-mode-2-design.md) §7.

## 2. Why "apply regardless of mode" is safe

Patch7 rewrites a single 4-byte instruction (the CBZ at
`AnchorOff + kPatch7RewriteDelta` = anchor + 0x18) to
`kPatch7BUnconditionalInsn` (`0x14000023`). Properties that make the patch
mode-agnostic:

- **Anchor uniqueness.** `kPatch7AnchorPattern` (the 4 CSEL/AND bytes
  immediately preceding the CBZ) is scanned with `ScanForBoundedSection` in
  the executable section only and is unique on the infiniti ABL. The
  anchor does not include the CBZ word itself, so re-applying the patch is
  idempotent (verified by `tests/patches/test_patch7.c`).
- **Non-mandatory.** `PATCH_MISS` on a non-matching ABL is a no-op; the
  patch engine logs the miss and continues.
- **Mode-1 inert.** Mode-1 fakelocks the verified-boot protocol view via
  `Mode1Policy_*`, so ABL's orange-state code path is never reached. The
  rewritten branch is dead code under mode-1.

Conclusion: there is no scenario where patch7 changes mode-1 behavior, and
gating it by mode would add complexity without benefit.

## 3. Changes

### 3.1 `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c`

- Remove the `#ifdef GBL_PATCH7_ENABLED` / `#else` / `#endif` structure.
- `kOemOneplusPatches[]` becomes a single-entry table with the patch7
  descriptor.
- `kOemOneplusPatchesCount` becomes `sizeof (kOemOneplusPatches) / sizeof
  (kOemOneplusPatches[0])` unconditionally.
- Update the file's leading doc comment: drop the "archived from active
  table" wording; describe patch7 as an oem-scope patch invoked via
  `--oem oneplus` (host) and the EFI runtime's automatic OEM aggregation.

No other source file in `GblChainloadPkg/` is touched. In particular:

- `Include/Library/PatchDesc.h` — unchanged. No `ModeMask` field, no new
  scope enum value.
- `Library/DynamicPatchLib/PatchTable.c::InitAggregate` — unchanged. It
  already aggregates `kOemOneplusPatches` unconditionally; that becomes a
  one-entry inclusion in mode-1 EFI runtime (inert) and mode-2 EFI runtime
  (live as the fallback path).
- `Library/DynamicPatchLib/PatchTable.c::EnsureInitScoped` — unchanged.
  Already keyed off the `oem` parameter; mode-1 install passes
  `GBL_OEM_NONE`, mode-2 install passes `GBL_OEM_ONEPLUS`.
- `Library/DynamicPatchLib/universal/universal.c`,
  `Library/DynamicPatchLib/mode_1/mode_1.c` — unchanged. Universal patches
  remain `SCOPE_UNIVERSAL`, included in both modes by both code paths.
  Mode-1 patches remain `SCOPE_MODE_1`, gated by `GBL_MODE >= 1` /
  `include_mode1`.

### 3.2 `zip/modes/mode-2-install.sh`

`detect_oem` widens the case branch from
`*oneplus*|*oppo*) OEM_ID=oneplus ;;` to:

```sh
*oneplus*|*oppo*|*oplus*|*realme*) OEM_ID=oneplus ;;
```

Rationale:

- `*oplus*` — the infiniti build.prop captures in `logs/` show
  `ro.product.bootimage.manufacturer=oplus` on a OnePlus device. Some
  newer oplus-family devices may set only this string.
- `*realme*` — Realme is part of the oplus family (shared ABL fork);
  user listed it explicitly. Anchor uniqueness on Realme ABLs is
  unverified but `PATCH_MISS` is the clean failure mode.

No other ZIP changes. The mode-1 install script is not touched (it does
not call `detect_oem`).

### 3.3 Tests

- `tests/patches/test_patch7.c` — currently calls `ApplyOrangeScreen`
  directly because patch7 was archived. Update to either:
  (a) keep the direct-call path AND assert table membership
  (`kOemOneplusPatchesCount >= 1` and an entry with `.Name ==
  "patch7-orange-screen"`), or
  (b) drive the patch through `DynamicPatchLib_EnsureInitScoped` + the
  patch engine.
  Recommendation: (a) — minimal churn, still verifies the new wiring.
  Keep the existing skip-guard on missing infiniti PE fixture.

- New `tests/host/0XX_patch7_via_oem.sh` (next free number) — confirms
  end-to-end host wiring. Two runs against the infiniti fixture:
  - `abl-patcher --in <pe> --oem oneplus --no-mode1` → patch7 applied
    (stderr shows `patch7-orange-screen` in the per-patch lines and
    `applied=` count includes it).
  - `abl-patcher --in <pe>` (default, no `--oem`) → patch7 not in the
    applied set.
  Skip-guard on missing fixture, consistent with `tests/host/060`,
  `062`, `083`.

- `tests/host/045_mode_taxonomy_lint.sh` — review and adjust if it asserts
  patch7's archived status; otherwise leave alone.

- Regression gate: `tests/host/060`, `062`, `083`, and the patch tests
  must stay green.

### 3.4 On-device verification

- Build mode-2 EFI locally with the patch7 change applied.
- Stage and boot via `fastboot stage <efi>` + `fastboot oem boot-efi`.
  **Do not** use `fastboot flash` for any partition (CLAUDE.md §Safety).
- Confirm: the orange-state warning screen does not render on boot.
- Confirm: mode-2 attestation suite (key attestation, Widevine, RKP,
  Strongbox, SOTER) continues to pass at the baseline established
  on infiniti 2026-05-18.
- Capture log directory under `logs/` with tag like
  `mode2-patch7-on_<sha>`.

## 4. Risks

- **Realme/Oppo anchor mismatch.** Unverified — we lack Realme/Oppo ABL
  fixtures. Effect: `PATCH_MISS`, orange-state warning continues to render
  on those devices. Documented as a known limitation; follow-up if real
  user reports it.
- **Mode-1 cached-ABL drift.** Not affected — mode-1 install does not
  pass `--oem`, so its cached ABL is byte-identical to today.
- **Mode-1 EFI-runtime Tier-2 dynamic patch.** Now aggregates patch7
  (since `InitAggregate` unconditionally includes the OEM table). On
  mode-1, the rewrite is harmless (dead code under fakelock), but it
  shows up in dynamic-patch logs. Acceptable; cleaner alternative would be
  gating `InitAggregate`'s OEM block on `GBL_MODE == 2`, which we
  consciously skipped per §2.

## 5. Implementation order

Single PR against `main`. Suggested commit order:

1. Unarchive `kOemOneplusPatches[]` (`oneplus_canoe.c`).
2. Update `tests/patches/test_patch7.c` for the new wiring; add the new
   host test under `tests/host/`.
3. Widen `zip/modes/mode-2-install.sh` OEM detect.
4. On-device verification + log capture; reference the log directory in
   the PR description.

# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 architecture in flight. See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the implementation plan series.

Today's working artifact: `dist/mode-1.efi` (Plan 1 deliverable) — protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly.

## Modes

- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view.
- **mode-2** *(Plan 2)* — TA-payload spoof at QSEE/SPSS boundaries; ABL stays honest; per-OTA typed-struct profile.
- **mode-3** *(Plan 3)* — universal baseline only; minimal experiment to gauge KM root-cert leaf survival.

## Build

```bash
./scripts/build.sh --mode 1               # production silent
./scripts/build.sh --mode 1 --auto --debug --verbose   # dev capture
```

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — hooks every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.

## Sibling

Old tree preserved read-only at `/home/vivy/gbl-chainload-dirty/` (tag `dirty/last-mode-fakelocked`).

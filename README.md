# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 architecture in flight. See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the implementation plan series.

Working artifacts: `dist/mode-0.efi` (pass-through observation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).

Mode-1 supports the "stock recovery + custom system" use case by default. If you flash a custom recovery, use `scripts/graft-vbmeta-from-stock.py` to graft stock vbmeta onto your custom image before flashing — see [`docs/re/recovery-normal-boot-fix-paths.md`](docs/re/recovery-normal-boot-fix-paths.md). A device-side companion module that automates this is Cleanup Phase 2 work.

## Modes

- **mode-0** — pass-through observation build. Patch engine + logfs only; no protocol hooks, no patch9. Useful for capturing logs against unmodified ABL verification behavior.
- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view.
- **mode-2** *(not yet implemented)* — TA-payload spoof at QSEE/SPSS boundaries; ABL stays honest; per-OTA typed-struct profile.
- **mode-3** *(not yet implemented)* — universal baseline only; minimal experiment to gauge KM root-cert leaf survival.

## Build

```bash
./scripts/build.sh --mode 0               # observation build (no hooks, no patch9)
./scripts/build.sh --mode 1               # fakelock production silent
./scripts/build.sh --mode 1 --auto --debug --verbose   # fakelock dev capture
```

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — hooks every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.

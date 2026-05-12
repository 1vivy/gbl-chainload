# tests/images — ABL fixture pool for the dynamic-patch harness

Tracked location for ABL fixtures consumed by `tests/patches/test_patch*` and
`tests/042_dynamic_patch_harness.sh`. (Top-level `images/` is gitignored —
device-specific stock OTA dumps stay local; the fixtures in this directory
are the curated test pool.)

## Naming convention

`<phone>-<codename>-<build-tail>-abl.img` for raw ABL FV wrappers, e.g.

- `op15-infiniti-201-abl.img` — OnePlus 15 (infiniti) build 16.0.7.201
- `op15-infiniti-703-abl.img` — OnePlus 15 (infiniti) build 16.0.5.703
- `op15t-fairlady-201-abl.img` — OnePlus 15T (fairlady)
- `xi17-pudding-44-abl.img` — Xiaomi 17 (pudding / K90 Pro Max)

Extracted PEs (post-unwrap) sit alongside as
`<phone>-<codename>-<build-tail>-LinuxLoader.efi` (`.efi` extension), produced
by the FV→PE extractor (`scripts/extract-pe-from-fv.sh` / `tools/fv-unwrap`,
WIP).

## Which test consumes what

| Test | File pattern consumed | Coverage |
|------|-----------------------|----------|
| `test_patch1` (patch1-efisp-recursion) | `*.efi`, `*.bin`, `*.img` | byte-scan; works on raw FV |
| `test_patch7` (orange-screen) | `LinuxLoader_infiniti.efi` only | offset-specific; needs the extracted infiniti PE |
| `test_patch6` (lock-state fastboot-gate) | `*.efi` (any OnePlus/Oppo libavb PE) | string-anchored gate-bypass; needs extracted PEs |
| `test_patch10` (libavb force-AVB-success) | `*.efi` (any Qcom libavb PE) | string-anchored function entry/exit rewrite; needs extracted PEs |
| `042` mandatory anchor-uniqueness | `*.efi` only | strict |
| `042` informational anchor-uniqueness | `*.bin`, `*.img` | non-fatal until extractor lands |

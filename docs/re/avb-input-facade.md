# AVB input façade plan for recovery/dtbo embedded vbmeta

Purpose: make fakelocked ABL/libavb see parseable stock-equivalent recovery/dtbo AVB metadata, so the normal locked/green boot path naturally emits coherent bootconfig, KeyMaster, Oplus, and per-partition vbmeta digest state.

## Why this exists

Captured userspace props show this target emits per-boot-partition AVB digest state:

```text
ro.boot.vbmeta.boot.digest
ro.boot.vbmeta.dtbo.digest
ro.boot.vbmeta.init_boot.digest
ro.boot.vbmeta.recovery.digest
ro.boot.vbmeta.vendor_boot.digest
ro.boot.vbmeta.digest
```

Therefore the preferred fix is not to patch every bootconfig/cmdline output. Instead, make the AVB inputs parse correctly and let stock ABL/Oplus code generate those outputs.

## Build-helper script

Use `scripts/extract-avb-embedded-vbmeta.py` to extract stock embedded AVB metadata from raw partition images:

```bash
./scripts/extract-avb-embedded-vbmeta.py \
  --partition recovery \
  --image stock_recovery_raw.img \
  --out build/avb-cache/recovery \
  --c-header build/avb-cache/recovery_avb_cached_blobs.h

./scripts/extract-avb-embedded-vbmeta.py \
  --partition dtbo \
  --image stock_dtbo_raw.img \
  --out build/avb-cache/dtbo \
  --c-header build/avb-cache/dtbo_avb_cached_blobs.h
```

Outputs per partition:

- `<partition>.avb_footer.bin` — the 64-byte AVB footer at `partition_size - 64`.
- `<partition>.embedded_vbmeta.bin` — the embedded vbmeta struct located by the footer.
- `<partition>.manifest.txt` — offsets, sizes, and SHA-256 hashes.
- optional C header containing byte arrays for development builds.

Inputs must be full raw partition images. Short Android images are not authoritative if they do not include bytes at the real partition footer offset.

## Intended runtime patch

The in-memory ABL patch should hook the binary equivalent of:

```text
AvbReadFromPartition(Partition, ReadOffset, NumBytes, Buffer, OutNumRead)
```

Source-side reference:

```text
edk2/QcomModulePkg/Library/avb/libavb/avb_ops.c:154
```

Normal-boot behavior:

```text
if partition is recovery/dtbo and read targets embedded AVB footer:
    return cached stock footer bytes

if partition is recovery/dtbo and read targets embedded vbmeta range from cached footer:
    return cached stock embedded vbmeta bytes

otherwise:
    call original partition read
```

This targets parser/input failures first:

```text
footer missing
AVBf magic missing
embedded vbmeta absent
OK_NOT_SIGNED due destroyed auth block
ERROR_INVALID_METADATA before SlotData is complete
```

Only after parser success should we add narrow digest-compare forgiveness for modified recovery/dtbo payload contents, if needed.

## Patch anchors

Source references for Ghidra/binary matching:

- `avb_ops.c:154` — `AvbReadFromPartition()`.
- `avb_slot_verify.c:699-728` — footer read and validation.
- `avb_slot_verify.c:748-793` — embedded vbmeta read and `avb_vbmeta_image_verify()`.
- `avb_slot_verify.c:497-503` — hash descriptor digest mismatch after parse succeeds.

Existing in-memory patch framework:

```text
GblChainloadPkg/Library/DynamicPatchLib/PatchEngine.c
GblChainloadPkg/Library/DynamicPatchLib/Trampoline.c
GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
```

## Open blockers for the actual binary patch

1. Ghidra-confirm binary RVAs/anchors for the target ABL build:
   - `AvbReadFromPartition` equivalent, or
   - footer-read callsites in `load_and_verify_vbmeta`.
2. Capture stock recovery/dtbo full raw partition images before modification.
3. Generate cached footer/vbmeta blobs.
4. Add `patch9-avb-input-facade` using the existing DynamicPatchLib trampoline/cave infrastructure.
5. Add optional `patch10-recovery-dtbo-hash-bypass` only if parse succeeds but digest comparison still blocks boot.

# AVB façade graft + libavb-status fastboot interface

**Date**: 2026-05-10
**Status**: **SUPERSEDED — 2026-05-12** (see top of [`docs/superpowers/plans/2026-05-10-avb-facade-graft-and-status.md`](../plans/2026-05-10-avb-facade-graft-and-status.md)). Graft's "transplant stock vbmeta" approach worked for system-boot only because patch10 hid the descriptor/content mismatch at the ABL layer; userspace `libfs_avb` (post AOSP `ec10d3c`) catches it independently on the recovery-boot path. The replacement is **synthesize** — building an unsigned vbmeta whose `expected_digest` is derived from the partition's actual content (`scripts/synthesize-vbmeta.py`, fastboot `oem synthesize-and-flash` in PR C). Body below preserved for historical context only.
**Working directory**: `/home/vivy/gbl-chainload` (v2 repo, `main` at `ee1b97f`).

## Context

Plan 2 (`v2.0.0-plan2-mode0-logfs-patch9v2`) shipped mode-1 + patch9 v2 (3-site). Device-validated on infiniti EU 16.0.5.703: ABL emits clean locked/green state. Custom recovery boots with that state.

But normal boot to Android system fails when custom recovery is flashed alongside LKM-patched init_boot. This spec restores that path with two related sub-projects:

- **Plan 3a — AVB façade graft**: transplant stock recovery's embedded vbmeta region into the user's custom recovery image so AOSP's first_stage_init AVB walk passes. Three tracks: host script + on-device fastboot oem + flashable ZIP.
- **Plan 3d — libavb-status fastboot interface**: `getvar` and `oem` surface for AVB-state diagnostics.

Both unconditionally included in the payload. Build-flag gating, size optimization, and DCE/`--gc-sections` analysis are explicitly deferred to a future plan (post Plan 3).

Mechanism explanations for AOSP-side AVB behavior are explicitly deferred to a parallel research session — not claimed in this spec.

## Section 1 — Behavior observations

What we *know* from device testing on infiniti EU 16.0.5.703 under mode-1 + patch9 v2:

1. **`vbmeta.img` is bytewise stock.** No Magisk/KSUN-style re-flash; no `--disable-verification` flag manipulation by the user.
2. **KSUN may patch stock `init_boot.img`** into a modified `init_boot.img` with a different digest. Empirically: this exhibits no problem with attestation or with AVB-side verification under mode-1. Mechanism not characterized here.
3. **A custom recovery that lacks an AVB footer cannot finish a normal boot to system, but can boot to recovery.** Mode-1's recoverable-continue path lets the chain failure pass through ABL and into recovery. The normal boot path falls into recovery instead of reaching Android system. Mechanism not characterized here.
4. **Upstream `gbl_root_canoe` author fixes this by transplanting stock recovery's embedded vbmeta footer into the user's custom recovery image** before flashing. Empirically validated on the same platform. This spec adopts the same approach.

What's in scope: the recovery-partition graft (item 4), and the status-inspection surface that supports both verification of the graft and general operator diagnostics.

## Section 2 — Plan 3a tooling

All three tracks share one core operation: copy the donor's embedded vbmeta region into the user's custom partition image, where the region is identified by the donor's `AvbFooter` at the last 64 bytes (magic `"AVBf"`).

```
custom recovery (input):  [ user kernel + ramdisk + 0x10000 padding | custom_garbage_footer? ]
stock recovery (donor):   [ stock kernel + ramdisk + 0x00000 padding | stock_avb_footer + stock_vbmeta_header + stock_aux_data ]
                                                                       ↓
grafted recovery (out):   [ user kernel + ramdisk + 0x10000 padding | stock_avb_footer + stock_vbmeta_header + stock_aux_data ]
```

The byte-region boundary is identified by parsing the donor's `AvbFooter`. The graft replaces the bytes `[partition_size - footer.vbmeta_offset - footer.vbmeta_size, partition_size]` in the custom image with the donor's equivalent region.

In-scope partitions: `recovery` (and `recovery_a`/`recovery_b`), `dtbo` (and any A/B variants). Both use chain_partition_descriptor per `docs/re/avb-descriptor-findings-eu-16.0.5.703.md`.

### Track 1 — Host Python script `scripts/graft-vbmeta.py`

Pure stdlib Python (`struct` + file I/O). No on-device dependency. ~150 lines.

```bash
./scripts/graft-vbmeta.py \
  --partition recovery \
  --stock /path/to/stock_recovery.img \
  --custom /path/to/twrp.img \
  --out /path/to/grafted.img
```

Validates donor footer magic (`"AVBf"`), parses footer, extracts vbmeta region, validates header magic (`"AVB0"`), splices into custom image at the matching byte-offset-from-end, writes output. Refuses if any magic check fails.

User then `fastboot flash recovery_a grafted.img` (and `_b` if A/B).

### Track 2 — On-device `fastboot oem graft-and-flash <partition>`

Workflow:

```bash
fastboot stage path/to/twrp.img
fastboot oem graft-and-flash recovery
# (dry-run prints expected operation; no write yet)
fastboot oem graft-and-flash recovery commit
# (atomic write to active slot's recovery partition)
```

Implementation behavior:

1. **Source validation.** Read the active slot's partition (e.g. `recovery_a` if `slot=a`) via BlockIo. Parse the `AvbFooter` from the last 64 bytes (uses `AvbParseLib`, §3). Validate `vbmeta_offset` and `vbmeta_size` are within partition. Refuse with explicit diagnostic if invalid (covers cases like "user already grafted" or "partition was custom-flashed without vbmeta").
2. **Staged-image validation.** Confirm the staged buffer is at least the partition size. Verify the buffer has plausible image structure for the target partition: recovery → Android boot image magic `"ANDROID!"` at offset 0; dtbo → fdt magic `0xD00DFEED` at offset 0 (or DTBO-container header for multi-entry dtbo).
3. **Header-region validation.** Parse `AvbVBMetaImageHeader` at the donor's vbmeta offset. Require magic `"AVB0"`. Verify algorithm field is one of the standard AVB algorithm types. Verify `auth_data_block_size + aux_data_block_size + sizeof(header) == vbmeta_size`. Refuse if any check fails.
4. **Dry-run by default.** First invocation prints donor footer fields, donor header fields, target partition info, staged image identity, and the bytes-to-be-written summary. No write. User issues the `commit` argv to actually write.
5. **Atomic write.** Single `BlockIo->WriteBlocks()` call against the partition handle. Atomic at the BlockIo layer.

Track 2 is always-built into the payload; no build-flag gating.

### Track 3 — Flashable ZIP

A user-assembled flashable ZIP runs from inside an existing custom recovery (TWRP/OFOX/etc.). Layout:

```
gbl-graft-installer.zip
├── META-INF/com/google/android/
│   ├── update-binary           # AnyKernel3-style entry; bash script
│   └── updater-script
├── recovery_custom.img         # user-provided
├── dtbo_custom.img             # user-provided (optional)
└── tools/
    ├── graft-vbmeta            # static-linked aarch64 binary
    └── busybox
```

`update-binary` script:

1. Read on-disk active slot's stock partition into `/tmp/stock_<part>.img` via `dd`.
2. Run `tools/graft-vbmeta --partition <part> --stock /tmp/stock_<part>.img --custom <part>_custom.img --out /tmp/grafted_<part>.img`.
3. `dd` the grafted image into `/dev/block/by-name/<part>_<slot>`.
4. Repeat for dtbo if present.

The ZIP template is shipped in this repo; user assembles their final ZIP by adding their custom images. We don't ship anyone's images.

### OTA persistence strategy

No persistent cache for stock vbmeta. The on-disk stock partition (post-OTA, pre-customize) IS the freshest source. Workflow after each OTA:

- Device reboots into new stock slot. The new slot's recovery partition contains stock recovery (just-installed by OTA).
- User runs Track 1 (host) or Track 3 (ZIP) to graft custom recovery using the on-disk stock as donor.
- Optionally Track 2 (fastboot oem) for first-time setup.

We avoid: caching stock vbmeta in our payload's FV (stale across OTAs), caching in logfs (stale across OTAs), parsing the OTA package format (varies by OEM; brittle).

## Section 3 — `AvbParseLib`

A minimal AVB structure parser, our own — no link dependency on libavb. Used by Plan 3a Track 2 (graft validation) and Plan 3d (status reporting).

**Files:**
- `GblChainloadPkg/Include/Library/AvbParseLib.h`
- `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`
- `GblChainloadPkg/Library/AvbParseLib/AvbParseLib.inf`

**Surface:**

```c
typedef struct {
  UINT64  OriginalImageSize;
  UINT64  VbmetaOffset;        /* bytes from start of partition */
  UINT64  VbmetaSize;
  UINT32  FooterMajorVersion;
  UINT32  FooterMinorVersion;
} GBL_AVB_FOOTER;

typedef struct {
  UINT32  AvbMajorVersion;
  UINT32  AvbMinorVersion;
  UINT64  AuthenticationDataBlockSize;
  UINT64  AuxiliaryDataBlockSize;
  UINT32  AlgorithmType;       /* AVB_ALGORITHM_TYPE_* */
  UINT64  HashOffset;
  UINT64  HashSize;
  UINT64  SignatureOffset;
  UINT64  SignatureSize;
  UINT64  PublicKeyOffset;
  UINT64  PublicKeySize;
  UINT64  PublicKeyMetadataOffset;
  UINT64  PublicKeyMetadataSize;
  UINT64  DescriptorsOffset;
  UINT64  DescriptorsSize;
  UINT64  RollbackIndex;
  UINT32  Flags;
  UINT32  RollbackIndexLocation;
  CHAR8   ReleaseString[48];
} GBL_AVB_VBMETA_HEADER;

typedef enum {
  GblAvbDescPropertyTag        = 0,
  GblAvbDescHashtreeTag        = 1,
  GblAvbDescHashTag            = 2,
  GblAvbDescKernelCmdlineTag   = 3,
  GblAvbDescChainPartitionTag  = 4,
} GBL_AVB_DESCRIPTOR_TAG;

EFI_STATUS EFIAPI AvbParse_Footer       (IN CONST UINT8 *Partition, IN UINT64 PartitionSize, OUT GBL_AVB_FOOTER *FooterOut);
EFI_STATUS EFIAPI AvbParse_VbmetaHeader (IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize, OUT GBL_AVB_VBMETA_HEADER *HeaderOut);
EFI_STATUS EFIAPI AvbParse_NextDescriptor (IN CONST UINT8 *AuxBlock, IN UINT64 AuxSize,
                                            IN OUT UINT64 *Cursor, OUT GBL_AVB_DESCRIPTOR_TAG *TagOut,
                                            OUT CONST UINT8 **DescriptorOut, OUT UINT64 *DescriptorLenOut);
EFI_STATUS EFIAPI AvbParse_HashDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen,
                                            OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
                                            OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut);
EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen,
                                                      OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
                                                      OUT CONST UINT8 **PublicKeyOut, OUT UINT32 *PublicKeyLenOut);
```

Implementation: ~400 lines C99, big-endian-aware (AVB structures stored big-endian on disk). Field layouts derived from `external/avb/libavb/avb_footer.h` and `avb_vbmeta_image.h` in the local AOSP checkout (reference, not link target).

Host-testable: `tests/avb/test_avbparse.c` constructs synthetic AVB structures (footer + header + descriptors) and verifies the parser returns expected fields. TDD pattern matching ScanLib in plan 1.

Always built and always linked (no build flag).

## Section 4 — Plan 3d libavb-status fastboot interface

Two interaction shapes, both backed by `AvbParseLib` and SHA256 (use whatever EDK-II provides; details deferred to plan-writing).

### `fastboot getvar` (single value, scriptable)

```
fastboot getvar vbmeta:digest                  → 64 hex chars (SHA256 of vbmeta partition on active slot)
fastboot getvar vbmeta:slot                    → "a" or "b"
fastboot getvar vbmeta:<part>:descriptor-type  → "hash" | "chain" | "hashtree" | "none"
fastboot getvar vbmeta:<part>:expected         → 64 hex chars (descriptor's stored hash) or "n/a"
fastboot getvar vbmeta:<part>:computed         → 64 hex chars (SHA256 over partition content) or "n/a"
fastboot getvar vbmeta:<part>:status           → "ok" | "mismatch" | "unsigned" | "n/a"
```

`<part>` is selected from a per-OEM enum compiled into our payload. `none` returned for unknown.

`status` interpretation:
- `ok` — descriptor type is hash and computed equals expected, OR descriptor type is chain and the chained partition's signature verifies.
- `mismatch` — descriptor type is hash and computed differs from expected.
- `unsigned` — descriptor type is chain and the chained partition has no AVB footer or wrong magic.
- `n/a` — partition has no descriptor in vbmeta (or partition not enumerated).

### `fastboot oem vbmeta-status` (multi-line report)

```
$ fastboot oem vbmeta-status
INFO partition       descriptor    expected                  computed                  status
INFO recovery_a      chain         8d897f624...              <chain_pk_hash>           ok
INFO dtbo            chain         8d897f624...              <chain_pk_hash>           ok
INFO init_boot_a     hash          a1b2c3d4e...              a1b2c3d4e...              ok
INFO vendor_boot_a   hash          5f4e3d2c1...              5f4e3d2c1...              ok
INFO ...
INFO vbmeta digest   sha256        <vbmeta_a digest>
INFO vbmeta flags    raw           <hex>
```

Useful for: confirming a graft worked (post-graft `recovery_a` should show `chain ok`), diagnosing system-boot failures (which partition's hash is wrong), verifying user's vbmeta state.

Always built and always linked (no build flag).

## Section 5 — Stop-lines

- No persistent vbmeta cache in our payload's FV. On-disk stock IS the donor source, refreshed naturally by OTAs.
- No `--disable-verification` / `--disable-verity` flag manipulation tooling. That's user's concern, not ours.
- Track 2 always requires explicit `commit` argv to write — never write on first invocation.
- No full libavb link. `AvbParseLib` is our own, focused on the structures we actually use.

## Section 6 — Out of scope

- Mode-2 KM-payload spoof (Play Integrity / SafetyNet). Separate plan.
- ABL caching / build-time embed. Separate plan.
- Hash-descriptor partition graft tooling. Empirically not needed; out of scope.
- Build flag gating, size optimization, `--gc-sections` audit. Deferred to a future plan post Plan 3.
- Mechanism explanations for AOSP-side AVB behavior (why hash-descriptor partitions tolerate LKM patches, why chain-descriptor failure forces recovery mode, what cmdline tokens propagate where). Parallel research session, separate doc, after empirical verification — not claimed here.

## Section 7 — Implementation outline (for writing-plans phase)

1. **AvbParseLib** with TDD. Synthetic AVB data fixtures. ~10 host-side test cases.
2. **Track 1** host script `scripts/graft-vbmeta.py` — graft logic in Python. Round-trip test against infiniti EU 16.0.5.703 stock recovery.
3. **Track 2** on-device `fastboot oem graft-and-flash` in edk2 fork's FastbootCmds, wiring AvbParseLib. Dry-run + commit semantics.
4. **Plan 3d** getvar + oem vbmeta-status handlers in FastbootCmds. SHA256 sourced from whatever's lightest in EDK-II.
5. **Track 3** flashable ZIP template — `update-binary` script, ports the Python graft logic to a small static-linked aarch64 binary suitable for recovery's busybox env. ZIP source layout in `zip/avb-graft-installer/`.
6. **Validation** — end-to-end on infiniti EU 16.0.5.703: graft custom recovery via Track 1 → flash → mode-1 + grafted recovery → boot system. Confirm `oem vbmeta-status` shows `recovery_<slot>: chain ok` post-graft.

End-state checklist:

- [ ] `scripts/graft-vbmeta.py` produces grafted output that passes a host-side AvbParseLib parse.
- [ ] `fastboot oem graft-and-flash recovery` dry-run prints sane diagnostics; `commit` writes successfully.
- [ ] Post-write `getvar vbmeta:recovery:status` returns `ok`.
- [ ] `fastboot oem vbmeta-status` returns multi-line report covering all enumerated partitions.
- [ ] After grafting custom recovery + reboot to system: device boots Android with custom recovery still flashed.
- [ ] Track 3 flashable ZIP roundtrips (install in custom recovery, grafts, flashes, reboot to system works).

## Section 8 — Open items deferred to writing-plans

- Specific edk2-side INF/DSC paths for the new fastboot commands.
- SHA256 source choice: BaseCryptLib (heavy) vs standalone (light). Decided in plan-writing.
- Static-linked binary toolchain for Track 3 (Go, C, etc.). Decided in plan-writing.
- `update-binary` shell-dialect compatibility (TWRP / OFOX / LineageOS recovery — busybox-portable).
- AvbParseLib test fixtures — synthetic + at least one real partition extracted from infiniti EU 16.0.5.703 stock recovery.

---

Mechanism explanations and EFISP-size analysis go to a parallel research session, separate from this spec.

# Recovery Normal-Boot Fix Paths

**Status (2026-05-12):** Confirmed. Custom-recovery + normal-boot under mode-1 fails in AOSP `first_stage_init` (libfs_avb), which re-walks the vbmeta descriptor tree from disk after our shim has already lied to ABL's KM. The fix has to re-shape the on-disk recovery image *before* first-stage init reads it. Two intended paths, **both Phase-2 work**: a host-side `scripts/graft-vbmeta-from-stock.py` and a device-side recovery-graft companion module. The bootloader shim does not carry the fix because the read path it would need to intercept lives in userspace libfs_avb, outside our chainload domain. The graft technique was validated on infiniti during the abandoned `feature/synthesize-fastboot-cmd` exploration (commit `b26686e` on origin, retained as orphan history until Phase 2 rebuilds the tooling).

**Original investigation date:** 2026-05-10.
**Scenario:** gbl-chainload mode-1 + patch9 v2 boots custom recovery but fails normal-boot into Android system when custom recovery is flashed alongside LKM-patched init_boot.

---

## Executive Summary

When ABL successfully fakes vbmeta verification (KM 0x208 green state, real OEM pubkey), the OS-side AVB validation in `first_stage_init` proceeds to call `AvbHandle::Open()` (libfs_avb), which re-walks the entire chain-partition descriptor tree from disk. **If the recovery partition's on-disk HASH descriptor (embedded in vbmeta.img) doesn't match the recovery partition's actual content, the chain-walk fails at the recovery validation step**, even though recovery boots successfully in recovery-only mode (where ABL's libavb recoverable-continue semantics apply).

The failure is **deterministic and silent**: init doesn't panic or hang—it returns a failed `AvbSlotVerifyResult`, and the device falls through to recovery mode or fastbootd. This explains why "custom recovery + normal boot" reaches recovery UI: init's error handling defaults to recovery fallback.

---

## Early-AVB Call Chain with File:Line References

### 1. **First-Stage Init Entry Point** (`first_stage_init.cpp`)
- **File:** `/home/vivy/android/fox_14.1/system/core/init/first_stage_init.cpp`
- **Init:** Entry into first-stage mount via `FirstStageMount::Create()` (line 227).
- **Key:** Sets up `FirstStageMountVBootV2` class to orchestrate AVB verification and dm-verity setup.

### 2. **First-Stage Mount Initialization** (`first_stage_mount.cpp`)
- **File:** `/home/vivy/android/fox_14.1/system/core/init/first_stage_mount.cpp`

| Function | Line | Purpose |
|----------|------|---------|
| `FirstStageMountVBootV2::DoCreateDevices()` | 241 | Initializes block devices, calls `InitAvbHandle()` |
| `FirstStageMountVBootV2::InitAvbHandle()` | 809 | Calls `AvbHandle::Open()` to verify vbmeta chain |
| `FirstStageMountVBootV2::MountPartitions()` | 530 | Mounts /system, /vendor, /product; calls `SetUpDmVerity()` per partition |
| `FirstStageMountVBootV2::SetUpDmVerity()` | 744 | For each fstab entry with avb flag, calls `AvbHandle::SetUpAvbHashtree()` |

### 3. **AVB Handle Open (userspace libavb bridge)** (`fs_avb.cpp`)
- **File:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/fs_avb.cpp`

| Function | Line | Purpose |
|----------|------|---------|
| `AvbHandle::Open()` | 453 | Main OS-side AVB entry point; calls `FsManagerAvbOps::AvbSlotVerify()` |
| `AvbHandle::Open()` [digest check] | 503–510 | **Verifies cmdline `androidboot.vbmeta.{hash_alg,size,digest}` against loaded vbmeta images** |
| `AvbHandle::SetUpAvbHashtree()` | 548 | Calls `LoadAvbHashtreeToEnableVerity()` for dm-verity setup |

**Critical:** Lines 503–510 in `fs_avb.cpp::AvbHandle::Open()`:
```cpp
std::unique_ptr<AvbVerifier> avb_verifier = AvbVerifier::Create();
if (!avb_verifier || !avb_verifier->VerifyVbmetaImages(avb_handle->vbmeta_images_)) {
    LERROR << "Failed to verify vbmeta digest";
    if (!allow_verification_error) {
        LERROR << "vbmeta digest error isn't allowed ";
        return nullptr;  // ← RETURNS NULL, INIT FAILS
    }
}
```

### 4. **Userspace AVB Slot Verify (libavb C library)** (`avb_ops.cpp`)
- **File:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/avb_ops.cpp`

| Line | Purpose |
|------|---------|
| 122–143 | `FsManagerAvbOps` constructor; initializes AvbOps struct with **no-op rollback_index and public_key checks** (lines 57–79) |
| 75 | Comment: *"androidboot.vbmeta.{hash_alg, size, digest} against the digest of all vbmeta images after invoking avb_slot_verify()"* |

**Key:** Userspace **does not re-check rollback indices or re-validate public keys** (ABL already did those). Instead, it:
1. Calls `avb_slot_verify()` (from libavb) to **load and validate all partitions per their hash/hashtree descriptors**.
2. Then **re-verifies the vbmeta digest against cmdline values** set by ABL.

### 5. **Slot Verify & Chain-Partition Walk** (`avb_slot_verify.c`)
- **File:** `/home/vivy/android/fox_14.1/external/avb/libavb/avb_slot_verify.c`

| Line | Descriptor Type | Purpose |
|------|-----------------|---------|
| 987–1001 | `AVB_DESCRIPTOR_TAG_HASH` | Load partition, compute hash, **compare against descriptor's digest** (line 441: `avb_safe_memcmp()`) |
| 1003–1063 | `AVB_DESCRIPTOR_TAG_CHAIN_PARTITION` | Recursively load vbmeta from chained partition; **re-verify its signature and descriptors** |
| 1140+ | `AVB_DESCRIPTOR_TAG_HASHTREE` | Load hashtree descriptor (used for dm-verity kernel setup, not re-verified in avb_slot_verify) |

**Critical hash-matching code** (lines 300–446):
```c
// Line 300: Load and validate hash descriptor from vbmeta
avb_hash_descriptor_validate_and_byteswap(..., &hash_desc);

// Lines 381–413: Hash the on-disk partition data
if (avb_strcmp((const char*)hash_desc.hash_algorithm, "sha256") == 0) {
    avb_sha256_init(&sha256_ctx);
    avb_sha256_update(&sha256_ctx, desc_salt, hash_desc.salt_len);
    avb_sha256_update(&sha256_ctx, image_buf, image_size_to_hash);
    digest = avb_sha256_final(&sha256_ctx);
}

// Lines 430–433: Compare computed hash against descriptor's digest
expected_digest = desc_digest;  // From vbmeta descriptor

// Line 441: MISMATCH → ERROR_VERIFICATION returned
if (avb_safe_memcmp(digest, expected_digest, digest_len) != 0) {
    ret = AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION;  // ← FAILURE PATH
    goto out;
}
```

---

## Cmdline Digest Handling (androidboot.vbmeta.* Values)

### Where Cmdline Digests Are Read

**File:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/fs_avb.cpp`

| Line | Field | Source |
|------|-------|--------|
| 114 | `vbmeta.size` | `fs_mgr_get_boot_config("vbmeta.size", &value)` |
| 123 | `vbmeta.hash_alg` | `fs_mgr_get_boot_config("vbmeta.hash_alg", &hash_alg)` |
| 137 | `vbmeta.digest` | `fs_mgr_get_boot_config("vbmeta.digest", &digest)` |

These values are **set by ABL** during bootloader-stage AVB verification.

### How Cmdline Digests Are Used

**File:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/fs_avb.cpp` → `AvbVerifier::VerifyVbmetaImages()` (lines 152–181)

```cpp
bool AvbVerifier::VerifyVbmetaImages(const std::vector<VBMetaData>& vbmeta_images) {
    // Lines 161–167: Compute hash of all vbmeta images loaded by userspace
    if (hash_alg_ == HashAlgorithm::kSHA256) {
        std::tie(total_size, digest_matched) =
                VerifyVbmetaDigest<SHA256Hasher>(vbmeta_images, digest_);
    }
    
    // Line 169–172: Verify size matches
    if (total_size != vbmeta_size_) {
        LERROR << "total vbmeta size mismatch: " << total_size << " (expected: " << vbmeta_size_;
        return false;
    }
    
    // Line 175–177: Verify digest matches
    if (!digest_matched) {
        LERROR << "vbmeta digest mismatch";
        return false;
    }
}
```

**What this verifies:**
- The **total vbmeta.img + all chained vbmeta images** (e.g., vbmeta_system, vbmeta_vendor) hash to the cmdline digest.
- **Does NOT re-verify individual partition hashes** (recovery, boot, system, etc.); those are checked during `avb_slot_verify()`.

---

## Recovery Partition Hash Verification Failure Mode

### The Problem

When a **custom recovery is flashed**, its on-disk content changes. However:
1. **ABL's vbmeta.img (stock)** contains a HASH descriptor for recovery with the **stock recovery's digest**.
2. **Custom recovery's actual digest differs** from what's in the descriptor.
3. **OS-side `avb_slot_verify()` will fail** when it processes the recovery HASH descriptor (line 441 of avb_slot_verify.c).

### Chain-Partition Descriptor Tree

vbmeta.img typically has:
```
vbmeta.img (main, signed by OEM key)
├─ HASH descriptor: recovery (digest = sha256(stock_recovery))
├─ HASHTREE descriptor: system
├─ CHAIN_PARTITION descriptor: vbmeta_system
│  └─ vbmeta_system.img (chained, signed by different key)
│     ├─ HASH descriptors: init_boot, dtbo, etc.
│     └─ HASHTREE descriptors: system, vendor, product
└─ HASH descriptor: boot
```

When OS-side `avb_slot_verify()` walks this tree:
1. Loads vbmeta.img from disk.
2. Walks HASH descriptors → **recovers recovery** (line 987–1001).
3. **If custom recovery is flashed, hash mismatch at line 441** → returns `AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION`.
4. Because this is not a fatal error (line 59: `result_should_continue()` returns true), the walk continues.
5. **However, init interprets any verification error as a boot failure** → falls back to recovery or fastbootd.

### Why "Boot Into Custom Recovery" Works But "Normal Boot" Fails

- **In recovery mode (ABL's libavb recoverable-continue):** ABL skips detailed recovery HASH validation and proceeds with recovery-only mode.
- **In normal boot (OS-side userspace AVB):** `first_stage_init` calls `AvbHandle::Open()`, which **must fully validate all descriptors**, including recovery. Custom recovery's mismatch causes `AvbSlotVerifyResult` to be non-OK → init fallback to recovery mode.

**File evidence:**
- `first_stage_mount.cpp` line 812: `avb_handle_ = AvbHandle::Open();`
- `fs_avb.cpp` line 488: `if (!allow_verification_error) { LERROR << "ERROR_VERIFICATION / PUBLIC_KEY_REJECTED isn't allowed "; return nullptr; }`

If `allow_verification_error=false` (locked device), `AvbHandle::Open()` returns `nullptr` → `InitAvbHandle()` returns false → mount fails → init falls back.

---

## Hypothesis Validation: vbmeta_system Chain Descriptor

The vbmeta_system partition (if used) contains its own HASH/HASHTREE descriptors for init_boot, boot, etc. If recovery's HASH descriptor is in the **main vbmeta.img** (not in vbmeta_system), the failure path is:

1. ABL's fakelock emits: `androidboot.vbmeta.digest=<stock_vbmeta_hash>`
2. OS reads and fakes the cmdline digest check (via our gbl-chainload patch9).
3. `avb_slot_verify()` still loads recovery partition and validates it against descriptor.
4. Custom recovery's actual hash ≠ descriptor's hash → ERROR_VERIFICATION.
5. `AvbHandle::Open()` returns with status `kVerificationError` or `nullptr`.
6. `first_stage_mount.cpp::SetUpDmVerity()` fails → mount path fails → recovery fallback.

**This matches observed behavior: device boots into custom recovery UI.**

---

## Intended Fix Paths (Phase 2)

Both fix paths below are Phase-2 work. Neither ships in this PR; neither lives on `main` today. The "Recommended Mitigations" section below is preserved for historical context — those options were considered as alternatives.

### Host path (Phase 2)

A `scripts/graft-vbmeta-from-stock.py` of the form:

```bash
scripts/graft-vbmeta-from-stock.py \
  --partition recovery \
  --in <custom>.img \
  --stock <stock-recovery>.img \
  --out <patched>.img
```

Writes stock recovery vbmeta at `round_up(custom_image_size, 4 KiB)`. With `patch10` in the boot path, ABL emits `verify_result_local=OK` for recovery and the slot-level recoverable error is caught by `patch10` → final OK. Technique validated on infiniti during the abandoned `feature/synthesize-fastboot-cmd` exploration (commit `b26686e`, retained on origin as orphan history). Script TBD under Cleanup Phase 2.

### Device path (Phase 2)

On-device companion module that performs the same graft against the on-device stock vbmeta, automatically, post-recovery-flash. Lives next to the OTA-cached-ABL module. Spec TBD.

---

## Recommended Mitigations (Ranked by Likelihood)

### (a) **AVB Partition-Read Facade (MOST LIKELY TO WORK)**

> **Why not pursued in shim (2026-05-12):** the read path lives in userspace libfs_avb, outside our chainload domain. Effectively superseded by the disk-side graft (Phase 2); the facade idea remains noted here as a design alternative considered.

**Approach:** Intercept libavb's `read_from_partition()` calls at the gbl-chainload entry point (BL31/TZ level) or via a libavb_ops override in first_stage_init.

**Implementation:**
- When recovery partition is read by `avb_slot_verify()`, return the **stock recovery's content** (or a pre-computed digest) instead of the custom recovery.
- Descriptor validation passes → chain walk succeeds → normal boot proceeds.
- Recovery partition is still custom (mounted by mount_all later in init).

**Likelihood:** HIGH
- Non-destructive: Only affects AVB validation path.
- Preserves all functionality (custom recovery still boots in recovery mode).
- Matches the "fakelock" pattern already in patch9.

**Effort:** Medium
- Requires override of `FsManagerAvbOps::ReadFromPartition()` in libfs_avb.
- Or patch first_stage_init to detect custom recovery and suppress descriptor validation.

---

### (b) **Mode-2 TA-Payload Spoof (KM-SIDE)**

**Approach:** Have TZ/TA report attestation properties that claim recovery partition is stock.

**Likelihood:** LOW
- Does not address actual descriptor mismatch in userspace AVB walk.
- Attestation props are separate from AVB descriptor validation.
- Would require reverse-engineering OEM's attestation schema.

---

### (c) **Cmdline Rewrite (ABL-Side Recovery Digest)**

**Approach:** Patch ABL to compute and emit the **custom recovery's digest** in the cmdline before jumping to kernel:
```
androidboot.vbmeta.recovery.digest=<custom_recovery_sha256>
```

**Likelihood:** LOW-MEDIUM
- Risky: Would require storing custom recovery's hash at flash time (BL staging).
- Also requires patching first_stage_init to read per-partition digests.
- Adds complexity to build system.

---

### (d) **Userspace Prop Override (Init Payload)**

**Approach:** Have a chained init payload run before `first_stage_init` to:
- Suppress AVB verification for recovery partition.
- Patch fstab entries to mark recovery as no_fail.

**Likelihood:** LOW
- Fragile: Relies on init execution order.
- Does not solve underlying descriptor mismatch.
- Recovery partition may still cause dm-verity errors.

---

## Root Cause Summary

| Component | Issue | Evidence |
|-----------|-------|----------|
| **ABL (locked/faked)** | Emits clean vbmeta digest via cmdline | KM 0x208, cmdline `androidboot.vbmeta.digest=<stock>` |
| **OS-side init** | Re-walks vbmeta descriptor tree from disk | `fs_avb.cpp:AvbHandle::Open() → avb_slot_verify()` |
| **Custom recovery** | On-disk content ≠ descriptor's hash | avb_slot_verify.c:441 hash mismatch |
| **Fallback behavior** | ERROR_VERIFICATION → recovery mode | first_stage_mount.cpp:812, fs_avb.cpp:488 |

---

> **Historical — resolved 2026-05-12.** The action items below were written before the disk-side graft was validated. They are preserved for context. Live next work is tracked under Cleanup Phase 2 (separate spec), not against this doc.

## Action Items for gbl-chainload

1. **Immediate (Confirm Hypothesis):**
   - Capture `/proc/bootloader_log` or dmesg during failed normal boot to see if recovery hash mismatch appears.
   - Search for "Hash of data does not match" (avb_slot_verify.c:442) in logs.

2. **Short-term (Proof of Concept):**
   - Implement partition-read facade (option a): Override `ReadFromPartition()` to return stock recovery when recovery partition is requested.
   - Test with custom recovery + LKM init_boot.

3. **Medium-term (Production):**
   - If PoC succeeds, integrate into patch9 v3 or mode-2 TA payload.
   - Document recovery partition handling in mode-1 specification.

---

## References

- **First-stage init:** `/home/vivy/android/fox_14.1/system/core/init/first_stage_mount.cpp:809–821`
- **Userspace AVB:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/fs_avb.cpp:453–535`
- **Hash validation:** `/home/vivy/android/fox_14.1/external/avb/libavb/avb_slot_verify.c:276–449`
- **Cmdline digest:** `/home/vivy/android/fox_14.1/system/core/fs_mgr/libfs_avb/fs_avb.cpp:106–150`

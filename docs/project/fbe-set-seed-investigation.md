# FBE_SET_SEED (QSEECOM/KeyMaster cmd 0x218) — investigation

Distilled findings on whether `KEYMASTER_FBE_SET_SEED` (cmd-id `0x218`,
`= KEYMASTER_UTILS_CMD_ID + 24`) is implicated in the FBE-wrap failure
seen when gbl-chainload spoofs KeyMaster verified-boot state.

## Question

When the bootloader fakes KM boot-state (mode-1 fakelock; planned mode-2),
the device's File-Based-Encryption keys can no longer be wrapped/unwrapped,
forcing a `/data` format. Is `FBE_SET_SEED` itself the trigger, or is the
breakage a downstream effect of the Root-of-Trust / boot-state change?

## What 0x218 does

`FBE_SET_SEED` is the ABL → TrustZone (QSEE KeyMaster TA) call that hands
the **FBE class-key derivation seed** into the secure world early in boot,
before HLOS / `vold` / `init` start. The seed is one input to the QSEE-side
key hierarchy that protects FBE storage keys.

Qualcomm FBE key hierarchy (from AOSP + Qualcomm FBE docs):

- FBE **class keys** (per Android user, per CE/DE class) are the raw
  `fscrypt` master keys the kernel uses. With hardware-wrapped keys these
  are generated and kept *inside* KeyMaster — plaintext class keys are
  never present in HLOS.
- On disk, the class-key blob is **wrapped twice**: once with the
  hardware's long-term device-unique key, and again with Android's
  standard key wrapping (tied to Verified Boot state + the user's Lock
  Screen Knowledge Factor).
- At runtime KeyMaster decrypts the on-disk keyblob, then re-wraps the
  class key with a **per-boot / per-class / per-user ephemeral key (EK)**
  that never leaves the secure environment. That ephemeral-wrapped form is
  what is cached in `vold` and the kernel keyring.
- When the kernel needs the key, TZ unwraps it, derives the 64-byte
  AES-256-XTS key, and programs the Inline Crypto Engine (ICE).

`FBE_SET_SEED` seeds the QSEE-side derivation context for that hierarchy.
It runs once per boot, from ABL, before any storage is mounted.

## Is the seed itself boot-state dependent?

**No — not directly.** The seed handed in `FBE_SET_SEED` is QSEE/ABL
key-derivation input material; nothing in the AOSP or Qualcomm
documentation indicates the seed value is a *function of* Verified Boot
color / `isUnlocked` / pubkey digest. The seed and the boot-state
(`SET_BOOT_STATE` 0x208, `SET_ROT` 0x201) are separate inputs delivered by
ABL to the KeyMaster TA.

That is exactly why the in-tree comment marks 0x218 `DO-NOT-MUTATE` and why
mode-1 already passes it through untouched: if mode-1 keeps the seed
byte-identical across boots (it does — the hook does not mutate it), the
seed contribution to the derivation is stable.

## What actually breaks FBE wrapping

The breakage is the **Root-of-Trust / boot-state change**, not the seed:

- KeyMaster keys are *cryptographically bound to the device Root of Trust*
  (the Verified Boot key) and, since Keymaster 2, to OS version + patch
  level. `Tag::ROOT_OF_TRUST` is bound into every key and cannot be
  overridden at creation/import time.
- The keys protecting CE/DE storage and filesystem metadata MUST be bound
  to a hardware-backed keystore that is itself bound to Verified Boot and
  the hardware Root of Trust.
- If a storage keyblob was created while KeyMaster's RoT context was
  *unlocked / ORANGE* (the real state of this test device), and the
  bootloader then presents KeyMaster a *different* RoT context
  (mode-1 fakelock: `isUnlocked 1->0`, and/or a different pubkey digest /
  GREEN color via `SET_BOOT_STATE`/`SET_ROT`), the long-term-wrapped
  keyblob no longer decrypts under the new context. `vold` then cannot
  unwrap the FBE class keys; `init`/`vold` failure to mount CE/DE storage
  escalates to Rescue Party → recovery → `/data` reformat.

This matches the sibling-repo RE observation
(`~/gbl_root_canoe/.re-notes/sessions/2026-04-26-keymaster-protocol-hook-v6.md`):
> "switching Keymaster to GREEN/locked can make wrapped storage keys fail
> to unwrap … the observed recovery/reformat behavior is plausibly not
> because AVB verification itself failed … but because Keymaster's
> boot-state mutation changed the cryptographic context used by
> storage/metadata keys."

## Verdict

**`FBE_SET_SEED` (0x218) is NOT the cause.** Confidence: high that the seed
is not the trigger; high that the RoT/boot-state mutation is.

- The seed is not boot-state-derived and mode-1 already passes it through
  unmutated, so it is stable across boots.
- FBE-wrap failure is the expected, documented consequence of changing the
  KeyMaster Root-of-Trust / boot-state (`SET_ROT` 0x201 /
  `SET_BOOT_STATE` 0x208) underneath storage keyblobs that were created
  under the device's real (unlocked/ORANGE) RoT.
- 0x218 is best treated as a **correlation witness**, not a suspect: log it
  to confirm the seed is identical across a clean boot and a fakelock
  boot. If `seedPfx` ever differs between modes, the assumption above is
  wrong and 0x218 must be re-examined.

## Recommended next step

1. Capture a `--debug` boot log on the test device with the new
   `GBL_INFO` 0x218 line present, in mode-0 (passthrough) and mode-1
   (fakelock). Confirm `seedPfx` is byte-identical across both — this
   validates "seed is stable, not the cause."
2. Focus the FBE fix on the RoT/boot-state path, not the seed. Options,
   roughly in order of preference:
   - Do not mutate `SET_ROT`/`SET_BOOT_STATE` *before* `vold`/storage
     init — i.e. keep the real (unlocked) RoT for the storage-key path and
     only spoof boot-state later / per-attestation, so storage keyblobs
     still unwrap under the RoT they were created with.
   - Or accept the one-time `/data` format: once userdata is *recreated*
     under the spoofed (locked) RoT, subsequent boots are self-consistent
     and FBE wrapping works again. (Acceptable per project memory —
     the test phone's `/data` is not load-bearing.)
3. Confirm the trigger with Android-side evidence (`logcat`, `vold`/`init`
   errors, recovery logs) on a fakelock boot — the bootloader log alone
   cannot show the `vold` unwrap failure.

## Code change accompanying this doc

`GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`, `KmDecodeKnownCmd`
case `0x218`: promoted from `VERBOSE` (compile-stripped from prod/`--debug`)
to `GBL_INFO` (visible in prod-via-UefiLog and `--debug`). The seed is
secret material — only a **4-byte hex prefix** (`seedPfx`) is logged, for
cross-boot correlation; raw seed bytes are never emitted. Not mode-gated.

## Sources

- AOSP — Hardware-wrapped keys:
  https://source.android.com/docs/security/features/encryption/hw-wrapped-keys
- AOSP — File-based encryption:
  https://source.android.com/docs/security/features/encryption/file-based
- AOSP — Metadata encryption:
  https://source.android.com/docs/security/features/encryption/metadata
- AOSP — Keymaster version binding / Root of Trust:
  https://source.android.com/docs/security/features/keystore/version-binding
- AOSP — KeyMint functions / `Tag::ROOT_OF_TRUST`:
  https://source.android.com/docs/security/features/keystore/implementer-ref
- Qualcomm — File Based Encryption (Snapdragon) whitepaper:
  https://www.qualcomm.com/media/documents/files/file-based-encryption.pdf
- Sibling RE notes —
  `~/gbl_root_canoe/.re-notes/sessions/2026-04-26-keymaster-protocol-hook-v6.md`

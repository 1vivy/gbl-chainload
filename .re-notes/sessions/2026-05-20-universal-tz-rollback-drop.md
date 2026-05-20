# 2026-05-20 — Universal TZ rollback-drop on-device verification

**Device:** infiniti
**Branch:** `universal-tz-rollback-drop` @ `6df1bdf` (impl `2671e58`)
**Log bundle:** `logs/20260520-020448_manual_drop-tz-rollback_v6df1bdf/`

## What fired

Both universal drops observed in `bootloader_log` and every `logfs/UefiLogSaved*.txt`:

```
scm-sip | smcid=0x02000801(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)
scm-sip | smcid=0x32000110(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID) | DROPPED (universal)
```

Infiniti uses the AB-aware path (`0x32000110`), as predicted for KM MajorVersion > 5. The legacy `0x0200011E` was not observed — expected.

ABL emits its own follow-up line right after the drop:
`TZ_UPDATE_ROLLBACK_VERSION_IF_A_B_PARTITION_FEATURE_ENABLED_ID failed, Status = (0x0)` — that's ABL reading the post-SMC out-result as 0 and interpreting it as a soft failure. It does not gate boot. This is the intended behavior of the drop: TZ never sees the bump, ABL notes the "failure", and boot proceeds.

## System behavior

- Device booted to system (lockscreen) normally.
- User-side grep for `KM_ERROR` / keymaster errors in logcat: no hits.
- `/data` intact across the boot (no factory-reset funnel).

## Verdict

**PASS.** Ready for PR.

# User oplusreserve1 locked/unlocked diff — 2026-05-13

Inputs:

```text
images/unlocked_oplusreserve1.img
  size 0x5a0000, SHA256 38ad52f90487fc2403941ff4bc15774eac3ed6b1a29a1d645c8eec884be150af

images/locked_oplusreserve1.img
  size 0x480000, SHA256 f94a7f1e10c4a1a69ba4df45cb7aa4f1f8980b96306cf379f6fe7b94bf229fd3
```

Tool added:

```text
scripts/dump-oplusreserve1.py
```

Usage:

```bash
python3 scripts/dump-oplusreserve1.py images/unlocked_oplusreserve1.img
python3 scripts/dump-oplusreserve1.py images/locked_oplusreserve1.img
python3 scripts/dump-oplusreserve1.py --diff images/unlocked_oplusreserve1.img images/locked_oplusreserve1.img
```

The script is read-only and dumps:

- image size/hash/block count,
- canonical 8 MiB-derived LBAs when present,
- tail-derived LBAs under explicit `tail_*` names for truncated/smaller dumps,
- DeepTest token fields,
- UnlockRecord fields/hash validity,
- changed block summaries for diffs.

## Important input caveat

These two files are not same-sized full 8 MiB `oplusreserve1` images:

```text
unlocked: 0x5a0000, 1440 blocks
locked:   0x480000, 1152 blocks
```

The canonical token block from prior full dumps is still present in both:

```text
LBA 1114 / offset 0x45a000 = LastBlock-0x3a5 in full 8 MiB images
```

The canonical UnlockRecord block is present only in the unlocked file:

```text
LBA 1187 / offset 0x4a3000 = LastBlock-0x35c in full 8 MiB images
```

The locked file ends at `0x480000`, before canonical UnlockRecord offset `0x4a3000`. Therefore this pair can prove token zeroing, but cannot compare the post-lock canonical UnlockRecord.

## Key result: lock zeroes the DeepTest token block

At canonical token block `LBA 1114 / offset 0x45a000`:

Unlocked/user image:

```text
sha256: a48b541232505819cc1021f9a401b89894e0db95aea2bbdc651ca81520607509
nonzero_full: 316
nonzero_first_0x140: 316
serial:     6568f010
marker:     0002
permission: 1
binding31:  0000000000000000000000000000000
model:      PLK110##########
```

Locked/user image:

```text
sha256: ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7
nonzero_full: 0
all zero: yes
```

Interpretation: this user pair directly confirms the previous static finding: post-lock state has the canonical `fastboot_unlock_data` / DeepTest token block zeroed.

## UnlockRecord observation from unlocked file

Canonical UnlockRecord in `unlocked_oplusreserve1.img` at `LBA 1187 / offset 0x4a3000`:

```text
magic:       0x939c978a
hash_valid:  yes
stored hash: 5e3ac6bedcb25661c6319b321b22a0a87eb6c4e80b71bdd349cf10c6d7edf75c
version:     1
serial_hash: 8bc7c810541cd0c937dc4e3b48f248a4
counter:     1
status:      0
```

`serial_hash` matches the first 16 bytes of:

```text
SHA256("6568f010") = 8bc7c810541cd0c937dc4e3b48f248a4358ef2b529cad25f4d473d67c6f5e6db
```

Notable implication: this image has a valid token block but a locked-looking UnlockRecord (`status=0`). This again supports the prior conclusion that UnlockRecord is accounting/display state, not the fastboot gate input. Token presence is the important preserve vector.

## Other changed common blocks

Comparing the overlapping first `0x480000` bytes found 32 changed 4K blocks. Aside from token LBA 1114, the visible changes are mostly boot/download/log data:

```text
LBA 1063 / 0x427000: ASCII download-over record
  unlocked: 2026-04-28 12:02:33, PLK110_16.0.5.701(CN01...)
  locked:   2026-03-11 02:31:04, PLK110_16.0.3.503(CN01...)

LBA 1080 / 0x438000: "bootlog0" header/counters differ
LBA 1081..1108: high-entropy/log blocks differ
LBA 1110 / 0x456000: "kernelog" small counter differs (4 -> 3)
LBA 1114 / 0x45a000: DeepTest token populated -> zero
```

Known stable block:

```text
LBA 1065 / 0x429000 ENABLE_UART:FALSE is identical in both images.
```

## Takeaways

1. The diff empirically validates the static state-8 token wipe path.
2. The mode-1 write-swallow preserving `LBA 1114 / LastBlock-0x3a5` is necessary and correctly targeted.
3. The locked sample is truncated before the canonical UnlockRecord block, so it cannot answer post-lock UnlockRecord status/counter for this user.
4. The unlocked sample's valid token + locked-looking UnlockRecord is useful evidence that UnlockRecord is not the fastboot authorization record.

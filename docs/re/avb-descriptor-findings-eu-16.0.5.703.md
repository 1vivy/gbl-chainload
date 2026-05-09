# AVB descriptor findings — EU 16.0.5.703 firmware

Firmware path:

```text
~/Downloads/RegionalHybrid Flasher 15 EU 16.0.5.703/OOS_FILES_HERE
```

Tooling:

```text
~/android/fox_14.1/external/avb/avbtool.py
scripts/extract-avb-embedded-vbmeta.py
```

## Summary

The per-partition bootconfig fields are not primarily an Oplus addition. They are built by Qualcomm/ABL AVB code from `AvbSlotVerifyData` / loaded partition metadata and then converted from `androidboot.*` cmdline entries into bootconfig by Qualcomm BootLib.

Evidence:

- `LinuxLoader_infiniti.efi` contains `androidboot.vbmeta.` and `_ROOT_DIGEST)` strings, but not hardcoded full strings for every partition. This suggests code constructs fields dynamically as `androidboot.vbmeta.<partition>.{hash_alg,digest}`.
- `UpdateCmdLine.c` generically moves any `androidboot.*` param into bootconfig for boot header v4+.
- The stock `vbmeta.img` contains chain descriptors for boot-style partitions; child images carry their own embedded vbmeta/footer and hash descriptors.

## Main vbmeta descriptors

`vbmeta.img`:

```text
Algorithm: SHA256_RSA4096
Public key sha1: a568677dbd61e0aa97ca6f3183dc28f505d1b83c
Rollback index: 0

Chain Partition descriptor: boot, rollback index location 4, key a568677d...
Chain Partition descriptor: dtbo, rollback index location 3, key a568677d...
Chain Partition descriptor: recovery, rollback index location 1, key a568677d...
Chain Partition descriptor: vbmeta_system, rollback index location 2, key 42e345c9...
Chain Partition descriptor: vbmeta_vendor, rollback index location 5, key 42e345c9...

Hash descriptor: init_boot
  image size: 3129344
  digest: 5d4304b5baa9ca8940efde67523fcb801f11c6d6c20703ae28f38d52688763df

Hash descriptor: vendor_boot
  image size: 25038848
  digest: 13425e43d380befcf9f17864c887b28bc73cae6cb7e7bcc39c18f0992105980d
```

`vbmeta_system.img` contains hashtree descriptors for product/system/system_ext and a hash descriptor for pvmfw:

```text
pvmfw digest: e0b3a8f75995978a4641a02566dea772fddbc2e5bbef7922aef8b6ed869c9fe8
```

`vbmeta_vendor.img` contains a hashtree descriptor for vendor.

## Embedded vbmeta descriptors

`boot.img`:

```text
Footer original image size: 39911424
VBMeta offset: 39911424
VBMeta size: 2368
Algorithm: SHA256_RSA4096
Hash descriptor: boot
  image size: 39911424
  digest: fd11be2666d963943adbc8a5093209a2659a632781d7549748a0fd6e713d5d39
```

`recovery.img`:

```text
Footer original image size: 39288832
VBMeta offset: 39288832
VBMeta size: 2240
Algorithm: SHA256_RSA4096
Hash descriptor: recovery
  image size: 39288832
  digest: 26d30936308f26ce6fd8badb55b4a4a22c9325b370835fa6af8b780907993a26
```

`dtbo.img`:

```text
Footer original image size: 10404240
VBMeta offset: 10407936
VBMeta size: 2240
Algorithm: SHA256_RSA4096
Hash descriptor: dtbo
  image size: 10404240
  digest: 7e881b0cc56f8a8a1e6d403b4e1cf70ae887e917311488fea2e6e43027af83a0
```

`init_boot.img` has an embedded vbmeta footer with algorithm `NONE`; its hash descriptor digest matches the main vbmeta init_boot digest.

`vendor_boot.img` has an embedded vbmeta footer with algorithm `NONE`; its hash descriptor digest matches the main vbmeta vendor_boot digest.

`pvmfw.img` has an embedded signed vbmeta footer whose hash descriptor digest matches `vbmeta_system.img`'s pvmfw descriptor.

## Cached blob extraction

Generated local cache artifacts:

```text
build/avb-cache/eu-16.0.5.703/boot/boot.avb_footer.bin
build/avb-cache/eu-16.0.5.703/boot/boot.embedded_vbmeta.bin
build/avb-cache/eu-16.0.5.703/recovery/recovery.avb_footer.bin
build/avb-cache/eu-16.0.5.703/recovery/recovery.embedded_vbmeta.bin
build/avb-cache/eu-16.0.5.703/dtbo/dtbo.avb_footer.bin
build/avb-cache/eu-16.0.5.703/dtbo/dtbo.embedded_vbmeta.bin
```

Notable hashes:

```text
boot embedded vbmeta sha256:     f92f03bb0a3a85d72a9a4d3a5cc6623f02b45124abcbea19ca8b5865fa9c8813
recovery embedded vbmeta sha256: 26cff6719c9c16ec6c62e567c3e04eb2074d1898b566b196336d71ea589e33b5
dtbo embedded vbmeta sha256:     e3bb4e600bab0e07b1046e10758d88d9d25521e15bb55795aaf5988d52d84085
```

## Bootconfig source conclusion

Observed bootconfig from logs includes:

```text
androidboot.vbmeta.boot.hash_alg = "sha256"
androidboot.vbmeta.boot.digest = "fd11be2666d963943adbc8a5093209a2659a632781d7549748a0fd6e713d5d39"
androidboot.vbmeta.dtbo.digest = "7e881b0cc56f8a8a1e6d403b4e1cf70ae887e917311488fea2e6e43027af83a0"
androidboot.vbmeta.pvmfw.digest = "e0b3a8f75995978a4641a02566dea772fddbc2e5bbef7922aef8b6ed869c9fe8"
androidboot.vbmeta.init_boot.digest = "5d4304b5baa9ca8940efde67523fcb801f11c6d6c20703ae28f38d52688763df"
androidboot.vbmeta.vendor_boot.digest = "13425e43d380befcf9f17864c887b28bc73cae6cb7e7bcc39c18f0992105980d"
```

These match AVB hash descriptors from `vbmeta.img`, child embedded vbmeta, or `vbmeta_system.img`:

- boot: child `boot.img` embedded vbmeta hash descriptor.
- dtbo: child `dtbo.img` embedded vbmeta hash descriptor.
- pvmfw: `vbmeta_system.img` hash descriptor.
- init_boot: main `vbmeta.img` hash descriptor and `init_boot.img` embedded unsigned descriptor.
- vendor_boot: main `vbmeta.img` hash descriptor and `vendor_boot.img` embedded unsigned descriptor.

The recovery value in the captured logs (`0cd944a7...`) does **not** match the EU stock `recovery.img` descriptor (`26d30936...`). This likely means the captured recovery userspace bootconfig came from a different current/custom recovery image or a different regional image than this EU firmware set. Do not use the captured recovery digest as proof for the EU stock recovery descriptor.

## Implications for gbl-chainload

The ABL-side bootconfig additions are likely Qualcomm/AVB dynamic output, not Oplus hand-written additions. That means the best patch remains input-level:

1. keep child embedded vbmeta/footer parseable for chained boot partitions;
2. let ABL compute/load descriptors normally;
3. avoid after-the-fact bootconfig string rewriting.

For custom recovery/dtbo, source stock embedded footer/vbmeta from firmware images or cached install data. Main `vbmeta.img` gives the chain descriptor and public key, but not the stock child vbmeta bytes required for parse and for `slot_data->vbmeta_images[]` digest construction.

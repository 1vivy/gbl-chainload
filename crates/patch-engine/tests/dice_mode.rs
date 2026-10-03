#![cfg(feature = "host")]

use patch_engine::{dice_mode, PatchOutcome};

const STOCK: &[u8] = include_bytes!("../../../tests/images/pe/infiniti-EU-16.0.5.703.efi");

#[test]
fn both_bcc_paths_become_normal_and_reapply_is_stable() {
    let mut pe = STOCK.to_vec();
    assert_eq!(dice_mode::apply(&mut pe), PatchOutcome::Ok);
    let changed: Vec<_> = STOCK
        .chunks_exact(4)
        .zip(pe.chunks_exact(4))
        .filter_map(|(before, after)| {
            (before != after).then(|| {
                (
                    u32::from_le_bytes(before.try_into().unwrap()),
                    u32::from_le_bytes(after.try_into().unwrap()),
                )
            })
        })
        .collect();
    // Main mode selection becomes Normal. The dummy branch is removed,
    // making its mode write unconditional for both locked and unlocked ABLs.
    assert_eq!(changed.len(), 3);
    assert!(changed.contains(&(0x1a89_0108, 0x5280_0028)));
    assert!(changed.contains(&(0x3400_0068, 0xd503_201f)));
    assert!(changed.contains(&(0x5280_0048, 0x5280_0028)));
    let once = pe.clone();
    assert_eq!(dice_mode::apply(&mut pe), PatchOutcome::Ok);
    assert_eq!(pe, once);
}

#[test]
fn missing_second_site_never_partially_patches() {
    let mut pe = STOCK.to_vec();
    let anchor = b"VB: Setting Dummy DICE params\n\0";
    let off = pe.windows(anchor.len()).position(|bytes| bytes == anchor).unwrap();
    pe[off] ^= 1;
    let original = pe.clone();
    assert_eq!(dice_mode::apply(&mut pe), PatchOutcome::Miss);
    assert_eq!(pe, original);
}

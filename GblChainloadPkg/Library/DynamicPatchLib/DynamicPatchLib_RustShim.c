/** @file DynamicPatchLib_RustShim.c
 *
 *  PR2 Task 6: the patch engine moved to crates/patch-engine. The Rust
 *  staticlib (libpatch_engine.a) exports `DynamicPatchLib_EnsureInit` +
 *  `DynamicPatch_Apply` as the firmware-side symbols.
 *
 *  EDK2's library-class machinery requires at least one C source per
 *  .inf to attach the library to. This placeholder satisfies that —
 *  the actual implementation is the Rust staticlib pulled in via
 *  DLINK2_FLAGS in GblChainloadPkg.dsc.
 **/

/* Empty translation unit — a single static-storage decl keeps GCC/Clang
 * silent in -Wempty-translation-unit mode. */
static const char kDynamicPatchLib_RustShim_Tag[] __attribute__((used)) =
    "patch-engine-rust";

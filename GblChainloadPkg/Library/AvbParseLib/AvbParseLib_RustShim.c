/** @file AvbParseLib_RustShim.c
 *
 *  PR2 Task 7: AVB structure parsing moved to crates/avb-parse. The
 *  Rust staticlib (libavb_parse.a) exports every `AvbParse_*` symbol
 *  the firmware + host tools call into.
 *
 *  EDK2's library-class machinery requires at least one C source per
 *  .inf to attach the library to. This placeholder satisfies that —
 *  the actual implementation is the Rust staticlib pulled in via
 *  DLINK2_FLAGS in GblChainloadPkg.dsc.
 **/

/* Empty translation unit — a single static-storage decl keeps GCC/Clang
 * silent in -Wempty-translation-unit mode. */
static const char kAvbParseLib_RustShim_Tag[] __attribute__((used)) =
    "avb-parse-rust";

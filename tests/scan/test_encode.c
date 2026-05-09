/** test_encode.c — TDD tests for WriteInstrU32, ReadInstrU32, EncodeCbz,
    RewriteCbz in Encode.c. **/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "Encode.h"

/* -------------------------------------------------------------------------
 * Test helpers
 * ------------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(got, want, label)                                        \
  do {                                                                     \
    if ((got) == (want)) {                                                 \
      printf ("[PASS] %s\n", (label));                                     \
      g_pass++;                                                            \
    } else {                                                               \
      printf ("[FAIL] %s: got 0x%X, want 0x%X\n", (label),               \
              (unsigned)(got), (unsigned)(want));                          \
      g_fail++;                                                            \
    }                                                                      \
  } while (0)

/* -------------------------------------------------------------------------
 * Test 1: WriteInstrU32 / ReadInstrU32 roundtrip + little-endian byte order
 * ------------------------------------------------------------------------- */
static void
test_write_read_roundtrip (void)
{
  UINT8  Buf[32];
  UINT32 Val;

  memset (Buf, 0, sizeof (Buf));
  WriteInstrU32 (Buf, 12, 0xDEADBEEFu);

  /* Verify raw bytes are little-endian */
  EXPECT_EQ (Buf[12], 0xEF, "roundtrip: Buf[12] == 0xEF");
  EXPECT_EQ (Buf[13], 0xBE, "roundtrip: Buf[13] == 0xBE");
  EXPECT_EQ (Buf[14], 0xAD, "roundtrip: Buf[14] == 0xAD");
  EXPECT_EQ (Buf[15], 0xDE, "roundtrip: Buf[15] == 0xDE");

  /* Verify readback */
  Val = ReadInstrU32 (Buf, 12);
  EXPECT_EQ (Val, 0xDEADBEEFu, "roundtrip: ReadInstrU32 == 0xDEADBEEF");
}

/* -------------------------------------------------------------------------
 * Test 2: EncodeCbz known value — forward branch
 *   cbz w24, +0x4  (InsnOff=0, TargetOff=4, delta=+1 instr)
 *   Expected: 0x34000000 | (1 << 5) | 24 = 0x34000038
 * ------------------------------------------------------------------------- */
static void
test_encode_cbz_known_value_forward (void)
{
  UINT32  Insn = 0;
  BOOLEAN Ok;

  Ok = EncodeCbz (0, 4, 24, &Insn);
  EXPECT_EQ (Ok,   TRUE,       "cbz_forward: returns TRUE");
  EXPECT_EQ (Insn, 0x34000038u, "cbz_forward: encodes 0x34000038");
}

/* -------------------------------------------------------------------------
 * Test 3: EncodeCbz known value — backward branch
 *   cbz w0, -0x4  (InsnOff=0x100, TargetOff=0xFC, delta=-1 instr)
 *   imm19 = -1 → 19-bit two's complement = 0x7FFFF
 *   Expected: 0x34000000 | (0x7FFFF << 5) | 0 = 0x34FFFFE0
 * ------------------------------------------------------------------------- */
static void
test_encode_cbz_known_value_backward (void)
{
  UINT32  Insn = 0;
  BOOLEAN Ok;

  Ok = EncodeCbz (0x100, 0xFC, 0, &Insn);
  EXPECT_EQ (Ok,   TRUE,        "cbz_backward: returns TRUE");
  EXPECT_EQ (Insn, 0x34FFFFe0u, "cbz_backward: encodes 0x34FFFFE0");
}

/* -------------------------------------------------------------------------
 * Test 4: EncodeCbz — misaligned target → FALSE
 * ------------------------------------------------------------------------- */
static void
test_encode_cbz_misaligned_target (void)
{
  UINT32  Insn = 0xDEADBEEFu;
  BOOLEAN Ok;

  /* target = InsnOff + 5: not 4-byte aligned */
  Ok = EncodeCbz (0, 5, 0, &Insn);
  EXPECT_EQ (Ok, FALSE, "cbz_misaligned: returns FALSE");
}

/* -------------------------------------------------------------------------
 * Test 5: EncodeCbz — out of range → FALSE
 *   19-bit signed range: [-2^18, 2^18-1] instructions = [-0x80000, 0x7FFFC] bytes.
 *   Use delta > 0x7FFFC bytes (e.g. TargetOff = 0x100000, InsnOff = 0).
 * ------------------------------------------------------------------------- */
static void
test_encode_cbz_out_of_range (void)
{
  UINT32  Insn = 0xDEADBEEFu;
  BOOLEAN Ok;

  Ok = EncodeCbz (0, 0x100000u, 0, &Insn);
  EXPECT_EQ (Ok, FALSE, "cbz_out_of_range: returns FALSE");
}

/* -------------------------------------------------------------------------
 * Test 6: EncodeCbz — Reg > 31 → FALSE
 * ------------------------------------------------------------------------- */
static void
test_encode_cbz_bad_reg (void)
{
  UINT32  Insn = 0xDEADBEEFu;
  BOOLEAN Ok;

  Ok = EncodeCbz (0, 4, 32, &Insn);
  EXPECT_EQ (Ok, FALSE, "cbz_bad_reg: returns FALSE");
}

/* -------------------------------------------------------------------------
 * Test 7: RewriteCbz — buffer check
 *   Buffer 256 bytes, RewriteCbz at offset 16, reg=24, target=20.
 *   Expected word at offset 16: 0x34000038
 *   Bytes outside [16,20) should be unchanged (all 0xCC).
 * ------------------------------------------------------------------------- */
static void
test_rewrite_cbz_buffer_check (void)
{
  UINT8   Buf[256];
  UINT32  Got;
  BOOLEAN Ok;

  memset (Buf, 0xCC, sizeof (Buf));
  Ok = RewriteCbz (Buf, 16, 24, 20);
  EXPECT_EQ (Ok, TRUE, "rewrite_cbz: returns TRUE");

  Got = ReadInstrU32 (Buf, 16);
  EXPECT_EQ (Got, 0x34000038u, "rewrite_cbz: word at offset 16 == 0x34000038");

  /* Bytes before the instruction word must be untouched */
  EXPECT_EQ (Buf[15], 0xCC, "rewrite_cbz: Buf[15] untouched");
  /* Bytes after the 4-byte word must be untouched */
  EXPECT_EQ (Buf[20], 0xCC, "rewrite_cbz: Buf[20] untouched");
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int
main (void)
{
  test_write_read_roundtrip ();
  test_encode_cbz_known_value_forward ();
  test_encode_cbz_known_value_backward ();
  test_encode_cbz_misaligned_target ();
  test_encode_cbz_out_of_range ();
  test_encode_cbz_bad_reg ();
  test_rewrite_cbz_buffer_check ();

  printf ("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

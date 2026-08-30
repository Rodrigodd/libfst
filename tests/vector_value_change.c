#include <fstapi.h>
#include <glib.h>
#include <string.h>

// The typed value change helpers all funnel into fstWriterEmitValueChange()
// after formatting the value as a binary string.

static void expect_value(fstReaderContext *reader, uint64_t tim, fstHandle handle, const char *want)
{
    char buffer[256];
    char *got = fstReaderGetValueFromHandleAtTime(reader, tim, handle, buffer);

    g_assert_true(got != NULL);
    g_assert_cmpstr(got, ==, want);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("vectors.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle small = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 12, "small", 0);
    fstHandle wide = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 40, "wide", 0);
    fstHandle vec = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 100, "vec", 0);

    fstWriterEmitTimeChange(writer, 0);

    fstWriterEmitValueChange32(writer, small, 12, 0xA5Au);
    fstWriterEmitValueChange64(writer, wide, 40, 0xDEADBEEFull);

    // A width that is not a whole number of words exercises both the remainder
    // and the whole word loops of fstWriterEmitValueChangeVec32().

    uint32_t vec_val[4] = {0x00000001u, 0x00000000u, 0x00000000u, 0x0000000Fu};
    fstWriterEmitValueChangeVec32(writer, vec, 100, vec_val);

    // Widths that fit in a single word delegate to the scalar helpers.

    uint32_t narrow32 = 0x2Au;
    uint64_t narrow64 = 0x123456789ABCDEF0ull;

    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitValueChangeVec32(writer, small, 12, &narrow32);
    fstWriterEmitValueChangeVec64(writer, wide, 40, &narrow64);
    fstWriterEmitDumpActive(writer, 0);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitDumpActive(writer, 1);
    fstWriterEmitValueChange32(writer, small, 12, 0);

    fstWriterEmitTimeChange(writer, 15);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("vectors.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetMaxHandle(reader), ==, 3);

    // 0xA5A truncated to 12 bits, then 0x2A written through the vector helper.

    expect_value(reader, 0, small, "101001011010");
    expect_value(reader, 5, small, "000000101010");
    expect_value(reader, 10, small, "000000000000");

    // 0xDEADBEEF in 40 bits, then the low 40 bits of 0x123456789ABCDEF0.

    expect_value(reader, 0, wide, "0000000011011110101011011011111011101111");
    expect_value(reader, 5, wide, "0111100010011010101111001101111011110000");

    // 100 bits: a 4 bit remainder word followed by three 32 bit words.

    expect_value(reader,
                 0,
                 vec,
                 "1111"
                 "00000000000000000000000000000000"
                 "00000000000000000000000000000000"
                 "00000000000000000000000000000001");

    // An unknown handle has no value.

    char buffer[256];
    g_assert_true(fstReaderGetValueFromHandleAtTime(reader, 0, 99, buffer) == NULL);

    fstReaderClose(reader);

    return 0;
}

/*
 * Vector values whose width is a whole number of words round trip, and neither
 * emitter reads past the end of the value array while writing one.  A read
 * past the end would not change the emitted value, so that half of this only
 * fails under a sanitizer.
 */

#include <fstapi.h>
#include <glib.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Widths that are a whole number of words, so that there is no remainder
    // word for the emitters to read.

    uint32_t narrow[2] = {0x89ABCDEFu, 0x01234567u};
    uint64_t wide[2] = {0xFEDCBA9876543210ull, 0x0123456789ABCDEFull};

    fstWriterContext *writer = fstWriterCreate("word_aligned_vectors.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle vec32 = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 64, "vec32", 0);
    fstHandle vec64 = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 128, "vec64", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChangeVec32(writer, vec32, 64, narrow);
    fstWriterEmitValueChangeVec64(writer, vec64, 128, wide);
    fstWriterEmitTimeChange(writer, 1);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("word_aligned_vectors.fst");
    g_assert_true(reader != NULL);

    char buffer[256];

    char *got32 = fstReaderGetValueFromHandleAtTime(reader, 0, vec32, buffer);
    g_assert_true(got32 != NULL);
    g_assert_cmpstr(got32,
                    ==,
                    "00000001001000110100010101100111"
                    "10001001101010111100110111101111");

    char *got64 = fstReaderGetValueFromHandleAtTime(reader, 0, vec64, buffer);
    g_assert_true(got64 != NULL);
    g_assert_cmpstr(got64,
                    ==,
                    "0000000100100011010001010110011110001001101010111100110111101111"
                    "1111111011011100101110101001100001110110010101000011001000010000");

    fstReaderClose(reader);

    return 0;
}

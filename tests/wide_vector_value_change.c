/*
 * fstWriterEmitValueChangeVec64() keeps every bit of a value that is wider
 * than one word.
 */

#include <fstapi.h>
#include <glib.h>

// The value the writer is expected to produce: every bit of the little endian
// word array from the most significant one down.
static gchar *expected_binary(const uint64_t *words, uint32_t bits)
{
    GString *text = g_string_new(NULL);

    for (int bit = (int)bits - 1; bit >= 0; bit--) {
        g_string_append_c(text, '0' + ((words[bit / 64] >> (bit % 64)) & 1));
    }

    return g_string_free(text, FALSE);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // 100 bits is one whole word plus a 36 bit remainder.  Both halves of
    // both words carry a distinct pattern.

    const uint32_t bits = 100;
    uint64_t value[2] = {0xFEDCBA9876543210ull, 0x0000000ABCDEF123ull};

    fstWriterContext *writer = fstWriterCreate("wide_vectors.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle vec = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, bits, "vec", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChangeVec64(writer, vec, bits, value);
    fstWriterEmitTimeChange(writer, 1);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("wide_vectors.fst");
    g_assert_true(reader != NULL);

    char buffer[256];
    char *got = fstReaderGetValueFromHandleAtTime(reader, 0, vec, buffer);
    g_assert_true(got != NULL);

    gchar *expected = expected_binary(value, bits);
    g_assert_cmpstr(got, ==, expected);
    g_free(expected);

    fstReaderClose(reader);

    return 0;
}

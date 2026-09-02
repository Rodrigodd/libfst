#include <fstapi.h>
#include <glib.h>
#include <string.h>

// A value change section packed with FASTLZ, read back through both entry
// points.  The chain of a packed signal is a length varint followed by the
// compressed bytes, and the reader has to hand the decompressor the second
// part only.  Two signals are needed: the bytes a reader reads past the end of
// one chain are the beginning of the next one.

#define WIDTH_A 120
#define WIDTH_B 129
#define STEPS 4

static char values_a[STEPS][WIDTH_A + 1];
static char values_b[STEPS][WIDTH_B + 1];

struct collected
{
    int count;
    int mismatches;
};

static void value_change_callback(void *user_data,
                                  uint64_t time,
                                  fstHandle handle,
                                  const unsigned char *value)
{
    struct collected *c = user_data;
    const char *expected;

    g_assert_cmpint(time, <, STEPS);
    g_assert_true(handle == 1 || handle == 2);

    expected = (handle == 1) ? values_a[time] : values_b[time];

    if (strcmp((const char *)value, expected) != 0) {
        c->mismatches++;
    }

    c->count++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Runs of a repeated character are what makes a chain worth packing, so
    // that the writer takes the compressed branch rather than storing it raw.

    for (int step = 0; step < STEPS; step++) {
        memset(values_a[step], (step & 1) ? '1' : '0', WIDTH_A);
        values_a[step][WIDTH_A] = 0;

        memset(values_b[step], (step & 1) ? '0' : '1', WIDTH_B);
        values_b[step][WIDTH_B] = 0;
    }

    fstWriterContext *writer = fstWriterCreate("fastlz_pack.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetPackType(writer, FST_WR_PT_FASTLZ);

    fstHandle var_a =
        fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, WIDTH_A, "bus_a", 0);
    fstHandle var_b =
        fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_INOUT, WIDTH_B, "bus_b", 0);

    for (int step = 0; step < STEPS; step++) {
        fstWriterEmitTimeChange(writer, step);
        fstWriterEmitValueChange(writer, var_a, values_a[step]);
        fstWriterEmitValueChange(writer, var_b, values_b[step]);
    }

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("fastlz_pack.fst");
    g_assert_true(reader != NULL);

    // Every value change has to come back through the iteration callback
    // unchanged.

    struct collected collected = {0, 0};

    fstReaderSetFacProcessMaskAll(reader);
    g_assert_cmpint(fstReaderIterBlocks2(reader, value_change_callback, NULL, &collected, NULL),
                    !=,
                    0);

    g_assert_cmpint(collected.count, ==, 2 * STEPS);
    g_assert_cmpint(collected.mismatches, ==, 0);

    // And so does every value served by the random access reader, which
    // decompresses a single chain on its own.

    for (int step = 0; step < STEPS; step++) {
        char buf[WIDTH_B + 64];
        char *value;

        value = fstReaderGetValueFromHandleAtTime(reader, step, var_a, buf);
        g_assert_true(value != NULL);
        g_assert_cmpstr(value, ==, values_a[step]);

        value = fstReaderGetValueFromHandleAtTime(reader, step, var_b, buf);
        g_assert_true(value != NULL);
        g_assert_cmpstr(value, ==, values_b[step]);
    }

    fstReaderClose(reader);

    return 0;
}

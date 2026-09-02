#include <fstapi.h>
#include <glib.h>

// Handle zero is what fstWriterCreateVar() returns when it fails, so a caller
// that does not check reaches the emitters with it.  Both of them have to treat
// it as no signal at all rather than as a handle to index by.

static int changes;

static void value_change_callback(void *user_data,
                                  uint64_t time,
                                  fstHandle handle,
                                  const unsigned char *value)
{
    (void)user_data;
    (void)time;
    (void)handle;
    (void)value;

    changes++;
}

static void value_change_callback_varlen(void *user_data,
                                         uint64_t time,
                                         fstHandle handle,
                                         const unsigned char *value,
                                         uint32_t len)
{
    (void)user_data;
    (void)time;
    (void)handle;
    (void)value;
    (void)len;

    changes++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("handle_zero.fst", 1);
    g_assert_true(writer != NULL);

    // A variable with no name is rejected, so this is the handle a caller that
    // does not check ends up passing on.

    fstHandle none = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 8, NULL, 0);
    g_assert_cmpint(none, ==, 0);

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 8, "bus", 0);
    g_assert_cmpint(var, ==, 1);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "00000000");

    fstWriterEmitTimeChange(writer, 1);

    // Neither of these may be taken for a signal: handle - 1 underflows and
    // the index derived from it is far outside the per handle arrays.

    fstWriterEmitValueChange(writer, none, "11111111");
    fstWriterEmitVariableLengthValueChange(writer, none, "hello", 5);

    fstWriterEmitTimeChange(writer, 2);
    fstWriterEmitValueChange(writer, var, "11111111");

    fstWriterClose(writer);

    // Only the two changes of the real signal reach the file.

    fstReaderContext *reader = fstReaderOpen("handle_zero.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetMaxHandle(reader), ==, 1);
    g_assert_cmpint(fstReaderGetVarCount(reader), ==, 1);

    fstReaderSetFacProcessMaskAll(reader);
    g_assert_cmpint(fstReaderIterBlocks2(reader,
                                         value_change_callback,
                                         value_change_callback_varlen,
                                         NULL,
                                         NULL),
                    !=,
                    0);

    g_assert_cmpint(changes, ==, 2);

    fstReaderClose(reader);

    return 0;
}

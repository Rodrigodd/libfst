/*
 * A variable length value change emitted before the first
 * fstWriterEmitTimeChange() is kept.
 */

#include <fstapi.h>
#include <glib.h>

typedef struct
{
    uint64_t time;
    fstHandle handle;
    gchar *value;
} ValueChange;

static void value_change_callback_varlen(void *user_data,
                                         uint64_t time,
                                         fstHandle handle,
                                         const unsigned char *value,
                                         uint32_t length)
{
    GArray *changes = user_data;
    ValueChange change = {time, handle, g_strndup((const gchar *)value, length)};

    g_array_append_val(changes, change);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("varlen_before_first_time_change.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    /* a length of zero makes this a variable length variable */
    fstHandle var = fstWriterCreateVar(writer, FST_VT_GEN_STRING, FST_VD_IMPLICIT, 0, "str", 0);

    fstWriterEmitVariableLengthValueChange(writer, var, "before", 6); /* no time change yet */
    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitVariableLengthValueChange(writer, var, "after", 5);
    fstWriterEmitTimeChange(writer, 10);

    fstWriterClose(writer);

    GArray *changes = g_array_new(FALSE, FALSE, sizeof(ValueChange));

    fstReaderContext *reader = fstReaderOpen("varlen_before_first_time_change.fst");
    g_assert_true(reader != NULL);

    /* a value change was emitted before the first time change, so the file
     * starts at 0 rather than at 5 */
    g_assert_cmpint(fstReaderGetStartTime(reader), ==, 0);
    g_assert_cmpint(fstReaderGetEndTime(reader), ==, 10);

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocks2(reader, NULL, value_change_callback_varlen, changes, NULL);
    fstReaderClose(reader);

    g_assert_cmpint(changes->len, ==, 2);

    g_assert_cmpint(g_array_index(changes, ValueChange, 0).time, ==, 0);
    g_assert_cmpstr(g_array_index(changes, ValueChange, 0).value, ==, "before");
    g_assert_cmpint(g_array_index(changes, ValueChange, 1).time, ==, 5);
    g_assert_cmpstr(g_array_index(changes, ValueChange, 1).value, ==, "after");

    g_array_free(changes, TRUE);

    return 0;
}

/*
 * Time stamps stay correct across fstWriterFlushContext(), both when the
 * queued flush writes a section and when it turns out to have nothing to
 * write.
 */

#include <fstapi.h>
#include <glib.h>

typedef struct
{
    uint64_t time;
    fstHandle handle;
    gchar *value;
} ValueChange;

static void value_change_callback(void *user_data,
                                  uint64_t time,
                                  fstHandle handle,
                                  const unsigned char *value)
{
    GArray *changes = user_data;
    ValueChange change = {time, handle, g_strdup((const gchar *)value)};

    g_array_append_val(changes, change);
}

static GArray *read_value_changes(const char *filename)
{
    GArray *changes = g_array_new(FALSE, FALSE, sizeof(ValueChange));

    fstReaderContext *reader = fstReaderOpen(filename);
    g_assert_true(reader != NULL);

    /* the value changes below have to agree with this */
    g_assert_cmpint(fstReaderGetStartTime(reader), ==, 10);
    g_assert_cmpint(fstReaderGetEndTime(reader), ==, 40);

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocks2(reader, value_change_callback, NULL, changes, NULL);
    fstReaderClose(reader);

    return changes;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // A flush with a value change pending starts a new section, and every time
    // stamp stays correct

    fstWriterContext *writer = fstWriterCreate("flush_context_pending.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "var", 0);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, var, "00000001");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitTimeChange(writer, 30);
    fstWriterFlushContext(writer);
    fstWriterEmitTimeChange(writer, 40);
    fstWriterEmitValueChange(writer, var, "00000010");

    fstWriterClose(writer);

    GArray *changes = read_value_changes("flush_context_pending.fst");

    g_assert_cmpint(changes->len, ==, 2);
    g_assert_cmpint(g_array_index(changes, ValueChange, 0).time, ==, 10);
    g_assert_cmpstr(g_array_index(changes, ValueChange, 0).value, ==, "00000001");
    g_assert_cmpint(g_array_index(changes, ValueChange, 1).time, ==, 40);
    g_assert_cmpstr(g_array_index(changes, ValueChange, 1).value, ==, "00000010");

    g_array_free(changes, TRUE);

    // The same history, except that no value change is pending when the flush
    // is queued, so it cannot write a section.  The time stamps have to come
    // out the same either way.

    writer = fstWriterCreate("flush_context_empty.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "var", 0);

    /* three time steps, so that the flush below is actually queued */
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitTimeChange(writer, 30);
    fstWriterFlushContext(writer);
    fstWriterEmitTimeChange(writer, 40);
    fstWriterEmitValueChange(writer, var, "00000001");

    fstWriterClose(writer);

    changes = read_value_changes("flush_context_empty.fst");

    g_assert_cmpint(changes->len, ==, 1);
    g_assert_cmpint(g_array_index(changes, ValueChange, 0).time, ==, 40);
    g_assert_cmpstr(g_array_index(changes, ValueChange, 0).value, ==, "00000001");

    g_array_free(changes, TRUE);

    return 0;
}

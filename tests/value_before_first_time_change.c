/*
 * A value change emitted before the first fstWriterEmitTimeChange() is kept,
 * and fstReaderIterBlocks2() and fstReaderGetValueFromHandleAtTime() report
 * the same history for it.
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

static const ValueChange *find_change(GArray *changes, fstHandle handle)
{
    for (guint i = 0; i < changes->len; i++) {
        const ValueChange *change = &g_array_index(changes, ValueChange, i);

        if (change->handle == handle) {
            return change;
        }
    }

    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("value_before_first_time_change.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstHandle before = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "before", 0);
    fstHandle after = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "after", 0);

    fstWriterEmitValueChange(writer, before, "00000001"); /* no time change emitted yet */
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, after, "00000010");

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("value_before_first_time_change.fst");
    g_assert_true(reader != NULL);

    // fstReaderGetValueFromHandleAtTime() reads the frame and reports both
    // values

    char buffer[64];
    g_assert_cmpstr(fstReaderGetValueFromHandleAtTime(reader, 0, before, buffer), ==, "00000001");
    g_assert_cmpstr(fstReaderGetValueFromHandleAtTime(reader, 0, after, buffer), ==, "00000010");

    // fstReaderIterBlocks2() has to report the same file contents

    GArray *changes = g_array_new(FALSE, FALSE, sizeof(ValueChange));

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocks2(reader, value_change_callback, NULL, changes, NULL);
    fstReaderClose(reader);

    const ValueChange *change = find_change(changes, after);
    g_assert_nonnull(change);
    g_assert_cmpint(change->time, ==, 0);
    g_assert_cmpstr(change->value, ==, "00000010");

    change = find_change(changes, before);
    g_assert_nonnull(change);
    g_assert_cmpint(change->time, ==, 0);
    g_assert_cmpstr(change->value, ==, "00000001");

    g_array_free(changes, TRUE);

    return 0;
}

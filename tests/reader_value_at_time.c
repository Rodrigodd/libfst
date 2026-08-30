#include <fstapi.h>
#include <glib.h>
#include <string.h>

// fstReaderGetValueFromHandleAtTime() answers from the frame at the start of
// the enclosing value change section when the queried time predates any change
// in that section.

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

    fstWriterContext *writer = fstWriterCreate("value_at_time.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle bit = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "bit", 0);
    fstHandle bus = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 8, "bus", 0);
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, bit, "0");
    fstWriterEmitValueChange(writer, bus, "00001111");

    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitValueChange(writer, bit, "1");

    // fstWriterFlushContext() only queues anything once the time chain holds
    // more than one entry, so a third time change is needed before it.

    fstWriterEmitTimeChange(writer, 10);

    // The flush is only queued here, the next time change honours it, so
    // everything from time 20 onwards lands in a second section whose frame
    // holds the values established above.

    fstWriterFlushContext(writer);
    g_assert_cmpint(fstWriterGetFlushContextPending(writer), ==, 1);

    fstWriterEmitTimeChange(writer, 20);

    fstWriterEmitTimeChange(writer, 25);
    fstWriterEmitValueChange(writer, bus, "11110000");

    fstWriterEmitTimeChange(writer, 30);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("value_at_time.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetValueChangeSectionCount(reader), >=, 2);

    // Inside the first section.

    expect_value(reader, 0, bit, "0");
    expect_value(reader, 0, bus, "00001111");
    expect_value(reader, 4, bit, "0");
    expect_value(reader, 5, bit, "1");
    expect_value(reader, 10, bit, "1");

    // Time 20 belongs to the second section but predates every change in it,
    // so the values come from that section's frame.

    expect_value(reader, 20, bit, "1");
    expect_value(reader, 20, bus, "00001111");

    // After the changes in the second section.

    expect_value(reader, 25, bus, "11110000");
    expect_value(reader, 30, bit, "1");

    // A time past the end of the dump has no value at all, it is not clamped
    // to the last one.

    char buffer[256];
    g_assert_true(fstReaderGetValueFromHandleAtTime(reader, 31, bit, buffer) == NULL);
    g_assert_true(fstReaderGetValueFromHandleAtTime(reader, 1000, bit, buffer) == NULL);

    fstReaderClose(reader);

    return 0;
}

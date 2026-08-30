/*
 * fstReaderGetValueFromHandleAtTime() formats a real variable the same way
 * whether the value comes from a value change or from the frame at the start
 * of a value change section.
 */

#include <fstapi.h>
#include <glib.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    double first = 1.25;
    double second = -3.5;

    fstWriterContext *writer = fstWriterCreate("real_frame.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle real = fstWriterCreateVar(writer, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 8, "real", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, real, &first);

    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitTimeChange(writer, 10);

    // The queued flush is honoured by the next time change, so time 20 onwards
    // lands in a second section whose frame holds the value written above.

    fstWriterFlushContext(writer);

    fstWriterEmitTimeChange(writer, 20);

    fstWriterEmitTimeChange(writer, 25);
    fstWriterEmitValueChange(writer, real, &second);

    fstWriterEmitTimeChange(writer, 30);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("real_frame.fst");
    g_assert_true(reader != NULL);
    g_assert_cmpint(fstReaderGetValueChangeSectionCount(reader), >=, 2);

    char buffer[256];

    // Time 0 and 25 are value changes, time 20 predates every change in its
    // section so it is served from that section's frame.  All three are the
    // same variable read through the same entry point and must be formatted
    // the same way.

    char *at_change = fstReaderGetValueFromHandleAtTime(reader, 0, real, buffer);
    g_assert_true(at_change != NULL);
    g_assert_cmpstr(at_change, ==, "r1.25");

    char *from_frame = fstReaderGetValueFromHandleAtTime(reader, 20, real, buffer);
    g_assert_true(from_frame != NULL);
    g_assert_cmpstr(from_frame, ==, "r1.25");

    char *after_change = fstReaderGetValueFromHandleAtTime(reader, 25, real, buffer);
    g_assert_true(after_change != NULL);
    g_assert_cmpstr(after_change, ==, "r-3.5");

    fstReaderClose(reader);

    return 0;
}

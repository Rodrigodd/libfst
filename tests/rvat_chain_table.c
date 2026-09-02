#include <fstapi.h>
#include <glib.h>

// The chain table at the end of a value change section is a varint stream, one
// entry per handle, and the readers walk it until the pointer reaches the end
// of the buffer.  A stream whose last entry does not land on the end exactly
// has to stop all the same.

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("rvat_chain.fst", 0);
    g_assert_true(writer != NULL);

    fstWriterSetPackType(writer, FST_WR_PT_LZ4);
    fstWriterSetRepackOnClose(writer, 1);

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 1, "flag", 0);

    // The flush closes a section that holds only time changes, so the section
    // the value change lands in starts with a chain table of its own.

    fstWriterEmitTimeChange(writer, 255);
    fstWriterEmitTimeChange(writer, 432);
    fstWriterEmitTimeChange(writer, 505);

    fstWriterFlushContext(writer);

    fstWriterEmitValueChange(writer, var, "-");

    fstWriterEmitTimeChange(writer, 760);
    fstWriterEmitTimeChange(writer, 766);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("rvat_chain.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetMaxHandle(reader), ==, 1);

    // The random access reader parses the chain table of the section holding
    // the requested time.

    char buf[64];
    char *value = fstReaderGetValueFromHandleAtTime(reader, 505, var, buf);

    g_assert_true(value != NULL);
    g_assert_cmpstr(value, ==, "-");

    fstReaderClose(reader);

    return 0;
}

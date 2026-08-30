#include <fstapi.h>
#include <glib.h>
#include <string.h>

// Timescale strings are parsed by scanning for the first unit character, with
// a leading 10 or 100 bumping the exponent.

static void check_timescale(const char *spec, int expected_exponent)
{
    fstWriterContext *writer = fstWriterCreate("timescale.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetTimescaleFromString(writer, spec);

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "v", 0);
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "0");
    fstWriterEmitTimeChange(writer, 1);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("timescale.fst");
    g_assert_true(reader != NULL);
    g_assert_cmpint(fstReaderGetTimescale(reader), ==, expected_exponent);
    fstReaderClose(reader);
}

static void test_timescale_strings(void)
{
    check_timescale("1s", 0);
    check_timescale("1ms", -3);
    check_timescale("1us", -6);
    check_timescale("1ns", -9);
    check_timescale("1ps", -12);
    check_timescale("1fs", -15);
    check_timescale("1as", -18);
    check_timescale("1zs", -21);

    check_timescale("10ns", -8);
    check_timescale("100ns", -7);

    // Without a recognised unit the default of nanoseconds is kept.

    check_timescale("1", -9);
}

// The writer status getters and the less common variable creation entry point.

static void test_writer_status(void)
{
    fstWriterContext *writer = fstWriterCreate("status.fst", 1);
    g_assert_true(writer != NULL);

    g_assert_cmpint(fstWriterGetFseekFailed(writer), ==, 0);
    g_assert_cmpint(fstWriterGetDumpSizeLimitReached(writer), ==, 0);
    g_assert_cmpint(fstWriterGetFlushContextPending(writer), ==, 0);

    fstWriterSetDumpSizeLimit(writer, 1024 * 1024);
    g_assert_cmpint(fstWriterGetDumpSizeLimitReached(writer), ==, 0);

    fstWriterSetValueList(writer, "a value list");

    // A path that really exists so that the realpath lookup succeeds.

    fstWriterSetSourceStem(writer, ".", 1, 1);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    fstHandle var = fstWriterCreateVar2(writer,
                                        FST_VT_VCD_WIRE,
                                        FST_VD_IMPLICIT,
                                        1,
                                        "v",
                                        0,
                                        "std_logic",
                                        FST_SVT_VHDL_SIGNAL,
                                        FST_SDT_VHDL_STD_LOGIC);
    g_assert_cmpint(var, ==, 1);

    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "0");
    fstWriterEmitTimeChange(writer, 1);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("status.fst");
    g_assert_true(reader != NULL);

    gboolean saw_valuelist = FALSE;
    gboolean saw_supvar = FALSE;
    gboolean saw_pathname = FALSE;
    struct fstHier *hier;

    while ((hier = fstReaderIterateHier(reader)) != NULL) {
        if (hier->htyp == FST_HT_VAR) {
            g_assert_cmpstr(hier->u.var.name, ==, "v");
        } else if (hier->htyp == FST_HT_ATTRBEGIN && hier->u.attr.typ == FST_AT_MISC) {
            switch (hier->u.attr.subtype) {
                case FST_MT_VALUELIST:
                    g_assert_cmpstr(hier->u.attr.name, ==, "a value list");
                    saw_valuelist = TRUE;
                    break;
                case FST_MT_SUPVAR:
                    saw_supvar = TRUE;
                    break;
                case FST_MT_PATHNAME:
                    // The stem was resolved with realpath(), so it must have
                    // become absolute.
                    g_assert_cmpint(hier->u.attr.name[0], ==, '/');
                    saw_pathname = TRUE;
                    break;
                default:
                    break;
            }
        }
    }

    g_assert_true(saw_valuelist);
    g_assert_true(saw_supvar);
    g_assert_true(saw_pathname);

    fstReaderClose(reader);
}

// fstWriterEmitDumpActive() records a blackout chain that the reader exposes
// through its own getters.

static void test_dump_activity(void)
{
    fstWriterContext *writer = fstWriterCreate("activity.fst", 1);
    g_assert_true(writer != NULL);

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "v", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "0");

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitDumpActive(writer, 0);

    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitDumpActive(writer, 1);

    fstWriterEmitTimeChange(writer, 30);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("activity.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetNumberDumpActivityChanges(reader), ==, 2);

    g_assert_cmpint(fstReaderGetDumpActivityChangeTime(reader, 0), ==, 10);
    g_assert_cmpint(fstReaderGetDumpActivityChangeValue(reader, 0), ==, 0);

    g_assert_cmpint(fstReaderGetDumpActivityChangeTime(reader, 1), ==, 20);
    g_assert_cmpint(fstReaderGetDumpActivityChangeValue(reader, 1), ==, 1);

    // Out of range indices report zero instead of reading past the array.

    g_assert_cmpint(fstReaderGetDumpActivityChangeTime(reader, 2), ==, 0);
    g_assert_cmpint(fstReaderGetDumpActivityChangeValue(reader, 2), ==, 0);

    fstReaderClose(reader);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_timescale_strings();
    test_writer_status();
    test_dump_activity();

    return 0;
}

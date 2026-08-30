#include <fstapi.h>
#include <glib.h>
#include <string.h>

// The header fields the writer patches in place after the fact, read back
// through the corresponding reader getters.

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("metadata.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetPackType(writer, FST_WR_PT_LZ4);
    fstWriterSetRepackOnClose(writer, 1);
    fstWriterSetParallelMode(writer, 0);

    fstWriterSetDate(writer, "Mon Jan  1 00:00:00 2024");
    fstWriterSetVersion(writer, "libfst test suite");
    fstWriterSetFileType(writer, FST_FT_VERILOG);
    fstWriterSetTimezero(writer, -42);
    fstWriterSetTimescaleFromString(writer, "1ps");

    fstWriterSetComment(writer, "a comment\nwith a newline");
    fstWriterSetEnvVar(writer, "LIBFST_TEST=1");

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "var", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "0");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, var, "1");
    fstWriterEmitTimeChange(writer, 20);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("metadata.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpstr(fstReaderGetVersionString(reader), ==, "libfst test suite");
    g_assert_cmpstr(fstReaderGetDateString(reader), ==, "Mon Jan  1 00:00:00 2024");
    g_assert_cmpint(fstReaderGetFileType(reader), ==, FST_FT_VERILOG);
    g_assert_cmpint(fstReaderGetTimezero(reader), ==, -42);
    g_assert_cmpint(fstReaderGetTimescale(reader), ==, -12);

    g_assert_cmpint(fstReaderGetStartTime(reader), ==, 0);
    g_assert_cmpint(fstReaderGetEndTime(reader), ==, 20);
    g_assert_cmpint(fstReaderGetMaxHandle(reader), ==, 1);
    g_assert_cmpint(fstReaderGetValueChangeSectionCount(reader), ==, 1);
    g_assert_cmpint(fstReaderGetMemoryUsedByWriter(reader), >=, 0);
    g_assert_cmpint(fstReaderGetDoubleEndianMatchState(reader), ==, 1);
    g_assert_cmpint(fstReaderGetFseekFailed(reader), ==, 0);

    // The comment and the environment variable are stored as hierarchy
    // attributes rather than header fields.

    gboolean saw_comment = FALSE;
    gboolean saw_envvar = FALSE;
    struct fstHier *hier;

    while ((hier = fstReaderIterateHier(reader)) != NULL) {
        if (hier->htyp != FST_HT_ATTRBEGIN || hier->u.attr.typ != FST_AT_MISC) {
            continue;
        }
        if (hier->u.attr.subtype == FST_MT_COMMENT) {
            // Newlines are flattened to spaces so that they cannot break the
            // hierarchy encoding.
            g_assert_cmpstr(hier->u.attr.name, ==, "a comment with a newline");
            saw_comment = TRUE;
        } else if (hier->u.attr.subtype == FST_MT_ENVVAR) {
            g_assert_cmpstr(hier->u.attr.name, ==, "LIBFST_TEST=1");
            saw_envvar = TRUE;
        }
    }

    g_assert_true(saw_comment);
    g_assert_true(saw_envvar);

    fstReaderClose(reader);

    return 0;
}

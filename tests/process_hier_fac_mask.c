/*
 * The facility process mask survives fstReaderProcessHier(), so a mask set
 * before it still governs the value changes fstReaderIterBlocks() writes.
 */

#include <fstapi.h>
#include <glib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("process_hier.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetTimescaleFromString(writer, "1ns");
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    fstHandle clk = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstHandle bus = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 4, "bus", 0);

    fstWriterSetUpscope(writer);

    for (uint64_t t = 0; t < 4; t++) {
        fstWriterEmitTimeChange(writer, t);
        fstWriterEmitValueChange(writer, clk, (t & 1) ? "1" : "0");
        fstWriterEmitValueChange(writer, bus, (t & 1) ? "1010" : "0101");
    }

    fstWriterEmitTimeChange(writer, 4);
    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("process_hier.fst");
    g_assert_true(reader != NULL);

    FILE *vcd = fopen("process_hier.vcd", "wb");
    g_assert_true(vcd != NULL);

    fstReaderSetFacProcessMaskAll(reader);

    g_assert_cmpint(fstReaderProcessHier(reader, vcd), ==, 1);

    // The mask was set above and nothing cleared it, so every facility must
    // still be enabled.

    g_assert_cmpint(fstReaderGetFacProcessMask(reader, clk), !=, 0);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, bus), !=, 0);

    g_assert_cmpint(fstReaderIterBlocks(reader, NULL, NULL, vcd), ==, 1);

    fclose(vcd);
    fstReaderClose(reader);

    gchar *contents = NULL;
    GError *error = NULL;
    g_assert_true(g_file_get_contents("process_hier.vcd", &contents, NULL, &error));
    g_assert_no_error(error);

    // The hierarchy is written whatever the mask says.

    g_assert_true(strstr(contents, "$scope module top $end") != NULL);
    g_assert_true(strstr(contents, "$enddefinitions $end") != NULL);

    // The value changes are what the mask governs.

    g_assert_true(strstr(contents, "b1010 ") != NULL);
    g_assert_true(strstr(contents, "b0101 ") != NULL);

    g_free(contents);

    return 0;
}

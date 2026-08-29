/*
 * A file whose hierarchy cannot be read is either rejected by fstReaderOpen()
 * or handed back as a usable context, never as one that reports variables it
 * cannot serve.  The case covered here is a file truncated after its header
 * block.
 */

#include <fstapi.h>
#include <glib.h>
#include <stdio.h>

/* Copy just the first block of an FST file, i.e. the header, into a new file */
static void copy_header_block_only(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    g_assert_true(in != NULL);

    /* one type byte plus a 64 bit big endian length that covers itself */
    int type = fgetc(in);
    g_assert_cmpint(type, ==, FST_BL_HDR);

    uint64_t length = 0;
    for (int i = 0; i < 8; i++) {
        int c = fgetc(in);
        g_assert_cmpint(c, !=, EOF);
        length = (length << 8) | (unsigned char)c;
    }

    g_assert_cmpint(fseek(in, 0, SEEK_SET), ==, 0);

    FILE *out = fopen(to, "wb");
    g_assert_true(out != NULL);

    for (uint64_t i = 0; i < length + 1; i++) {
        int c = fgetc(in);
        g_assert_cmpint(c, !=, EOF);
        fputc(c, out);
    }

    fclose(out);
    fclose(in);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("reader_truncated_full.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "var", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "00000001");
    fstWriterEmitTimeChange(writer, 1);

    fstWriterClose(writer);

    copy_header_block_only("reader_truncated_full.fst", "reader_truncated.fst");

    fstReaderContext *reader = fstReaderOpen("reader_truncated.fst");

    if (reader != NULL) {
        // Opening succeeded, so the context has to be usable, even though
        // the hierarchy went away with the truncated blocks.

        g_assert_true(fstReaderIterateHier(reader) != NULL);

        // The mask functions have to work on any context that opened.

        fstReaderSetFacProcessMaskAll(reader);
        fstReaderSetFacProcessMask(reader, 1);
        g_assert_cmpint(fstReaderGetFacProcessMask(reader, 1), !=, 0);

        fstReaderClose(reader);
    }

    return 0;
}

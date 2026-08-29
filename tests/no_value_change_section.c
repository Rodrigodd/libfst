/*
 * The hierarchy of a file that has no value change section is still readable
 * through fstReaderIterateHier().
 */

#include <fstapi.h>
#include <glib.h>
#include <stdio.h>

/* Copy an FST file, dropping its value change sections.  A block is one type
 * byte followed by a 64 bit big endian length that includes the eight bytes of
 * the length field itself. */
static void copy_without_value_change_sections(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    g_assert_true(in != NULL);

    FILE *out = fopen(to, "wb");
    g_assert_true(out != NULL);

    g_assert_cmpint(fseek(in, 0, SEEK_END), ==, 0);
    long end = ftell(in);

    long pos = 0;
    int copied = 0;
    int dropped = 0;

    while (pos < end) {
        g_assert_cmpint(fseek(in, pos, SEEK_SET), ==, 0);

        int type = fgetc(in);
        g_assert_cmpint(type, !=, EOF);

        uint64_t length = 0;
        for (int i = 0; i < 8; i++) {
            int c = fgetc(in);
            g_assert_cmpint(c, !=, EOF);
            length = (length << 8) | (unsigned char)c;
        }
        g_assert_cmpint(length, >, 0);

        gboolean is_value_change = (type == FST_BL_VCDATA) ||
                                   (type == FST_BL_VCDATA_DYN_ALIAS) ||
                                   (type == FST_BL_VCDATA_DYN_ALIAS2);

        if (is_value_change) {
            dropped++;
        } else {
            g_assert_cmpint(fseek(in, pos, SEEK_SET), ==, 0);

            for (uint64_t i = 0; i < length + 1; i++) {
                int c = fgetc(in);
                g_assert_cmpint(c, !=, EOF);
                fputc(c, out);
            }

            copied++;
        }

        pos += 1 + (long)length;
    }

    g_assert_cmpint(dropped, ==, 1);
    g_assert_cmpint(copied, >=, 2); /* at least the header and the hierarchy */

    fclose(out);
    fclose(in);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("no_value_change_full.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "var", 0);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "0");
    fstWriterEmitTimeChange(writer, 1);

    fstWriterClose(writer);

    copy_without_value_change_sections("no_value_change_full.fst", "no_value_change.fst");

    fstReaderContext *reader = fstReaderOpen("no_value_change.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetVarCount(reader), ==, 1);
    g_assert_cmpint(fstReaderGetValueChangeSectionCount(reader), ==, 0);

    // The hierarchy is still in the file and has to be readable

    struct fstHier *iter = fstReaderIterateHier(reader);
    g_assert_true(iter != NULL);
    g_assert_cmpint(iter->htyp, ==, FST_HT_VAR);
    g_assert_cmpstr(iter->u.var.name, ==, "var");
    g_assert_cmpint(iter->u.var.length, ==, 1);

    iter = fstReaderIterateHier(reader);
    g_assert_true(iter == NULL);

    fstReaderClose(reader);

    return 0;
}

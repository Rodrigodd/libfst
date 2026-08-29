/*
 * A value change section that ends up holding no value change leaves no block
 * behind, so the hierarchy and geometry blocks written after it stay
 * reachable.  Covers an empty first section and an empty later one.
 */

#include <fstapi.h>
#include <glib.h>
#include <stdio.h>

/* Walk the block list, looking for a block a reader cannot advance past.  A
 * block is one type byte followed by a 64 bit big endian length that includes
 * the eight bytes of the length field itself. */
static gboolean has_zero_length_block(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    g_assert_true(f != NULL);

    g_assert_cmpint(fseek(f, 0, SEEK_END), ==, 0);
    long end = ftell(f);

    long pos = 0;
    gboolean found = FALSE;

    while (pos < end) {
        g_assert_cmpint(fseek(f, pos, SEEK_SET), ==, 0);

        int type = fgetc(f);
        g_assert_cmpint(type, !=, EOF);

        uint64_t length = 0;
        for (int i = 0; i < 8; i++) {
            int c = fgetc(f);
            g_assert_cmpint(c, !=, EOF);
            length = (length << 8) | (unsigned char)c;
        }

        if (length == 0) {
            g_printerr("# zero length block of type %d at offset %ld of %s\n", type, pos, filename);
            found = TRUE;
            break;
        }

        pos += 1 + (long)length;
    }

    fclose(f);

    return found;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // An empty section that is not the first one: the first section holds a
    // value change, the last time change cuts a second one that stays empty

    fstWriterContext *writer = fstWriterCreate("empty_section_second.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "var", 0);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, var, "00000001");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitTimeChange(writer, 30);
    fstWriterFlushContext(writer);
    fstWriterEmitTimeChange(writer, 40); /* cuts the section; the new one stays empty */

    fstWriterClose(writer);

    g_assert_false(has_zero_length_block("empty_section_second.fst"));

    fstReaderContext *reader = fstReaderOpen("empty_section_second.fst");
    g_assert_true(reader != NULL);

    struct fstHier *iter = fstReaderIterateHier(reader);
    g_assert_true(iter != NULL);
    g_assert_cmpint(iter->htyp, ==, FST_HT_VAR);
    g_assert_cmpstr(iter->u.var.name, ==, "var");

    fstReaderClose(reader);

    // The same shape with the empty section first

    writer = fstWriterCreate("empty_section_first.fst", 1);
    g_assert_true(writer != NULL);
    fstWriterSetTimescaleFromString(writer, "1ns");

    fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_OUTPUT, 8, "var", 0);

    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitTimeChange(writer, 30);

    fstWriterClose(writer);

    g_assert_false(has_zero_length_block("empty_section_first.fst"));

    // The blocks written after the section header have to stay reachable

    reader = fstReaderOpen("empty_section_first.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetVarCount(reader), ==, 1);

    iter = fstReaderIterateHier(reader);
    g_assert_true(iter != NULL);
    g_assert_cmpint(iter->htyp, ==, FST_HT_VAR);
    g_assert_cmpstr(iter->u.var.name, ==, "var");

    fstReaderClose(reader);

    return 0;
}

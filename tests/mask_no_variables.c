#include <fstapi.h>
#include <glib.h>

// A file with no variables still opens, and the facility process mask
// functions still have to be callable on it.  The mask is allocated either when
// the geometry block is read or by the fstReaderProcessHier() call at the end
// of the reader init, and a file with no variables has no geometry block and no
// hierarchy for that call to recover, so it is left with no mask at all.

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("mask_no_vars.fst", 1);
    g_assert_true(writer != NULL);

    // The pack type reaches the hierarchy block as well, and it is the LZ4
    // packed one of a file with no variables that cannot be recovered.

    fstWriterSetPackType(writer, FST_WR_PT_LZ4);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitTimeChange(writer, 1);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("mask_no_vars.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetMaxHandle(reader), ==, 0);

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderClrFacProcessMaskAll(reader);

    // There is no handle to set, and no handle reads back as set.

    fstReaderSetFacProcessMask(reader, 1);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, 1), ==, 0);
    fstReaderClrFacProcessMask(reader, 1);

    // Iterating the value changes of such a file consults the mask as well.

    g_assert_cmpint(fstReaderIterBlocks2(reader, NULL, NULL, NULL, NULL), !=, 0);

    fstReaderClose(reader);

    return 0;
}

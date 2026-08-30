#include <fstapi.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    int scalar_changes;
    int varlen_changes;
    uint64_t last_time;
    fstHandle only_handle; // when non-zero, no other handle may be reported
} Collected;

static void on_value_change(void *user, uint64_t time, fstHandle facidx, const unsigned char *value)
{
    Collected *collected = user;

    g_assert_true(value != NULL);
    if (collected->only_handle != 0) {
        g_assert_cmpint(facidx, ==, collected->only_handle);
    }

    collected->scalar_changes++;
    collected->last_time = time;
}

static void on_varlen_change(void *user,
                             uint64_t time,
                             fstHandle facidx,
                             const unsigned char *value,
                             uint32_t len)
{
    Collected *collected = user;

    (void)facidx;
    g_assert_true(value != NULL);
    g_assert_cmpint(len, ==, strlen("payload"));
    g_assert_cmpint(memcmp(value, "payload", len), ==, 0);

    collected->varlen_changes++;
    collected->last_time = time;
}

static void write_file(void)
{
    fstWriterContext *writer = fstWriterCreate("iter_blocks.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    fstHandle bit = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "bit", 0);
    fstHandle real = fstWriterCreateVar(writer, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 8, "real", 0);
    fstHandle str = fstWriterCreateVar(writer, FST_VT_GEN_STRING, FST_VD_IMPLICIT, 0, "str", 0);

    fstWriterSetUpscope(writer);

    double sample = 1.5;

    for (uint64_t t = 0; t < 6; t++) {
        fstWriterEmitTimeChange(writer, t);
        fstWriterEmitValueChange(writer, bit, (t & 1) ? "1" : "0");
        fstWriterEmitValueChange(writer, real, &sample);
        fstWriterEmitVariableLengthValueChange(writer, str, "payload", strlen("payload"));

        if (t == 2) {
            // Force a second value change section.
            fstWriterFlushContext(writer);
        }
    }

    fstWriterEmitTimeChange(writer, 6);
    fstWriterClose(writer);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    write_file();

    fstReaderContext *reader = fstReaderOpen("iter_blocks.fst");
    g_assert_true(reader != NULL);
    g_assert_cmpint(fstReaderGetValueChangeSectionCount(reader), >=, 2);

    fstHandle max_handle = fstReaderGetMaxHandle(reader);
    g_assert_cmpint(max_handle, ==, 3);

    // No facility is processed until the caller opts in.

    for (fstHandle h = 1; h <= max_handle; h++) {
        g_assert_cmpint(fstReaderGetFacProcessMask(reader, h), ==, 0);
    }

    fstReaderSetFacProcessMaskAll(reader);
    for (fstHandle h = 1; h <= max_handle; h++) {
        g_assert_cmpint(fstReaderGetFacProcessMask(reader, h), !=, 0);
    }

    // Out of range handles read back as disabled rather than crashing.

    g_assert_cmpint(fstReaderGetFacProcessMask(reader, 0), ==, 0);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, max_handle + 1), ==, 0);

    // Restricting the mask to a single facility must silence the others.

    fstReaderClrFacProcessMaskAll(reader);
    for (fstHandle h = 1; h <= max_handle; h++) {
        g_assert_cmpint(fstReaderGetFacProcessMask(reader, h), ==, 0);
    }

    fstReaderSetFacProcessMask(reader, 1);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, 1), !=, 0);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, 2), ==, 0);

    Collected collected = {0};
    collected.only_handle = 1;

    g_assert_cmpint(fstReaderIterBlocks(reader, on_value_change, &collected, NULL), ==, 1);
    g_assert_cmpint(collected.scalar_changes, ==, 6);
    g_assert_cmpint(collected.last_time, ==, 5);

    // Clearing that one facility again leaves nothing to report.

    fstReaderClrFacProcessMask(reader, 1);
    g_assert_cmpint(fstReaderGetFacProcessMask(reader, 1), ==, 0);

    Collected empty = {0};
    g_assert_cmpint(fstReaderIterBlocks(reader, on_value_change, &empty, NULL), ==, 1);
    g_assert_cmpint(empty.scalar_changes, ==, 0);

    // With every facility enabled the variable length variable is reported
    // through the second callback of fstReaderIterBlocks2().

    fstReaderSetFacProcessMaskAll(reader);
    fstReaderIterBlocksSetNativeDoublesOnCallback(reader, 1);

    Collected all = {0};
    g_assert_cmpint(fstReaderIterBlocks2(reader, on_value_change, on_varlen_change, &all, NULL),
                    ==,
                    1);
    g_assert_cmpint(all.varlen_changes, ==, 6);
    g_assert_cmpint(all.scalar_changes, ==, 12); // bit and real, six times each

    // A limited time range skips the value change sections that fall outside
    // it, the file was written with a flush in the middle so that there is
    // more than one section to skip.

    fstReaderSetLimitTimeRange(reader, 0, 1);

    Collected limited = {0};
    g_assert_cmpint(fstReaderIterBlocks2(reader, on_value_change, on_varlen_change, &limited, NULL),
                    ==,
                    1);
    g_assert_cmpint(limited.scalar_changes, >, 0);
    g_assert_cmpint(limited.scalar_changes, <, all.scalar_changes);
    g_assert_cmpint(limited.last_time, <=, 2);

    fstReaderSetUnlimitedTimeRange(reader);

    Collected unlimited = {0};
    g_assert_cmpint(
        fstReaderIterBlocks2(reader, on_value_change, on_varlen_change, &unlimited, NULL),
        ==,
        1);
    g_assert_cmpint(unlimited.scalar_changes, ==, all.scalar_changes);

    fstReaderClose(reader);

    return 0;
}

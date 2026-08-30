#include <fstapi.h>
#include <glib.h>
#include <string.h>

// Enum tables are encoded into a space separated attribute string by the
// writer and decoded again by fstUtilityExtractEnumTableFromString().

static const char *literals[] = {"IDLE", "RUN", "needs escaping\t", "DONE"};
static const char *values[] = {"00", "01", "10", "11"};
#define ELEM_COUNT (sizeof(literals) / sizeof(literals[0]))

static void check_table(const char *encoded)
{
    struct fstETab *table = fstUtilityExtractEnumTableFromString(encoded);
    g_assert_true(table != NULL);

    g_assert_cmpstr(table->name, ==, "state_t");
    g_assert_cmpint(table->elem_count, ==, ELEM_COUNT);

    for (unsigned int i = 0; i < ELEM_COUNT; i++) {
        g_assert_cmpstr(table->literal_arr[i], ==, literals[i]);
        g_assert_cmpstr(table->val_arr[i], ==, values[i]);
    }

    fstUtilityFreeEnumTable(table);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("enum_table.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    // min_valbits pads the values out with leading zeroes.

    fstEnumHandle table =
        fstWriterCreateEnumTable(writer, "state_t", ELEM_COUNT, 2, literals, values);
    g_assert_cmpint(table, ==, 1);

    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_IMPLICIT, 2, "state", 0);

    // Referencing the table again only emits the handle, not the payload.

    fstWriterEmitEnumTableRef(writer, table);

    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "00");
    fstWriterEmitTimeChange(writer, 1);
    fstWriterEmitValueChange(writer, var, "11");

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("enum_table.fst");
    g_assert_true(reader != NULL);

    int definitions = 0;
    int references = 0;
    struct fstHier *hier;

    while ((hier = fstReaderIterateHier(reader)) != NULL) {
        if (hier->htyp != FST_HT_ATTRBEGIN || hier->u.attr.typ != FST_AT_MISC ||
            hier->u.attr.subtype != FST_MT_ENUMTABLE) {
            continue;
        }

        g_assert_cmpint(hier->u.attr.arg, ==, table);

        if (hier->u.attr.name_length != 0) {
            check_table(hier->u.attr.name);
            definitions++;
        } else {
            references++;
        }
    }

    g_assert_cmpint(definitions, ==, 1);
    g_assert_cmpint(references, ==, 1);

    fstReaderClose(reader);

    // A string whose element count does not match the number of separators is
    // rejected rather than parsed into a malformed table.

    g_assert_true(fstUtilityExtractEnumTableFromString("bad 3 a b 0 1") == NULL);
    g_assert_true(fstUtilityExtractEnumTableFromString(NULL) == NULL);

    // Freeing a NULL table is a no-op.

    fstUtilityFreeEnumTable(NULL);

    // Utilities-only readers exist for decoding enum tables without a file.

    fstReaderContext *utilities = fstReaderOpenForUtilitiesOnly();
    g_assert_true(utilities != NULL);
    fstReaderClose(utilities);

    return 0;
}

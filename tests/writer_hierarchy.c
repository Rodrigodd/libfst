#include <fstapi.h>
#include <glib.h>
#include <string.h>

// Scopes, aliases, attributes and source stems, written and then read back
// through the hierarchy iterator.

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fstWriterContext *writer = fstWriterCreate("hierarchy.fst", 1);
    g_assert_true(writer != NULL);

    fstWriterSetTimescale(writer, -9);

    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", NULL);

    // A source stem interns the path, so emitting the same path twice must
    // only produce a single FST_MT_PATHNAME attribute.

    fstWriterSetSourceStem(writer, "top.v", 1, 0);
    fstWriterSetSourceInstantiationStem(writer, "top.v", 2, 0);

    fstWriterSetAttrBegin(writer, FST_AT_MISC, FST_MT_UNKNOWN, "custom", 7);
    fstHandle var = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_INPUT, 8, "bus", 0);
    fstWriterSetAttrEnd(writer);

    // An alias shares the handle of the variable it aliases.

    fstHandle alias =
        fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_INPUT, 8, "bus_alias", var);
    g_assert_cmpint(alias, ==, var);

    fstWriterSetScope(writer, FST_ST_VCD_BEGIN, "inner", "component");
    fstHandle inner = fstWriterCreateVar(writer, FST_VT_VCD_INTEGER, FST_VD_OUTPUT, 32, "count", 0);
    fstWriterSetUpscope(writer);

    fstWriterSetUpscope(writer);

    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, var, "00000000");
    fstWriterEmitValueChange32(writer, inner, 32, 0);
    fstWriterEmitTimeChange(writer, 1);
    fstWriterEmitValueChange32(writer, inner, 32, 1);

    fstWriterClose(writer);

    fstReaderContext *reader = fstReaderOpen("hierarchy.fst");
    g_assert_true(reader != NULL);

    g_assert_cmpint(fstReaderGetVarCount(reader), ==, 3);
    g_assert_cmpint(fstReaderGetScopeCount(reader), ==, 2);
    g_assert_cmpint(fstReaderGetAliasCount(reader), ==, 1);

    int scopes = 0;
    int upscopes = 0;
    int user_info[2];
    int pathnames = 0;
    int sourcestems = 0;
    int istems = 0;
    gboolean saw_custom = FALSE;
    gboolean saw_alias = FALSE;
    struct fstHier *hier;

    while ((hier = fstReaderIterateHier(reader)) != NULL) {
        switch (hier->htyp) {
            case FST_HT_SCOPE:
                // The flat scope name is maintained by the caller, the
                // hierarchy iterator only reports the scope records.
                fstReaderPushScope(reader, hier->u.scope.name, &user_info[scopes]);

                if (scopes == 0) {
                    g_assert_cmpint(hier->u.scope.typ, ==, FST_ST_VCD_MODULE);
                    g_assert_cmpstr(hier->u.scope.name, ==, "top");
                    g_assert_cmpint(hier->u.scope.name_length, ==, 3);
                    g_assert_cmpstr(fstReaderGetCurrentFlatScope(reader), ==, "top");
                    g_assert_cmpint(fstReaderGetCurrentScopeLen(reader), ==, 3);
                } else {
                    g_assert_cmpint(hier->u.scope.typ, ==, FST_ST_VCD_BEGIN);
                    g_assert_cmpstr(hier->u.scope.name, ==, "inner");
                    g_assert_cmpstr(hier->u.scope.component, ==, "component");
                    g_assert_cmpstr(fstReaderGetCurrentFlatScope(reader), ==, "top.inner");
                    g_assert_cmpint(fstReaderGetCurrentScopeLen(reader), ==, 9);
                }

                g_assert_true(fstReaderGetCurrentScopeUserInfo(reader) == &user_info[scopes]);
                scopes++;
                break;

            case FST_HT_UPSCOPE:
                g_assert_true(fstReaderPopScope(reader) != NULL);
                upscopes++;
                break;

            case FST_HT_VAR:
                if (strcmp(hier->u.var.name, "bus_alias") == 0) {
                    g_assert_true(hier->u.var.is_alias);
                    g_assert_cmpint(hier->u.var.handle, ==, var);
                    saw_alias = TRUE;
                } else if (strcmp(hier->u.var.name, "bus") == 0) {
                    g_assert_false(hier->u.var.is_alias);
                    g_assert_cmpint(hier->u.var.length, ==, 8);
                    g_assert_cmpint(hier->u.var.direction, ==, FST_VD_INPUT);
                }
                break;

            case FST_HT_ATTRBEGIN:
                g_assert_cmpint(hier->u.attr.typ, ==, FST_AT_MISC);
                switch (hier->u.attr.subtype) {
                    case FST_MT_PATHNAME:
                        g_assert_cmpstr(hier->u.attr.name, ==, "top.v");
                        pathnames++;
                        break;
                    case FST_MT_SOURCESTEM:
                        g_assert_cmpint(hier->u.attr.arg, ==, 1);
                        sourcestems++;
                        break;
                    case FST_MT_SOURCEISTEM:
                        g_assert_cmpint(hier->u.attr.arg, ==, 2);
                        istems++;
                        break;
                    case FST_MT_UNKNOWN:
                        g_assert_cmpstr(hier->u.attr.name, ==, "custom");
                        g_assert_cmpint(hier->u.attr.arg, ==, 7);
                        saw_custom = TRUE;
                        break;
                    default:
                        break;
                }
                break;

            default:
                break;
        }
    }

    g_assert_cmpint(scopes, ==, 2);
    g_assert_cmpint(upscopes, ==, 2);
    g_assert_cmpstr(fstReaderGetCurrentFlatScope(reader), ==, "");
    g_assert_true(fstReaderGetCurrentScopeUserInfo(reader) == NULL);
    g_assert_cmpint(fstReaderGetCurrentScopeLen(reader), ==, 0);
    g_assert_cmpint(pathnames, ==, 1);
    g_assert_cmpint(sourcestems, ==, 1);
    g_assert_cmpint(istems, ==, 1);
    g_assert_true(saw_custom);
    g_assert_true(saw_alias);

    // Discarding a partially built scope stack must leave the flat name empty.

    fstReaderPushScope(reader, "stale", NULL);
    fstReaderPushScope(reader, "stale2", NULL);
    fstReaderResetScope(reader);
    g_assert_cmpstr(fstReaderGetCurrentFlatScope(reader), ==, "");
    g_assert_true(fstReaderPopScope(reader) == NULL);

    // Rewinding restarts the iterator from the first record.

    g_assert_cmpint(fstReaderIterateHierRewind(reader), ==, 1);
    hier = fstReaderIterateHier(reader);
    g_assert_true(hier != NULL);
    g_assert_cmpint(hier->htyp, ==, FST_HT_SCOPE);
    g_assert_cmpstr(hier->u.scope.name, ==, "top");

    fstReaderClose(reader);

    return 0;
}

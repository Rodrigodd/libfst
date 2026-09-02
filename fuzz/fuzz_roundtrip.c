/*
 * Round trip fuzz target.
 *
 * The input is consumed as a tape of commands: a small configuration block,
 * then a sequence of hierarchy commands, then a sequence of body commands --
 * time changes, value changes and flushes.  Everything the writer is asked to
 * do is recorded in a model, the file is closed, opened with the reader, and
 * every reader entry point is checked against that model:
 *
 *   - the fstReaderIterBlocks2() callback stream,
 *   - fstReaderGetValueFromHandleAtTime(), sampled over the recorded times,
 *   - the fstReaderIterateHier() record stream,
 *   - the header fields and the dump activity list.
 *
 * The model reproduces the quirks of the API rather than an idealized version
 * of it, so that a deviation from a documented quirk is a failure.  They are
 * marked QUIRK below.  On a failure the decoded command sequence is printed as
 * pseudo code, so that a crash artifact can be turned into a test in tests/ by
 * hand.
 *
 * Deliberately out of the generated command space:
 *
 *   - handle 0, and handles above the last one fstWriterCreateVar() returned:
 *     fstWriterEmitValueChange() tests "handle <= maxhandle" only, so handle 0
 *     underflows into an out of bounds index.  That is API misuse rather than
 *     a round trip question.
 *   - two value changes for the same handle at the same time: the writer keeps
 *     both, and nothing pins down which order the reader reports them in, so
 *     the model could not tell a reordering from a correct result.
 *   - time changes that move backwards: the time chain is a delta encoding
 *     with unsigned varints, so a backwards step is not representable.
 *
 * AI-assisted
 */

#include <fstapi.h>

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_VARS 24
#define MAX_HIER 96
#define MAX_BODY 384
#define MAX_CHANGES 384
#define MAX_BLACKOUTS 32
#define MAX_WIDTH 136
#define MAX_VARLEN 48
#define MAX_SCOPE_DEPTH 8
#define MAX_RVAT_SAMPLES 64
#define SCRIPT_SIZE 65536

/* the value characters a bit vector may hold */
static const char vec_chars[] = "01xzhulw-";
#define N_VEC_CHARS (sizeof(vec_chars) - 1)

enum var_kind
{
    VK_BITS,
    VK_REAL,
    VK_STRING
};

/* one signal, that is one handle; several variables may share it by aliasing */
struct sig
{
    enum var_kind kind;
    uint32_t width; /* bits for VK_BITS, 8 for VK_REAL, 0 for VK_STRING */
    int has_change;
    uint64_t last_time; /* time of the last change recorded for this handle */
};

/* one value change, in emission order, which is time sorted */
struct change
{
    uint64_t time;
    fstHandle handle;
    uint32_t len; /* bytes in val */
    double d; /* for VK_REAL */
    unsigned char val[MAX_WIDTH + 1]; /* bit string for VK_BITS, bytes for VK_STRING */
    int delivered;
};

/* one record the hierarchy iterator is expected to return */
struct hrec
{
    int htyp;
    int typ;
    int subtype;
    int direction;
    uint32_t length;
    uint64_t arg;
    fstHandle handle;
    int is_alias;
    int etab; /* index into etab_presets for an enum table definition, else -1 */
    int has_comp;
    char name[96];
    char comp[16];
};

struct etab_preset
{
    const char *name;
    uint32_t elem_count;
    unsigned int min_valbits;
    const char **literals;
    const char **values;
};

static const char *etab_lits_0[] = {"IDLE", "RUN", "DONE"};
static const char *etab_vals_0[] = {"00", "01", "10"};
static const char *etab_lits_1[] = {"OFF", "ON"};
static const char *etab_vals_1[] = {"0", "1"};

/* min_valbits equals every value length, so no zero padding is applied */
static const struct etab_preset etab_presets[] = {
    {"state_t", 3, 2, etab_lits_0, etab_vals_0},
    {"flag_t", 2, 1, etab_lits_1, etab_vals_1},
};
#define N_ETAB_PRESETS ((int)(sizeof(etab_presets) / sizeof(etab_presets[0])))

/* ------------------------------------------------------------------- tape */

static struct
{
    const uint8_t *data;
    size_t len;
    size_t pos;
} tape;

static int tape_eof(void)
{
    return tape.pos >= tape.len;
}

static uint8_t tape_u8(void)
{
    return tape_eof() ? 0 : tape.data[tape.pos++];
}

static uint64_t tape_uint(unsigned int bytes)
{
    uint64_t v = 0;
    unsigned int i;

    for (i = 0; i < bytes; i++) {
        v = (v << 8) | tape_u8();
    }
    return v;
}

/* inclusive range */
static uint32_t tape_range(uint32_t lo, uint32_t hi)
{
    if (lo >= hi) {
        return lo;
    }
    return lo + (uint32_t)(tape_u8() % (hi - lo + 1));
}

/* ------------------------------------------------------------------ model */

static struct sig sigs[MAX_VARS];
static unsigned int n_sigs;

static struct change changes[MAX_CHANGES];
static unsigned int n_changes;
static int changes_dropped; /* the model ran out of room, the value oracles go quiet */

static struct hrec hrecs[MAX_HIER];
static unsigned int n_hrecs;
static int hrecs_dropped;

static struct
{
    uint64_t time;
    unsigned char active;
} blackouts[MAX_BLACKOUTS];
static unsigned int n_blackouts;
static int blackouts_dropped;

static uint64_t model_first_time;
static uint64_t model_cur_time;
static int time_started;
static int limit_hit; /* the dump size limit locked the writer at some point */

static uint64_t model_var_count;
static uint64_t model_scope_count;
static uint64_t model_alias_count;

static int cfg_timescale;
static int64_t cfg_timezero;
static int cfg_filetype;
static int cfg_check_process_hier_mask;

static const char version_string[] = "libfst fuzz roundtrip";
static const char date_string[] = "Mon Jan  1 00:00:00 2024";

static fstWriterContext *writer;

/* ----------------------------------------------------------------- script */

static char script[SCRIPT_SIZE];
static size_t script_len;
static int trace; /* FST_FUZZ_TRACE: echo the script as it is decoded */

static void say(const char *fmt, ...)
{
    va_list ap;
    int n;

    /*
     * A crash inside the library never reaches dump_and_abort(), so echoing
     * each command as it is decoded is the only way to see what led to it.
     */
    if (trace) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
    }

    if (script_len + 1 >= SCRIPT_SIZE) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(script + script_len, SCRIPT_SIZE - script_len, fmt, ap);
    va_end(ap);

    if (n > 0) {
        script_len += (size_t)n;
        if (script_len >= SCRIPT_SIZE) {
            script_len = SCRIPT_SIZE - 1;
        }
    }
}

static void dump_and_abort(const char *fmt, ...)
{
    va_list ap;
    size_t i;

    fflush(stdout);

    fprintf(stderr, "\n=== libfst fuzz roundtrip: the reader and the model disagree ===\n\n");

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n--- decoded command sequence ---\n%s", script);

    fprintf(stderr, "\n--- input bytes (%zu) ---\n", tape.len);
    for (i = 0; i < tape.len; i++) {
        fprintf(stderr, "%02x%s", tape.data[i], ((i % 24) == 23) ? "\n" : " ");
    }
    fprintf(stderr, "\n");
    fflush(stderr);

    abort();
}

/* CHECK reports the condition itself, CHECKF a message explaining it */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            dump_and_abort("%s:%d: failed check: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                          \
    } while (0)

#define CHECKF(cond, ...)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            dump_and_abort(__VA_ARGS__);                                                           \
        }                                                                                          \
    } while (0)

/* ---------------------------------------------------------------- helpers */

static void real_to_string(double d, char *buf, size_t size, int prefix)
{
    if (prefix) {
        snprintf(buf, size, "r%.16g", d);
    } else {
        snprintf(buf, size, "%.16g", d);
    }
}

static struct hrec *push_hrec(int htyp)
{
    struct hrec *h;

    if (n_hrecs >= MAX_HIER) {
        hrecs_dropped = 1;
        return NULL;
    }

    h = &hrecs[n_hrecs++];
    memset(h, 0, sizeof(*h));
    h->htyp = htyp;
    h->etab = -1;
    return h;
}

static int writer_locked(void)
{
    return fstWriterGetDumpSizeLimitReached(writer) != 0;
}

/*
 * QUIRK: a value change emitted before any time change makes the writer emit
 * an implicit time change at zero, so that the value lands in the section that
 * time change opens instead of only in the frame.  Returns 0 when the writer
 * is locked and that implicit time change is dropped along with everything
 * else.
 */
static int model_start_time(void)
{
    if (time_started) {
        return 1;
    }
    if (writer_locked()) {
        return 0;
    }

    time_started = 1;
    model_first_time = 0;
    model_cur_time = 0;
    say("    /* implicit */ fstWriterEmitTimeChange(w, 0);\n");
    return 1;
}

static void record_change(fstHandle handle, const unsigned char *val, uint32_t len, double d)
{
    struct change *c;

    sigs[handle - 1].has_change = 1;
    sigs[handle - 1].last_time = model_cur_time;

    if (n_changes >= MAX_CHANGES) {
        changes_dropped = 1;
        return;
    }

    c = &changes[n_changes++];
    memset(c, 0, sizeof(*c));
    c->time = model_cur_time;
    c->handle = handle;
    c->len = len;
    c->d = d;
    if (len) {
        memcpy(c->val, val, len);
    }
}

/* --------------------------------------------------------------- generate */

static void gen_metadata(uint8_t flags)
{
    int packtype = (int)tape_range(0, 2);

    fstWriterSetPackType(writer, (enum fstWriterPackType)packtype);
    fstWriterSetParallelMode(writer, (flags & 2) ? 1 : 0);
    fstWriterSetRepackOnClose(writer, (flags & 4) ? 1 : 0);

    cfg_timescale = -(int)tape_range(0, 12);
    cfg_timezero = (int64_t)(int16_t)tape_uint(2);
    cfg_filetype = (int)tape_range(FST_FT_MIN, FST_FT_MAX);

    fstWriterSetTimescale(writer, cfg_timescale);
    fstWriterSetTimezero(writer, cfg_timezero);
    fstWriterSetFileType(writer, (enum fstFileType)cfg_filetype);
    fstWriterSetVersion(writer, version_string);
    fstWriterSetDate(writer, date_string);

    say("fstWriterSetPackType(w, %s);\n",
        (packtype == 0) ? "FST_WR_PT_ZLIB"
                        : ((packtype == 1) ? "FST_WR_PT_FASTLZ" : "FST_WR_PT_LZ4"));
    say("fstWriterSetParallelMode(w, %d);\n", (flags & 2) ? 1 : 0);
    say("fstWriterSetRepackOnClose(w, %d);\n", (flags & 4) ? 1 : 0);
    say("fstWriterSetTimescale(w, %d);\n", cfg_timescale);
    say("fstWriterSetTimezero(w, %" PRId64 ");\n", cfg_timezero);
    say("fstWriterSetFileType(w, %d);\n", cfg_filetype);

    if (flags & 8) {
        /* the comment lands in the hierarchy as an attribute, not in the header */
        struct hrec *h = push_hrec(FST_HT_ATTRBEGIN);

        fstWriterSetComment(writer, "fuzz comment");
        say("fstWriterSetComment(w, \"fuzz comment\");\n");

        if (h) {
            h->typ = FST_AT_MISC;
            h->subtype = FST_MT_COMMENT;
            snprintf(h->name, sizeof(h->name), "fuzz comment");
        }
    }

    if (flags & 16) {
        struct hrec *h = push_hrec(FST_HT_ATTRBEGIN);

        fstWriterSetEnvVar(writer, "LIBFST_FUZZ=1");
        say("fstWriterSetEnvVar(w, \"LIBFST_FUZZ=1\");\n");

        if (h) {
            h->typ = FST_AT_MISC;
            h->subtype = FST_MT_ENVVAR;
            snprintf(h->name, sizeof(h->name), "LIBFST_FUZZ=1");
        }
    }

    if (flags & 32) {
        /*
         * A limit this small locks the writer on the first flush, which is the
         * interesting case: everything after the lock is silently dropped.
         */
        uint64_t limit = (uint64_t)tape_range(1, 64) * 64;

        fstWriterSetDumpSizeLimit(writer, limit);
        say("fstWriterSetDumpSizeLimit(w, %" PRIu64 ");\n", limit);
    }

    cfg_check_process_hier_mask = (flags & 64) ? 1 : 0;
}

static void gen_hierarchy(void)
{
    unsigned int depth = 0;
    unsigned int n_cmds = 0;
    int n_etabs = 0;
    fstEnumHandle etab_handles[MAX_HIER];
    int attr_open = 0;

    while (!tape_eof() && n_cmds < MAX_HIER) {
        uint8_t op = tape_u8();
        n_cmds++;

        switch (op % 7) {
            case 0: { /* end of the hierarchy phase */
                if (attr_open) {
                    fstWriterSetAttrEnd(writer);
                    say("fstWriterSetAttrEnd(w);\n");
                    push_hrec(FST_HT_ATTREND);
                }
                return;
            }

            case 1: { /* scope */
                struct hrec *h;
                int typ = (int)tape_range(0, 11);
                int with_comp = tape_u8() & 1;
                char name[16];
                char comp[16];

                if (depth >= MAX_SCOPE_DEPTH) {
                    break;
                }

                snprintf(name, sizeof(name), "s%u", n_cmds);
                snprintf(comp, sizeof(comp), "c%u", n_cmds);

                fstWriterSetScope(writer,
                                  (enum fstScopeType)typ,
                                  name,
                                  with_comp ? comp : NULL);
                say("fstWriterSetScope(w, %d, \"%s\", %s);\n",
                    typ,
                    name,
                    with_comp ? "comp" : "NULL");

                depth++;
                model_scope_count++;

                h = push_hrec(FST_HT_SCOPE);
                if (h) {
                    h->typ = typ;
                    snprintf(h->name, sizeof(h->name), "%s", name);
                    h->has_comp = with_comp;
                    if (with_comp) {
                        snprintf(h->comp, sizeof(h->comp), "%s", comp);
                    }
                }
                break;
            }

            case 2: { /* upscope */
                if (depth == 0) {
                    break;
                }

                fstWriterSetUpscope(writer);
                say("fstWriterSetUpscope(w);\n");

                depth--;
                push_hrec(FST_HT_UPSCOPE);
                break;
            }

            case 3: { /* variable */
                struct hrec *h;
                enum var_kind kind;
                int vt;
                int vd = (int)tape_range(FST_VD_MIN, FST_VD_MAX);
                uint32_t len;
                fstHandle handle;
                char name[16];

                if (n_sigs >= MAX_VARS) {
                    break;
                }

                switch (tape_range(0, 3)) {
                    case 0:
                        kind = VK_REAL;
                        vt = FST_VT_VCD_REAL;
                        len = 8;
                        break;
                    case 1:
                        kind = VK_STRING;
                        vt = FST_VT_GEN_STRING;
                        len = 0;
                        break;
                    default:
                        kind = VK_BITS;
                        vt = (tape_u8() & 1) ? FST_VT_VCD_WIRE : FST_VT_VCD_REG;
                        len = tape_range(1, MAX_WIDTH);
                        break;
                }

                snprintf(name, sizeof(name), "v%u", n_cmds);

                handle = fstWriterCreateVar(writer,
                                            (enum fstVarType)vt,
                                            (enum fstVarDir)vd,
                                            len,
                                            name,
                                            0);
                say("fstHandle h%u = fstWriterCreateVar(w, %d, %d, %u, \"%s\", 0);\n",
                    (unsigned)handle,
                    vt,
                    vd,
                    len,
                    name);

                CHECKF(handle == n_sigs + 1,
                       "fstWriterCreateVar() returned handle %u, expected %u\n",
                       (unsigned)handle,
                       n_sigs + 1);

                sigs[n_sigs].kind = kind;
                sigs[n_sigs].width = (kind == VK_REAL) ? 8 : len;
                sigs[n_sigs].has_change = 0;
                n_sigs++;
                model_var_count++;

                h = push_hrec(FST_HT_VAR);
                if (h) {
                    h->typ = vt;
                    h->direction = vd;
                    /* a real is recast to eight bytes by the writer */
                    h->length = (kind == VK_REAL) ? 8 : len;
                    h->handle = handle;
                    h->is_alias = 0;
                    snprintf(h->name, sizeof(h->name), "%s", name);
                }
                break;
            }

            case 4: { /* alias of an existing variable */
                struct hrec *h;
                fstHandle target;
                fstHandle handle;
                int vd = (int)tape_range(FST_VD_MIN, FST_VD_MAX);
                int vt;
                uint32_t len;
                char name[16];

                if (n_sigs == 0) {
                    break;
                }

                target = (fstHandle)tape_range(1, n_sigs);
                snprintf(name, sizeof(name), "a%u", n_cmds);

                switch (sigs[target - 1].kind) {
                    case VK_REAL:
                        vt = FST_VT_VCD_REAL;
                        len = 8;
                        break;
                    case VK_STRING:
                        vt = FST_VT_GEN_STRING;
                        len = 0;
                        break;
                    default:
                        vt = FST_VT_VCD_WIRE;
                        len = sigs[target - 1].width;
                        break;
                }

                handle = fstWriterCreateVar(writer,
                                            (enum fstVarType)vt,
                                            (enum fstVarDir)vd,
                                            len,
                                            name,
                                            target);
                say("fstWriterCreateVar(w, %d, %d, %u, \"%s\", %u); /* alias */\n",
                    vt,
                    vd,
                    len,
                    name,
                    (unsigned)target);

                CHECKF(handle == target,
                       "an alias of handle %u came back as handle %u\n",
                       (unsigned)target,
                       (unsigned)handle);

                model_var_count++;
                model_alias_count++;

                h = push_hrec(FST_HT_VAR);
                if (h) {
                    h->typ = vt;
                    h->direction = vd;
                    h->length = (sigs[target - 1].kind == VK_REAL) ? 8 : len;
                    h->handle = handle;
                    h->is_alias = 1;
                    snprintf(h->name, sizeof(h->name), "%s", name);
                }
                break;
            }

            case 5: { /* attribute begin or end */
                if (attr_open) {
                    fstWriterSetAttrEnd(writer);
                    say("fstWriterSetAttrEnd(w);\n");
                    push_hrec(FST_HT_ATTREND);
                    attr_open = 0;
                } else {
                    struct hrec *h;
                    uint64_t arg = tape_uint(2);
                    char name[16];

                    snprintf(name, sizeof(name), "attr%u", n_cmds);

                    fstWriterSetAttrBegin(writer,
                                          FST_AT_MISC,
                                          FST_MT_UNKNOWN,
                                          name,
                                          arg);
                    say("fstWriterSetAttrBegin(w, FST_AT_MISC, FST_MT_UNKNOWN, \"%s\", %" PRIu64
                        ");\n",
                        name,
                        arg);

                    h = push_hrec(FST_HT_ATTRBEGIN);
                    if (h) {
                        h->typ = FST_AT_MISC;
                        h->subtype = FST_MT_UNKNOWN;
                        h->arg = arg;
                        snprintf(h->name, sizeof(h->name), "%s", name);
                    }
                    attr_open = 1;
                }
                break;
            }

            default: { /* enum table definition or reference */
                struct hrec *h;

                if (n_etabs > 0 && (tape_u8() & 1)) {
                    int pick = (int)tape_range(0, (uint32_t)n_etabs - 1);

                    fstWriterEmitEnumTableRef(writer, etab_handles[pick]);
                    say("fstWriterEmitEnumTableRef(w, %u);\n", (unsigned)etab_handles[pick]);

                    h = push_hrec(FST_HT_ATTRBEGIN);
                    if (h) {
                        h->typ = FST_AT_MISC;
                        h->subtype = FST_MT_ENUMTABLE;
                        h->arg = etab_handles[pick];
                        h->etab = -1; /* a reference carries no payload */
                    }
                } else {
                    int pick = (int)tape_range(0, N_ETAB_PRESETS - 1);
                    const struct etab_preset *p = &etab_presets[pick];
                    fstEnumHandle eh;

                    if (n_etabs >= MAX_HIER) {
                        break;
                    }

                    eh = fstWriterCreateEnumTable(writer,
                                                  p->name,
                                                  p->elem_count,
                                                  p->min_valbits,
                                                  p->literals,
                                                  p->values);
                    say("fstEnumHandle e%d = fstWriterCreateEnumTable(w, \"%s\", ...);\n",
                        n_etabs,
                        p->name);

                    CHECK(eh != 0);

                    etab_handles[n_etabs] = eh;
                    n_etabs++;

                    h = push_hrec(FST_HT_ATTRBEGIN);
                    if (h) {
                        h->typ = FST_AT_MISC;
                        h->subtype = FST_MT_ENUMTABLE;
                        h->arg = eh;
                        h->etab = pick;
                    }
                }
                break;
            }
        }
    }

    if (attr_open) {
        fstWriterSetAttrEnd(writer);
        say("fstWriterSetAttrEnd(w);\n");
        push_hrec(FST_HT_ATTREND);
    }
}

/* pick a handle of the given kind, or 0 if there is none */
static fstHandle pick_handle(enum var_kind kind)
{
    fstHandle candidates[MAX_VARS];
    unsigned int n = 0;
    unsigned int i;

    for (i = 0; i < n_sigs; i++) {
        if (sigs[i].kind == kind) {
            candidates[n++] = (fstHandle)(i + 1);
        }
    }

    if (n == 0) {
        return 0;
    }
    return candidates[tape_range(0, n - 1)];
}

static double tape_double(void)
{
    uint64_t bits = tape_uint(8);
    double d;

    memcpy(&d, &bits, sizeof(d));

    /*
     * Non-finite doubles round trip as bit patterns, but a signalling NaN is
     * free to change payload on the way through a register, so keep the
     * generated values finite.
     */
    if (!isfinite(d)) {
        d = (double)(int64_t)bits / 1024.0;
    }

    return d;
}

static void gen_body(void)
{
    unsigned int n_cmds = 0;

    while (!tape_eof() && n_cmds < MAX_BODY) {
        uint8_t op = tape_u8();
        n_cmds++;

        switch (op % 8) {
            case 0:
            case 1: { /* time change */
                uint64_t delta = tape_range(0, 255);
                uint64_t t = model_cur_time + delta;
                int locked = writer_locked();

                fstWriterEmitTimeChange(writer, t);
                say("fstWriterEmitTimeChange(w, %" PRIu64 ");%s\n",
                    t,
                    locked ? " /* dropped, writer locked */" : "");

                /* QUIRK: while the dump size limit holds, time changes are dropped */
                if (!locked) {
                    if (!time_started) {
                        time_started = 1;
                        model_first_time = t;
                    }
                    model_cur_time = t;
                }
                break;
            }

            case 2:
            case 3: { /* bit vector value change */
                fstHandle h = pick_handle(VK_BITS);
                unsigned char val[MAX_WIDTH + 1];
                uint32_t width;
                uint32_t i;
                int locked;

                if (h == 0) {
                    break;
                }

                width = sigs[h - 1].width;
                for (i = 0; i < width; i++) {
                    val[i] = (unsigned char)vec_chars[tape_u8() % N_VEC_CHARS];
                }
                val[width] = 0;

                if (!model_start_time()) {
                    break;
                }

                /* one change per handle per time, see the header comment */
                if (sigs[h - 1].has_change && sigs[h - 1].last_time == model_cur_time) {
                    break;
                }

                locked = writer_locked();

                fstWriterEmitValueChange(writer, h, val);
                say("fstWriterEmitValueChange(w, h%u, \"%s\");%s\n",
                    (unsigned)h,
                    (const char *)val,
                    locked ? " /* frame only, writer locked */" : "");

                /*
                 * QUIRK: while locked the value only updates the frame, and no
                 * further section is written, so it never reaches the file.
                 */
                if (!locked) {
                    record_change(h, val, width, 0.0);
                }
                break;
            }

            case 4: { /* real value change */
                fstHandle h = pick_handle(VK_REAL);
                double d;
                int locked;

                if (h == 0) {
                    break;
                }

                d = tape_double();

                if (!model_start_time()) {
                    break;
                }
                if (sigs[h - 1].has_change && sigs[h - 1].last_time == model_cur_time) {
                    break;
                }

                locked = writer_locked();

                fstWriterEmitValueChange(writer, h, &d);
                say("fstWriterEmitValueChange(w, h%u, &(double){%.17g});%s\n",
                    (unsigned)h,
                    d,
                    locked ? " /* frame only, writer locked */" : "");

                if (!locked) {
                    record_change(h, NULL, 0, d);
                }
                break;
            }

            case 5: { /* variable length value change */
                fstHandle h = pick_handle(VK_STRING);
                unsigned char val[MAX_VARLEN];
                uint32_t len;
                uint32_t i;
                int locked;

                if (h == 0) {
                    break;
                }

                len = tape_range(0, MAX_VARLEN);
                for (i = 0; i < len; i++) {
                    val[i] = tape_u8();
                }

                locked = writer_locked();

                /*
                 * QUIRK: unlike the fixed length path this returns before the
                 * implicit time change, so while locked nothing happens at all.
                 */
                if (!locked) {
                    if (!model_start_time()) {
                        break;
                    }
                    if (sigs[h - 1].has_change && sigs[h - 1].last_time == model_cur_time) {
                        break;
                    }
                }

                fstWriterEmitVariableLengthValueChange(writer, h, val, len);
                say("fstWriterEmitVariableLengthValueChange(w, h%u, buf, %u);%s\n",
                    (unsigned)h,
                    len,
                    locked ? " /* dropped, writer locked */" : "");

                if (!locked) {
                    record_change(h, val, len, 0.0);
                }
                break;
            }

            case 6: { /* flush */
                fstWriterFlushContext(writer);
                say("fstWriterFlushContext(w);\n");

                /*
                 * QUIRK: once the writer is locked, and before the first time
                 * change, nothing is ever reported as pending.
                 */
                if (writer_locked() || !time_started) {
                    CHECKF(fstWriterGetFlushContextPending(writer) == 0,
                           "fstWriterGetFlushContextPending() reported a pending flush while the "
                           "writer was %s\n",
                           writer_locked() ? "locked" : "before its first time change");
                }
                break;
            }

            default: { /* dump activity */
                int enable = tape_u8() & 1;

                fstWriterEmitDumpActive(writer, enable);
                say("fstWriterEmitDumpActive(w, %d);\n", enable);

                /* the dump activity list ignores the dump size limit */
                if (n_blackouts < MAX_BLACKOUTS) {
                    blackouts[n_blackouts].time = model_cur_time;
                    blackouts[n_blackouts].active = (unsigned char)enable;
                    n_blackouts++;
                } else {
                    blackouts_dropped = 1;
                }
                break;
            }
        }

        if (writer_locked()) {
            limit_hit = 1;
        }
    }
}

/*
 * QUIRK: when simulation time never advanced, fstWriterClose() emits a time
 * change at zero and then clones the frame into it, so that a reader still
 * sees the initial value of every signal -- 'x' repeated for a bit vector and
 * NaN for a real.  A variable length handle gets nothing, since it has no
 * frame slot and fstWriterEmitValueChange() ignores a zero length.
 */
static void model_close(void)
{
    unsigned int i;

    if (time_started || writer_locked()) {
        return;
    }

    time_started = 1;
    model_first_time = 0;
    model_cur_time = 0;

    say("/* fstWriterClose() mocks up time zero values for %u handles */\n", n_sigs);

    for (i = 0; i < n_sigs; i++) {
        unsigned char val[MAX_WIDTH + 1];

        switch (sigs[i].kind) {
            case VK_STRING:
                break;
            case VK_REAL:
                record_change((fstHandle)(i + 1), NULL, 0, strtod("NaN", NULL));
                break;
            default:
                memset(val, 'x', sigs[i].width);
                val[sigs[i].width] = 0;
                record_change((fstHandle)(i + 1), val, sigs[i].width, 0.0);
                break;
        }
    }
}

/* ---------------------------------------------------------------- oracles */

static uint64_t last_cb_time;
static int have_cb_time;

static int find_change(uint64_t time, fstHandle handle)
{
    unsigned int i;

    for (i = 0; i < n_changes; i++) {
        if (!changes[i].delivered && changes[i].time == time && changes[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static void check_cb_common(uint64_t time, fstHandle handle)
{
    CHECKF(handle >= 1 && handle <= n_sigs,
           "a callback reported handle %u, the last handle is %u\n",
           (unsigned)handle,
           n_sigs);

    if (have_cb_time) {
        CHECKF(time >= last_cb_time,
               "callback times went backwards: %" PRIu64 " after %" PRIu64 "\n",
               time,
               last_cb_time);
    }
    have_cb_time = 1;
    last_cb_time = time;
}

static void value_cb(void *user, uint64_t time, fstHandle handle, const unsigned char *value)
{
    struct change *c;
    int idx;

    (void)user;

    check_cb_common(time, handle);

    if (changes_dropped) {
        return;
    }

    idx = find_change(time, handle);
    CHECKF(idx >= 0,
           "the callback reported a change for handle %u at time %" PRIu64
           " that was never written: \"%s\"\n",
           (unsigned)handle,
           time,
           (const char *)value);

    c = &changes[idx];
    c->delivered = 1;

    if (sigs[handle - 1].kind == VK_REAL) {
        char expect[64];

        /*
         * QUIRK: fstReaderIterBlocks2() hands a real over as "%.16g" while
         * fstReaderGetValueFromHandleAtTime() hands the same value over as
         * "r%.16g".
         */
        real_to_string(c->d, expect, sizeof(expect), 0);
        CHECKF(strcmp((const char *)value, expect) == 0,
               "real handle %u at time %" PRIu64 ": read \"%s\", wrote \"%s\"\n",
               (unsigned)handle,
               time,
               (const char *)value,
               expect);
    } else {
        CHECKF(sigs[handle - 1].kind == VK_BITS,
               "handle %u is variable length but came back through the fixed length callback\n",
               (unsigned)handle);
        CHECKF(strlen((const char *)value) == c->len,
               "handle %u at time %" PRIu64 ": read %u characters, wrote %u\n",
               (unsigned)handle,
               time,
               (unsigned)strlen((const char *)value),
               c->len);
        CHECKF(memcmp(value, c->val, c->len) == 0,
               "handle %u at time %" PRIu64 ": read \"%s\", wrote \"%s\"\n",
               (unsigned)handle,
               time,
               (const char *)value,
               (const char *)c->val);
    }
}

static void varlen_cb(void *user,
                      uint64_t time,
                      fstHandle handle,
                      const unsigned char *value,
                      uint32_t len)
{
    struct change *c;
    int idx;

    (void)user;

    check_cb_common(time, handle);

    if (changes_dropped) {
        return;
    }

    idx = find_change(time, handle);
    CHECKF(idx >= 0,
           "the varlen callback reported a change for handle %u at time %" PRIu64
           " that was never written\n",
           (unsigned)handle,
           time);

    c = &changes[idx];
    c->delivered = 1;

    CHECKF(sigs[handle - 1].kind == VK_STRING,
           "handle %u is fixed length but came back through the varlen callback\n",
           (unsigned)handle);
    CHECKF(len == c->len,
           "varlen handle %u at time %" PRIu64 ": read %u bytes, wrote %u\n",
           (unsigned)handle,
           time,
           len,
           c->len);
    CHECKF(len == 0 || memcmp(value, c->val, len) == 0,
           "varlen handle %u at time %" PRIu64 ": the bytes differ\n",
           (unsigned)handle,
           time);
}

static void check_hierarchy(fstReaderContext *reader)
{
    struct fstHier *hier;
    unsigned int i = 0;

    if (hrecs_dropped) {
        return;
    }

    while ((hier = fstReaderIterateHier(reader)) != NULL) {
        struct hrec *h;

        CHECKF(i < n_hrecs,
               "the hierarchy has more records than were written: got at least %u, wrote %u\n",
               i + 1,
               n_hrecs);

        h = &hrecs[i];
        CHECKF(hier->htyp == h->htyp,
               "hierarchy record %u: read type %d, wrote type %d\n",
               i,
               hier->htyp,
               h->htyp);

        switch (h->htyp) {
            case FST_HT_SCOPE:
                CHECKF(hier->u.scope.typ == h->typ,
                       "scope record %u: read scope type %d, wrote %d\n",
                       i,
                       hier->u.scope.typ,
                       h->typ);
                CHECKF(strcmp(hier->u.scope.name, h->name) == 0,
                       "scope record %u: read name \"%s\", wrote \"%s\"\n",
                       i,
                       hier->u.scope.name,
                       h->name);
                CHECKF(hier->u.scope.name_length == strlen(h->name),
                       "scope record %u: read name length %u, wrote %u\n",
                       i,
                       (unsigned)hier->u.scope.name_length,
                       (unsigned)strlen(h->name));
                if (h->has_comp) {
                    CHECKF(strcmp(hier->u.scope.component, h->comp) == 0,
                           "scope record %u: read component \"%s\", wrote \"%s\"\n",
                           i,
                           hier->u.scope.component,
                           h->comp);
                }
                break;

            case FST_HT_VAR:
                CHECKF(hier->u.var.typ == h->typ,
                       "var record %u: read var type %d, wrote %d\n",
                       i,
                       hier->u.var.typ,
                       h->typ);
                CHECKF(hier->u.var.direction == h->direction,
                       "var record %u: read direction %d, wrote %d\n",
                       i,
                       hier->u.var.direction,
                       h->direction);
                CHECKF(hier->u.var.length == h->length,
                       "var record %u: read length %u, wrote %u\n",
                       i,
                       (unsigned)hier->u.var.length,
                       (unsigned)h->length);
                CHECKF(hier->u.var.handle == h->handle,
                       "var record %u: read handle %u, wrote %u\n",
                       i,
                       (unsigned)hier->u.var.handle,
                       (unsigned)h->handle);
                CHECKF((hier->u.var.is_alias != 0) == (h->is_alias != 0),
                       "var record %u: read is_alias %d, wrote %d\n",
                       i,
                       hier->u.var.is_alias,
                       h->is_alias);
                CHECKF(strcmp(hier->u.var.name, h->name) == 0,
                       "var record %u: read name \"%s\", wrote \"%s\"\n",
                       i,
                       hier->u.var.name,
                       h->name);
                break;

            case FST_HT_ATTRBEGIN:
                CHECKF(hier->u.attr.typ == h->typ,
                       "attribute record %u: read type %d, wrote %d\n",
                       i,
                       hier->u.attr.typ,
                       h->typ);
                CHECKF(hier->u.attr.subtype == h->subtype,
                       "attribute record %u: read subtype %d, wrote %d\n",
                       i,
                       hier->u.attr.subtype,
                       h->subtype);
                CHECKF(hier->u.attr.arg == h->arg,
                       "attribute record %u: read arg %" PRIu64 ", wrote %" PRIu64 "\n",
                       i,
                       hier->u.attr.arg,
                       h->arg);

                if (h->etab >= 0) {
                    /* an enum table definition carries the encoded table */
                    const struct etab_preset *p = &etab_presets[h->etab];
                    struct fstETab *decoded;
                    uint32_t k;

                    decoded = fstUtilityExtractEnumTableFromString(hier->u.attr.name);
                    CHECKF(decoded != NULL,
                           "attribute record %u: the enum table string did not decode: \"%s\"\n",
                           i,
                           hier->u.attr.name);
                    CHECKF(strcmp(decoded->name, p->name) == 0,
                           "attribute record %u: enum table name \"%s\", wrote \"%s\"\n",
                           i,
                           decoded->name,
                           p->name);
                    CHECKF(decoded->elem_count == p->elem_count,
                           "attribute record %u: enum table has %u elements, wrote %u\n",
                           i,
                           (unsigned)decoded->elem_count,
                           (unsigned)p->elem_count);
                    for (k = 0; k < p->elem_count; k++) {
                        CHECKF(strcmp(decoded->literal_arr[k], p->literals[k]) == 0,
                               "attribute record %u: enum literal %u is \"%s\", wrote \"%s\"\n",
                               i,
                               k,
                               decoded->literal_arr[k],
                               p->literals[k]);
                        CHECKF(strcmp(decoded->val_arr[k], p->values[k]) == 0,
                               "attribute record %u: enum value %u is \"%s\", wrote \"%s\"\n",
                               i,
                               k,
                               decoded->val_arr[k],
                               p->values[k]);
                    }
                    fstUtilityFreeEnumTable(decoded);
                } else if (h->subtype == FST_MT_ENUMTABLE) {
                    /* an enum table reference carries only the handle */
                    CHECKF(hier->u.attr.name_length == 0,
                           "attribute record %u: an enum table reference carried a payload: "
                           "\"%s\"\n",
                           i,
                           hier->u.attr.name);
                } else {
                    CHECKF(strcmp(hier->u.attr.name, h->name) == 0,
                           "attribute record %u: read name \"%s\", wrote \"%s\"\n",
                           i,
                           hier->u.attr.name,
                           h->name);
                }
                break;

            default:
                break;
        }

        i++;
    }

    CHECKF(i == n_hrecs,
           "the hierarchy has %u records, wrote %u\n",
           i,
           n_hrecs);
}

static void check_value_at_time(fstReaderContext *reader)
{
    unsigned int sampled = 0;
    unsigned int i;
    char buf[MAX_WIDTH + 64];

    /* the times the reader could still have on file */
    uint64_t cutoff = have_cb_time ? last_cb_time : 0;

    if (changes_dropped || !have_cb_time) {
        return;
    }

    for (i = 0; i < n_changes && sampled < MAX_RVAT_SAMPLES; i++) {
        struct change *c = &changes[i];
        struct sig *s = &sigs[c->handle - 1];
        unsigned int j;
        struct change *last = c;
        char *got;

        if (s->kind == VK_STRING) {
            /*
             * QUIRK: fstReaderGetValueFromHandleAtTime() has no variable
             * length path and returns NULL for such a handle.
             */
            CHECKF(fstReaderGetValueFromHandleAtTime(reader, c->time, c->handle, buf) == NULL,
                   "fstReaderGetValueFromHandleAtTime() returned a value for the variable length "
                   "handle %u\n",
                   (unsigned)c->handle);
            continue;
        }

        if (c->time > cutoff) {
            continue;
        }

        /* the change in effect at c->time is the last one at or before it */
        for (j = i + 1; j < n_changes; j++) {
            if (changes[j].handle == c->handle && changes[j].time <= c->time) {
                last = &changes[j];
            }
        }

        got = fstReaderGetValueFromHandleAtTime(reader, c->time, c->handle, buf);
        CHECKF(got != NULL,
               "fstReaderGetValueFromHandleAtTime() returned nothing for handle %u at time %" PRIu64
               ", which has a value change there\n",
               (unsigned)c->handle,
               c->time);

        if (s->kind == VK_REAL) {
            char expect[64];

            real_to_string(last->d, expect, sizeof(expect), 1);
            CHECKF(strcmp(got, expect) == 0,
                   "fstReaderGetValueFromHandleAtTime() for real handle %u at time %" PRIu64
                   ": read \"%s\", wrote \"%s\"\n",
                   (unsigned)c->handle,
                   c->time,
                   got,
                   expect);
        } else {
            CHECKF(strcmp(got, (const char *)last->val) == 0,
                   "fstReaderGetValueFromHandleAtTime() for handle %u at time %" PRIu64
                   ": read \"%s\", wrote \"%s\"\n",
                   (unsigned)c->handle,
                   c->time,
                   got,
                   (const char *)last->val);
        }

        sampled++;
    }
}

static void check_header(fstReaderContext *reader)
{
    CHECKF(fstReaderGetStartTime(reader) == model_first_time,
           "start time is %" PRIu64 ", wrote %" PRIu64 "\n",
           fstReaderGetStartTime(reader),
           model_first_time);
    CHECKF(fstReaderGetEndTime(reader) == model_cur_time,
           "end time is %" PRIu64 ", wrote %" PRIu64 "\n",
           fstReaderGetEndTime(reader),
           model_cur_time);
    CHECKF(fstReaderGetMaxHandle(reader) == n_sigs,
           "max handle is %u, created %u signals\n",
           (unsigned)fstReaderGetMaxHandle(reader),
           n_sigs);
    CHECKF(fstReaderGetVarCount(reader) == model_var_count,
           "var count is %" PRIu64 ", wrote %" PRIu64 "\n",
           fstReaderGetVarCount(reader),
           model_var_count);
    CHECKF(fstReaderGetScopeCount(reader) == model_scope_count,
           "scope count is %" PRIu64 ", wrote %" PRIu64 "\n",
           fstReaderGetScopeCount(reader),
           model_scope_count);
    CHECKF(fstReaderGetAliasCount(reader) == model_alias_count,
           "alias count is %" PRIu64 ", wrote %" PRIu64 "\n",
           fstReaderGetAliasCount(reader),
           model_alias_count);
    CHECKF(fstReaderGetTimescale(reader) == cfg_timescale,
           "timescale is %d, wrote %d\n",
           (int)fstReaderGetTimescale(reader),
           cfg_timescale);
    CHECKF(fstReaderGetTimezero(reader) == cfg_timezero,
           "timezero is %" PRId64 ", wrote %" PRId64 "\n",
           fstReaderGetTimezero(reader),
           cfg_timezero);
    CHECKF(fstReaderGetFileType(reader) == cfg_filetype,
           "file type is %d, wrote %d\n",
           fstReaderGetFileType(reader),
           cfg_filetype);
    CHECK(fstReaderGetVersionString(reader) != NULL);
    CHECKF(strcmp(fstReaderGetVersionString(reader), version_string) == 0,
           "version is \"%s\", wrote \"%s\"\n",
           fstReaderGetVersionString(reader),
           version_string);
    CHECK(fstReaderGetDateString(reader) != NULL);
    CHECKF(strcmp(fstReaderGetDateString(reader), date_string) == 0,
           "date is \"%s\", wrote \"%s\"\n",
           fstReaderGetDateString(reader),
           date_string);
    CHECK(fstReaderGetFseekFailed(reader) == 0);
}

static void check_blackouts(fstReaderContext *reader)
{
    unsigned int i;

    if (blackouts_dropped) {
        return;
    }

    CHECKF(fstReaderGetNumberDumpActivityChanges(reader) == n_blackouts,
           "the dump activity list has %u entries, wrote %u\n",
           (unsigned)fstReaderGetNumberDumpActivityChanges(reader),
           n_blackouts);

    for (i = 0; i < n_blackouts; i++) {
        CHECKF(fstReaderGetDumpActivityChangeTime(reader, i) == blackouts[i].time,
               "dump activity %u is at time %" PRIu64 ", wrote %" PRIu64 "\n",
               i,
               fstReaderGetDumpActivityChangeTime(reader, i),
               blackouts[i].time);
        CHECKF(fstReaderGetDumpActivityChangeValue(reader, i) == blackouts[i].active,
               "dump activity %u is %d, wrote %d\n",
               i,
               (int)fstReaderGetDumpActivityChangeValue(reader, i),
               (int)blackouts[i].active);
    }
}

static void check_process_hier_keeps_mask(fstReaderContext *reader)
{
    FILE *sink;
    fstHandle h;

    if (!cfg_check_process_hier_mask || n_sigs == 0) {
        return;
    }

    sink = tmpfile();
    if (!sink) {
        return;
    }

    /*
     * The mask reads as reader wide configuration, so setting it before
     * fstReaderProcessHier() has to keep it: that function rebuilds the per
     * handle arrays and used to reset the mask while doing so.
     */
    fstReaderSetFacProcessMaskAll(reader);
    CHECK(fstReaderProcessHier(reader, sink) != 0);

    for (h = 1; h <= n_sigs; h++) {
        CHECKF(fstReaderGetFacProcessMask(reader, h) != 0,
               "fstReaderProcessHier() cleared the process mask of handle %u\n",
               (unsigned)h);
    }

    fclose(sink);
}

static void verify(const char *path)
{
    fstReaderContext *reader;
    unsigned int i;

    reader = fstReaderOpen(path);
    CHECKF(reader != NULL, "fstReaderOpen() rejected a file this library wrote\n");

    check_header(reader);
    check_hierarchy(reader);
    check_blackouts(reader);

    fstReaderSetFacProcessMaskAll(reader);
    CHECK(fstReaderIterBlocks2(reader, value_cb, varlen_cb, NULL, NULL) != 0);

    if (!changes_dropped) {
        for (i = 0; i < n_changes; i++) {
            if (changes[i].delivered) {
                continue;
            }

            /*
             * QUIRK: once the dump size limit locks the writer the section
             * being built is discarded, so the reader sees a prefix of what
             * was written.  Sections do not straddle a time, so everything at
             * or before the last reported time has to be there.
             */
            CHECKF(limit_hit && (!have_cb_time || changes[i].time > last_cb_time),
                   "the change for handle %u at time %" PRIu64
                   " was written but never reported (last reported time %" PRIu64 ", dump size "
                   "limit %s)\n",
                   (unsigned)changes[i].handle,
                   changes[i].time,
                   have_cb_time ? last_cb_time : 0,
                   limit_hit ? "was reached" : "was not reached");
        }
    }

    check_value_at_time(reader);
    check_process_hier_keeps_mask(reader);

    fstReaderClose(reader);
}

/* ------------------------------------------------------------------- main */

static void reset(const uint8_t *data, size_t size)
{
    tape.data = data;
    tape.len = size;
    tape.pos = 0;

    memset(sigs, 0, sizeof(sigs));
    n_sigs = 0;

    memset(changes, 0, sizeof(changes));
    n_changes = 0;
    changes_dropped = 0;

    memset(hrecs, 0, sizeof(hrecs));
    n_hrecs = 0;
    hrecs_dropped = 0;

    memset(blackouts, 0, sizeof(blackouts));
    n_blackouts = 0;
    blackouts_dropped = 0;

    model_first_time = 0;
    model_cur_time = 0;
    time_started = 0;
    limit_hit = 0;

    model_var_count = 0;
    model_scope_count = 0;
    model_alias_count = 0;

    last_cb_time = 0;
    have_cb_time = 0;

    script[0] = 0;
    script_len = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char path[] = "/tmp/libfst-fuzz-XXXXXX";
    const char *tmpdir = getenv("TMPDIR");
    char pathbuf[512];
    uint8_t flags;
    int fd;

    if (size < 4 || size > (1u << 16)) {
        return 0;
    }

    trace = getenv("FST_FUZZ_TRACE") != NULL;

    if (tmpdir && *tmpdir) {
        snprintf(pathbuf, sizeof(pathbuf), "%s/libfst-fuzz-XXXXXX", tmpdir);
    } else {
        snprintf(pathbuf, sizeof(pathbuf), "%s", path);
    }

    fd = mkstemp(pathbuf);
    if (fd < 0) {
        return 0;
    }
    close(fd);

    reset(data, size);

    flags = tape_u8();

    writer = fstWriterCreate(pathbuf, (flags & 1) ? 1 : 0);
    if (!writer) {
        unlink(pathbuf);
        return 0;
    }
    say("fstWriterContext *w = fstWriterCreate(path, %d);\n", (flags & 1) ? 1 : 0);

    gen_metadata(flags);
    gen_hierarchy();
    gen_body();

    if (writer_locked()) {
        limit_hit = 1;
    }

    model_close();

    fstWriterClose(writer);
    writer = NULL;
    say("fstWriterClose(w);\n");

    verify(pathbuf);

    unlink(pathbuf);
    return 0;
}

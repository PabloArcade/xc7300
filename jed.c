/* jed.c -- JEDEC reader for the XEPLD (XC7000/XC7200/XC7300) dialect.
 *
 * Derived from the xc9500dump reader, but the XEPLD dialect differs in ways
 * that MATTER and were measured on real MAKEJED output, not assumed:
 *
 *   1. There is no `N DEVICE <name>` note field. The part name sits on its
 *      own line immediately after STX (0x02), e.g. "XC7336-5PC44", followed
 *      by a line "<design>*".
 *
 *   2. L records are IRREGULAR AND DELIBERATELY OVERLAPPING. Measured on
 *      P1.JED (XC7336):
 *
 *          L00080  40 bits   -> 80..119
 *          L00088   8 bits   -> 88..95     (inside the previous record)
 *          L00090  32 bits   -> 90..121    (address not a multiple of 8)
 *
 *      and the overlapping regions DISAGREE, so this is not redundancy: later
 *      records are corrections. Standard JEDEC last-write-wins is therefore
 *      the correct model, and records must be applied strictly in file order.
 *      A parser that assumes fixed-width non-overlapping records will read a
 *      different bitmap and never notice.
 *
 *   3. Only a fraction of QF is explicitly listed -- 16176 of 128960 fuses on
 *      P1 -- and the rest take the F<n> default. F1 (all ones) is what MAKEJED
 *      writes.
 */

#define _GNU_SOURCE
#include "jed.h"

#include "decode.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_whole_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* The part name is the first line after STX that looks like XC7... */
static void find_device(const char *text, size_t len, JedFile *out) {
    size_t i;
    out->device[0] = '\0';
    for (i = 0; i + 3 < len; i++) {
        if ((text[i] == 'X' || text[i] == 'x') &&
            (text[i+1] == 'C' || text[i+1] == 'c') && text[i+2] == '7') {
            size_t j = 0;
            while (i + j < len && j < sizeof(out->device) - 1) {
                char c = text[i + j];
                if (c == '\r' || c == '\n' || c == '*' || c == '\x02') break;
                out->device[j] = c;
                j++;
            }
            while (j > 0 && (out->device[j-1] == ' ' || out->device[j-1] == '\t'))
                j--;
            out->device[j] = '\0';
            if (j >= 6) return;          /* e.g. XC7336 */
            out->device[0] = '\0';
        }
    }
}

/* INTEL HEX RECOGNITION.
 *
 * A device programmer writes `.prg` files in Intel hex -- the format
 * `HPREPORT -r I` produces -- not JEDEC. Feeding one to a JEDEC parser gave
 * only "cannot read", which is correct but unhelpful.
 *
 * This recognises the format, validates every record's checksum, and reports
 * what it holds. It does NOT decode fuses: the byte addresses are the
 * PROGRAMMER's address space, and the mapping to JEDEC fuse numbers is not
 * established -- `L = 8*addr + bit` scores 57%% and byte-per-fuse 62%%, both
 * noise, tested against two real readbacks held in BOTH formats. */
int ihex_probe(const char *path, IhexInfo *info) {
    size_t len = 0, i, nrec = 0, nbyte = 0, bad = 0;
    long lo = -1, hi = -1;
    char *text = read_whole_file(path, &len);
    const char *p;
    if (!text) return -1;
    memset(info, 0, sizeof(*info));
    for (p = text; *p; ) {
        if (*p != ':') { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        {
            unsigned v[4], sum = 0, k;
            int n, addr, typ;
            if (sscanf(p + 1, "%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3]) != 4)
                { bad++; while (*p && *p != '\n') p++; if (*p) p++; continue; }
            n = (int)v[0]; addr = (int)((v[1] << 8) | v[2]); typ = (int)v[3];
            for (k = 0; k < 4; k++) sum += v[k];
            for (k = 0; k < (unsigned)n + 1; k++) {
                unsigned b;
                if (sscanf(p + 9 + 2 * k, "%2x", &b) != 1) { bad++; break; }
                sum += b;
            }
            if ((sum & 0xFF) != 0) bad++;
            if (typ == 0) {
                nrec++; nbyte += (size_t)n;
                if (lo < 0 || addr < lo) lo = addr;
                if (hi < 0 || addr + n - 1 > hi) hi = addr + n - 1;
            }
        }
        while (*p && *p != '\n') p++; if (*p) p++;
    }
    free(text);
    info->records = nrec; info->bytes = nbyte;
    info->lo = lo; info->hi = hi; info->bad_checksums = bad;
    return nrec ? 0 : -1;
}

/* which format is this file? */
int file_format(const char *path) {
    size_t len = 0; char *t = read_whole_file(path, &len); const char *p;
    int fmt = FMT_UNKNOWN;
    if (!t) return FMT_UNKNOWN;
    /* JEDEC WINS ON AMBIGUITY. A JEDEC header is free text and
     * routinely contains a colon followed by hex digits ("Date: 08/14..."),
     * so an Intel-hex test that only looks for `:xx` fires on it. An Intel
     * hex record must start a LINE and carry a full 4-byte prologue; a JEDEC
     * file is identified by `*` or `QF` anywhere in the header. Check JEDEC
     * first, and require line-start for hex. */
    for (p = t; *p && (size_t)(p - t) < 4096; p++)
        if (*p == '*' || (*p == 'Q' && p[1] == 'F')) { fmt = FMT_JEDEC; break; }
    if (fmt == FMT_UNKNOWN)
        for (p = t; *p && (size_t)(p - t) < 4096; p++) {
            if (*p != ':' || (p != t && p[-1] != '\n' && p[-1] != '\r'))
                continue;
            {   unsigned a, b, c, d;
                if (sscanf(p + 1, "%2x%2x%2x%2x", &a, &b, &c, &d) == 4)
                    { fmt = FMT_IHEX; break; }
            }
        }
    free(t);
    return fmt;
}

int jed_load(const char *path, JedFile *out) {
    unsigned char *seen = NULL;
    size_t len = 0, i;
    char *text = read_whole_file(path, &len);
    long qf = -1;
    size_t qf_pos = 0;
    int  deflt = 1;                       /* MAKEJED writes F1 */
    const char *p;

    if (!text) return -1;
    memset(out, 0, sizeof(*out));

    /* QF -- the authoritative fuse count */
    for (i = 0; i + 2 < len; i++) {
        if (text[i] == 'Q' && text[i+1] == 'F' && isdigit((unsigned char)text[i+2])) {
            qf = strtol(text + i + 2, NULL, 10);
            qf_pos = i;
            break;
        }
    }
    if (qf <= 0) { free(text); return -1; }

    /* F<n> -- default state of every fuse not covered by an L record.
     *
     * MUST be searched only AFTER the QF field. The XEPLD dialect puts the
     * DESIGN NAME on its own line as `<name>*`, so a design called F0 or F1
     * produces the byte sequence "\nF0*" -- indistinguishable from the
     * default-fuse field by pattern alone. Scanning from the start of the
     * file therefore read the design name as the fuse default and inverted
     * the whole bitmap.
     *
     * Found the hard way: a probe campaign whose designs were named F0..F6
     * reported 95.9% of fuses clear on one design and 3.4% on its siblings,
     * and diffs of 119292 fuses between designs differing by one literal.
     * JEDEC writes QF before F, and the design name before both, so
     * anchoring the search at QF removes the ambiguity. */
    for (i = qf_pos; i + 1 < len; i++) {
        if (text[i] == 'F' && (text[i+1] == '0' || text[i+1] == '1')) {
            int prev_ok = (i == 0) || text[i-1] == '\n' || text[i-1] == '\r' ||
                          text[i-1] == '*' || text[i-1] == ' ';
            if (prev_ok && (i + 2 >= len || text[i+2] == '*')) {
                deflt = text[i+1] - '0';
                break;
            }
        }
    }

    out->count = (size_t)qf;
    out->fuses = (unsigned char *)malloc(out->count);
    seen = (unsigned char *)calloc(out->count ? out->count : 1, 1);
    if (!seen) { free(text); return -1; }
    if (!out->fuses) { free(text); return -1; }
    memset(out->fuses, (unsigned char)deflt, out->count);
    out->default_state = deflt;
    out->n_records = 0;
    out->recs = NULL;
    out->n_recs = 0;
    out->max_addr = 0;

    find_device(text, len, out);

    /* L record application.
     *
     * HISTORY, because it is easy to re-introduce: an earlier version read
     * overlapping records as last-write-wins, a later one used a UNION
     * reconstruction. BOTH were wrong: the
     * RECORD is the unit of meaning, and every consumer reads the OWNING
     * record via jed_owned(). The flat fuses[] array below is kept only for
     * info/diff/dump, which are address-oriented by nature.
     *
     * JED_UNION was removed as dead code sweep -- it had no
     * caller left and its presence implied a choice that no longer exists. */
    p = text;
    while ((p = strchr(p, 'L')) != NULL) {
        const char *q = p + 1;
        long addr;
        size_t nbits = 0;
        if (!isdigit((unsigned char)*q)) { p++; continue; }
        addr = strtol(q, (char **)&q, 10);
        if (addr < 0) { p++; continue; }
        /* bits run until the field terminator '*' */
        while (*q && *q != '*') {
            if (*q == '0' || *q == '1') {
                size_t a = (size_t)addr + nbits;
                if (a < out->count) {
                    out->fuses[a] = (unsigned char)(*q - '0');
                    seen[a] = 1;
                }
                nbits++;
            } else if (!isspace((unsigned char)*q)) {
                break;                    /* not an L record after all */
            }
            q++;
        }
        if (nbits) {
            /* keep the record verbatim as well */
            JedRec *nr = (JedRec *)realloc(out->recs,
                            (out->n_recs + 1) * sizeof(JedRec));
            if (nr) {
                const char *r = p + 1;
                size_t j = 0;
                /* skip the ADDRESS digits -- '0' and '1' inside the address
                 * would otherwise be collected as fuse bits and shift the
                 * whole payload. */
                while (isdigit((unsigned char)*r)) r++;
                out->recs = nr;
                out->recs[out->n_recs].addr  = (size_t)addr;
                out->recs[out->n_recs].nbits = nbits;
                out->recs[out->n_recs].bits  = (unsigned char *)malloc(nbits);
                while (*r && *r != '*' && j < nbits) {
                    if (*r == '0' || *r == '1')
                        out->recs[out->n_recs].bits[j++] = (unsigned char)(*r - '0');
                    r++;
                }
                out->n_recs++;
            }
            out->n_records++;
            if ((size_t)addr + nbits > out->max_addr)
                out->max_addr = (size_t)addr + nbits;
        }
        p = (*q == '*') ? q + 1 : p + 1;
    }

    /* stated fuse checksum, if present: a "C<hex>*" field */
    out->stated_checksum = -1;
    for (i = 0; i + 1 < len; i++) {
        if (text[i] == 'C' && isxdigit((unsigned char)text[i+1])) {
            int prev_ok = (i == 0) || text[i-1] == '\n' || text[i-1] == '\r' ||
                          text[i-1] == '*';
            if (prev_ok) {
                char *end = NULL;
                long v = strtol(text + i + 1, &end, 16);
                if (end && *end == '*') { out->stated_checksum = v; break; }
            }
        }
    }

    free(seen);
    free(text);

    /* build the O(1) address index -- see jed.h's
     * comment on `by_addr`. Sized to `max_addr+1` (every record's own
     * `addr` is < max_addr+1 by construction, line 270-271 above).
     * `SIZE_MAX` marks "no record starts here"; a genuine record index
     * is written in FILE ORDER, so an address that (in practice, never
     * observed, but not assumed impossible) has more than one record
     * starting at it keeps the EARLIEST one -- an `if` that only ever
     * assigns when the slot is still the sentinel preserves the linear
     * scan's own first-match semantics exactly, not merely usually. */
    out->by_addr = NULL;
    out->by_addr_n = out->max_addr + 1;
    if (out->by_addr_n > 0) {
        out->by_addr = (size_t *)malloc(out->by_addr_n * sizeof(size_t));
        if (out->by_addr) {
            for (i = 0; i < out->by_addr_n; i++) out->by_addr[i] = (size_t)-1;
            for (i = 0; i < out->n_recs; i++) {
                size_t a = out->recs[i].addr;
                if (a < out->by_addr_n && out->by_addr[a] == (size_t)-1)
                    out->by_addr[a] = i;
            }
        }
    }
    return 0;
}

void jed_free(JedFile *jf) {
    size_t i;
    for (i = 0; i < jf->n_recs; i++) free(jf->recs[i].bits);
    free(jf->recs);
    jf->recs = NULL;
    jf->n_recs = 0;
    free(jf->by_addr);
    jf->by_addr = NULL;
    jf->by_addr_n = 0;
    if (!jf) return;
    free(jf->fuses);
    jf->fuses = NULL;
    jf->count = 0;
}


/* THE DECODER'S READ SET, MEASURED FROM THE DECODER.
 * `jed_owned_ex` is the SOLE choke point for every fuse read `decode.c`
 * or `main.c`'s decode-path helpers ever perform (verified by grep: the
 * only other `jf->fuses[]` accesses in the whole tree are `cmd_info`,
 * `cmd_diff`, `cmd_dump`, `cmd_colat` -- utility commands, never called
 * during `decode`). `want` (block*128+rec_off) is already in the same
 * writable-ADDRESS units the census/mutation-scan use
 * (`readback`'s byte-addressing convention, -- no separate
 * encoder needed.
 *
 * Env-gated (`XC73_FUSE_READ_LOG=<path>`), OFF by default, ZERO effect
 * on any decode's own output either way -- this only ever WRITES a
 * side file, at process exit, never reads back or branches on what it
 * recorded. Cost when disabled: one static-int check and one NULL
 * pointer check per call, the same shape as every other cached
 * `getenv()` gate in this codebase since the
 * performance fix (jed.h/jed.c's own `by_addr` comment) -- measured
 * negligible against that fix's baseline, not merely assumed so (see
 * The regression figures). A bitset, not a per-call
 * fprintf: a dense XC73108 decode alone calls this ~250 million times
 * (sec. The gprof figure); logging every call
 * would dominate runtime. 200,000 addresses covers every device's own
 * QF/8 (largest is XC73108 at 97,392) with headroom, checked at
 * config time, not assumed. */
static unsigned char *g_read_seen = NULL;
/* The NATIVE (block,rec_off,group,bit) tuple that produced
 * each flat address, recorded the FIRST time it is seen -- reuses the
 * real call's own parameters, never re-derives them by guessing a
 * decomposition of the flat sum back apart (multiple (block,rec_off,
 * group,bit) tuples can sum to the same flat address; only the tuple
 * decode.c ACTUALLY called with is real, and that is exactly what
 * these four arrays capture). Lets a later pass call the SAME
 * `name_bit()` naming function `rdiff` itself uses -- no duplicate
 * naming logic (rule 4). */
static int *g_read_block = NULL;
static int *g_read_recoff = NULL;
static int *g_read_group = NULL;
static int *g_read_bit = NULL;
static size_t g_read_seen_n = 0;
static const char *g_read_log_path = NULL;
static int g_read_log_checked = 0;

static void fuse_read_log_flush(void) {
    size_t i;
    FILE *f;
    if (!g_read_seen || !g_read_log_path) return;
    f = fopen(g_read_log_path, "w");
    if (!f) return;
    for (i = 0; i < g_read_seen_n; i++)
        if (g_read_seen[i])
            fprintf(f, "%lu %d %d %d %d\n", (unsigned long)i,
                    g_read_block[i], g_read_recoff[i], g_read_group[i],
                    g_read_bit[i]);
    fclose(f);
}

static void fuse_read_log_note(size_t addr, int block, int rec_off,
                                int group, int bit) {
    if (!g_read_log_checked) {
        g_read_log_checked = 1;
        g_read_log_path = getenv("XC73_FUSE_READ_LOG");
        if (g_read_log_path) {
            g_read_seen_n = 200000;
            g_read_seen  = (unsigned char *)calloc(g_read_seen_n, 1);
            g_read_block  = (int *)calloc(g_read_seen_n, sizeof(int));
            g_read_recoff = (int *)calloc(g_read_seen_n, sizeof(int));
            g_read_group  = (int *)calloc(g_read_seen_n, sizeof(int));
            g_read_bit    = (int *)calloc(g_read_seen_n, sizeof(int));
            if (g_read_seen && g_read_block && g_read_recoff &&
                g_read_group && g_read_bit)
                atexit(fuse_read_log_flush);
            else g_read_log_path = NULL; /* calloc failed: stay silent,
                                           * never crash a real decode
                                           * over an instrumentation
                                           * side channel */
        }
    }
    if (g_read_seen && addr < g_read_seen_n && !g_read_seen[addr]) {
        g_read_seen[addr] = 1;
        g_read_block[addr] = block;
        g_read_recoff[addr] = rec_off;
        g_read_group[addr] = group;
        g_read_bit[addr] = bit;
    }
}

int jed_owned_ex(const JedFile *jf, size_t block, int rec_off, int group,
                 int bit, int dflt, int *status) {
    size_t want = block * 128 + (size_t)rec_off;
    size_t idx = (size_t)(group * 8 + bit);
    /* log `want + idx`, NOT `want` alone. `want`
     * only ever identifies the RECORD (its own L<addr> start, always a
     * multiple of 8 in every observed record); the census/mutation-scan
     * "address" space external analysis tools use
     * is the FLAT per-
     * bit position used by the file's own L-numbering (`addr+nbits` per
     * character, the "overlapping records" comment) -- i.e.
     * `want` for the group-0/bit-0 fuse, `want+1` for group-0/bit-1,
     * ... up to `want+39` for group-4/bit-7. Verified empirically
     * against a real record (`L00000` 40 bits, `L00008` 8 bits
     * starting INSIDE it) before trusting this, not assumed: logging
     * `want` alone would have made every non-record-start address
     * (7 of every 8, on this dialect's own row spacing) look
     * permanently "never read" regardless of whether decode.c ever
     * actually asked about it -- exactly the contradiction
     * a reconciliation step exists to catch. */
    if (group >= 0 && bit >= 0)
        fuse_read_log_note(want + idx, (int)block, rec_off, group, bit);
                                 /* no-op unless XC73_FUSE_READ_LOG is
                                  * set (checked once, cached) -- see
                                  * comment above */
    /* O(1) via `by_addr` instead of scanning `recs[]`
     * -- see jed.h's comment on the field. Falls back to the original
     * linear scan ONLY if the index failed to build (a malloc failure
     * at load time, `jed.c`'s own `jed_load`) or `want` falls outside
     * it (cannot happen for a real record, since every `recs[i].addr`
     * is `< by_addr_n` by construction, but a CALLER can ask about any
     * `(block,rec_off)` pair, including ones with no record at all and
     * arithmetically above `max_addr`) -- both cases correctly mean
     * "no record", never a wrong answer. */
    if (jf->by_addr && want < jf->by_addr_n) {
        size_t i = jf->by_addr[want];
        if (i == (size_t)-1) {
            if (status) *status = JED_NO_REC;
            return dflt;
        }
        if (idx < jf->recs[i].nbits) {
            if (status) *status = JED_OK;
            return jf->recs[i].bits[idx];
        }
        if (status) *status = JED_PAST_END;
        return dflt;
    }
    {   size_t i;
        for (i = 0; i < jf->n_recs; i++)
            if (jf->recs[i].addr == want) {
                if (idx < jf->recs[i].nbits) {
                    if (status) *status = JED_OK;
                    return jf->recs[i].bits[idx];
                }
                /* The record is HERE and is TOO SHORT. Not an unwritten
                 * fuse. */
                if (status) *status = JED_PAST_END;
                return dflt;
            }
    }
    if (status) *status = JED_NO_REC;
    return dflt;
}

int jed_owned(const JedFile *jf, size_t block, int rec_off, int group, int bit,
              int dflt) {
    return jed_owned_ex(jf, block, rec_off, group, bit, dflt, NULL);
}

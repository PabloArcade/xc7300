#ifndef JED_H
#define JED_H

#include <stddef.h>

/* A parsed JEDEC (.jed) fuse map in the XEPLD dialect. fuses[i] is 0 or 1. */
/* One L record, kept verbatim. The flat fuses[] array cannot represent the
 * XEPLD dialect faithfully: a record's five groups ARE one
 * macrocell's five product-term rows, and records for adjacent macrocells
 * legitimately describe the SAME addresses. Flattening loses which record
 * owns which value. */
typedef struct {
    size_t addr;
    size_t nbits;
    unsigned char *bits;      /* nbits entries, 0 or 1 */
} JedRec;

typedef struct {
    unsigned char *fuses;
    JedRec *recs;
    size_t n_recs;
    size_t count;             /* QF -- authoritative fuse count */
    size_t max_addr;          /* highest address touched by an L record */
    size_t n_records;         /* number of L records applied */
    int    default_state;     /* from F<n>; MAKEJED writes 1 */
    long   stated_checksum;   /* from the C<hex> field, or -1 if absent */
    char   device[128];       /* e.g. "XC7336-5PC44", taken from the line
                               * after STX -- XEPLD has no N DEVICE field */
    /* `jed_owned_ex` used to LINEAR-SCAN `recs[]` for
     * every single call -- fine for a real board (~1000 records, few
     * thousand calls) but catastrophic for a dense, PARTITION-forced
     * design (~2400 records, ~250 MILLION calls -- confirmed by gprof,
     * `jed_owned` alone was 92% of a 78-second decode). `by_addr[want]`
     * gives the SAME lookup (first record in file order whose `.addr`
     * equals `want`, matching the scan's own first-match semantics
     * exactly) in O(1). Built once, right after L-record parsing, in
     * `jed_load` -- sized to `max_addr+1`, every slot initialised to
     * `SIZE_MAX` ("no record starts here") before the real record
     * indices are written in, so absence is a single sentinel value
     * checked before ever indexing `recs[]`. */
    size_t *by_addr;          /* by_addr[addr] = index into recs[], or
                               * SIZE_MAX if no record starts at addr */
    size_t  by_addr_n;        /* allocated length of by_addr (max_addr+1) */
} JedFile;

/* Loads and parses a .jed file. Returns 0 on success, -1 on failure. */
int jed_load(const char *path, JedFile *out);

/* The owning-record read. Returns the fuse at (block, offset) as
 * written by the record that STARTS at `rec_off` in that block -- i.e. the
 * record belonging to the macrocell whose group base is rec_off. Returns
 * `dflt` when that record is absent or too short. */
int jed_owned(const JedFile *jf, size_t block, int rec_off, int group, int bit,
              int dflt);

/* Status codes for jed_owned_ex. `jed_owned` collapses the last two into
 * `dflt`, and that hid a real defect for a whole revision: with
 * the XC7354 FAST row base set to 1, row4 addressed group 5 of a 40-BIT
 * record -- five groups -- so every read fell off the end and returned the
 * default 1, which reads as a DISABLED row. A record that EXISTS but is too
 * short for the group being asked for means the ADDRESSING MODEL IS WRONG,
 * and it must not look like an unwritten fuse. */
#define JED_OK        0   /* The record exists and covers the group */
#define JED_NO_REC    1   /* no record starts here -- normal, take the default */
#define JED_PAST_END  2   /* The record EXISTS and is TOO SHORT -- model error */

int jed_owned_ex(const JedFile *jf, size_t block, int rec_off, int group,
                 int bit, int dflt, int *status);

void jed_free(JedFile *jf);

#endif

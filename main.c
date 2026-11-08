/* xc7300dump -- Xilinx XC7300 EPLD bitmap inspector.
 *
 * WHAT THIS IS, AT v1
 * -------------------
 * A fuse-map RESEARCH instrument, not a decompiler. It reads XEPLD JEDEC
 * bitmaps, reports what is actually known about them, and -- the point of the
 * exercise -- diffs matched pairs so the fuse map can be discovered the same
 * way the XC9500 one was: change ONE thing in the source design, refit, and
 * see exactly which fuses moved.
 *
 * It does NOT yet decode logic. The XC7300 fuse map is not known, and this
 * tool will not invent one. An unresolved field is honest; a wrong signal
 * name in a decompiler is a silent lie, and this project has paid for that
 * lesson repeatedly.
 */

#define _GNU_SOURCE
#include "jed.h"
#include "device.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define VERSION "xc7300 decoder tool by Darksoft v2.71 -- use at your own risk."

/* every decode command below assumes the XC7336 map -- 4 FB bits per
 * group, 9 macrocells, the geometry. That map is VALIDATED on
 * XC7336 and XC7318 (FFB-only, 4 and 2 function blocks). Applied to a bigger
 * part it produces a confident WRONG answer: on an XC7354 design it reported
 * 21 macrocells where the fitter's .RES says 33, and it cannot represent
 * FB5/FB6 at all. Refuse rather than mislead. */
/* ROWS and USED are validated on XC7354 as well (49 of 54 macrocells
 * measured, all six blocks). CONFIG, UIM and SIGNATURE are NOT -- they have
 * only ever been measured on XC7336/XC7318 -- so they keep the stricter
 * guard. A tool that decodes what it has measured and refuses the rest is
 * honest; one that applies an XC7336 config map to a High-Density part is
 * not. */
/* XC73144 gets its OWN message. It is a real part -- 16 function
 * blocks, 144 macrocells, 276 flip-flops, and the only device in the family
 * with FastFunctionBlock input-pad registers -- but `PARTLIST.XCT` has NO
 * ENTRY for it in any package, so XACT 5.0.0 cannot target it and this
 * project cannot generate ground truth for it. Saying "unsupported" would
 * imply the work simply has not been done; the truth is that the toolchain
 * we have CANNOT produce a single XC73144 bitmap. */
/* The same part, flagged rather than refused, for the commands that
 * are device-independent. `info`, `diff`, `rdiff` and `dump` read the JEDEC's
 * own structure and are meaningful on ANY device -- refusing them would hide
 * the one thing an XC73144 bitmap CAN be used for. But they must not be read
 * as endorsement: nothing downstream can decode the part. */
int device_is_73144_note(const JedFile *jf) {
    if (!strstr(jf->device, "XC73144")) return 0;
    fprintf(stderr,
      "NOTE: '%s' is an XC73144.\n"
      "  Structural output (info/diff/rdiff/dump) is valid -- it reads the\n"
      "  JEDEC itself. But this part CANNOT BE DECODED: no available tool\n"
      "  writes an XC73144 true fuse map. `decode` and\n"
      "  `uim` will refuse.\n\n", jf->device);
    return 1;
}

int device_is_73144(const JedFile *jf) {
    if (!strstr(jf->device, "XC73144")) return 0;
    fprintf(stderr,
      "REFUSING to decode '%s'.\n"
      "\n"
      "XC73144 IS A REAL PART -- 16 function blocks, 144 macrocells, 276\n"
      "flip-flops, and the ONLY device in the family with FastFunctionBlock\n"
      "input-pad registers. It is NOT a gap in this tool's coverage.\n"
      "\n"
      "IT IS NOT DECODABLE WITH THE TOOLS THAT EXIST.\n"
      "XACT 6 CAN fit it -- full .EQN/.MAP/.PIN/.RES reports, a .VM6\n"
      "database and a programmer image all exist -- but XACT 6 has no\n"
      "TRUE-FUSE writer, and XACT 5, which has one, does not know the part:\n"
      "\n"
      "    v5 MAKEJED + v5 HPREPORT  ->  rg37: device type 73144160 is not\n"
      "                                  supported in this release\n"
      "    `strings HPREPORT.EXE | grep -c 73144`  ->  0\n"
      "    v6's own writers          ->  \"Programmer Jedec Bit Map\", QF39296\n"
      "                                  -- the PROGRAMMER address space,\n"
      "                                     NOT the fuse array\n"
      "\n"
      "Nor can the fuse count be inferred: QF/dump-address is 32.09, 32.09,\n"
      "42.36, 41.79 and 98.60 across the five measured devices -- no fixed\n"
      "multiple, and not linear in macrocells. XC7318 and XC7336 share BOTH\n"
      "their QF and their dump, so QF is a property of the DIE.\n"
      "\n"
      "The 1996 EPLD Application Guide (Xilinx 0401363, p46) says:\n"
      "    \"the PINOUT COMPATIBLE XC73144 can be used as a DROP IN\n"
      "     REPLACEMENT to the XC73108\"\n"
      "so if an XC73144 bitmap ever arrives from a real device or another\n"
      "toolchain, the PC84 pinout measured for XC73108 -- 60 of 108\n"
      "macrocells bonded -- applies to it.\n"
      "\n"
      "Decoding one here would apply a map measured on a DIFFERENT DEVICE\n"
      "and yield a WRONG answer, not a partial one.\n", jf->device);
    return 1;
}

static int device_rows_supported(const JedFile *jf) {
    if (device_is_73144(jf)) return 0;
    if (strstr(jf->device, "XC7336") || strstr(jf->device, "XC7318")
        || strstr(jf->device, "XC7354") || strstr(jf->device, "XC7372")
        || strstr(jf->device, "XC73108")) return 1;
    fprintf(stderr,
        "REFUSING to decode '%s'.\n"
        "Row decoding is validated for XC7336, XC7318 and XC7354 only.\n", jf->device);
    return 0;
}

static int device_supported(const JedFile *jf) {
    if (strstr(jf->device, "XC7336") || strstr(jf->device, "XC7318")) return 1;
    fprintf(stderr,
        "REFUSING to decode '%s'.\n"
        "The fuse map is validated for XC7336 and XC7318 only (FFB-only\n"
        "parts). Larger devices have more function blocks than this map\n"
        "encodes and High-Density blocks it does not model at all; decoding\n"
        "one here yields a WRONG answer, not a partial one.\n"
        "`info`, `dump`, `diff` and `rdiff` still work on any device.\n",
        jf->device);
    return 0;
}

static void usage(void) {
    printf("%s\n\n", VERSION);
    printf("Usage:\n");
    printf("  xc7300dump info <file.jed>\n");
    printf("        device, QF, record structure, fuse census\n");
    printf("  xc7300dump diff <a.jed> <b.jed> [--max N]\n");
    printf("        every differing fuse address between two bitmaps\n");
    printf("  xc7300dump dump <file.jed> <from> <count>\n");
    printf("        raw fuse values over an address range\n");
    printf("  xc7300dump rdiff <a.jed> <b.jed> [--max N]\n");
    printf("        RECORD-BY-RECORD diff, and names the row/slot\n");
    printf("        USE THIS, not `diff` -- see README\n");
    printf("  xc7300dump decode <file.jed> [outbase]\n");
    printf("        DECOMPILE: emit <outbase>.PLD + .VMF for XEPLD\n");
    printf("  xc7300dump signature <file.jed>\n");
    printf("        the MAKEJED -s signature, ASCII\n");
    printf("  xc7300dump uim <file.jed>\n");
    printf("        UIM crosspoints; both source bands named BY RULE\n");
    printf("  xc7300dump config <file.jed> <fb> <mc>\n");
    printf("        macrocell configuration, 9 of 13 bits named\n");
    printf("  xc7300dump rows <file.jed> <fb> <mc>\n");
    printf("        a macrocell's five PRODUCT-TERM rows, decoded\n");
    printf("  xc7300dump col <file.jed> <fb> <mc> [blocks]\n");
    printf("        (colat <file.jed> <offset> [blocks] for a raw offset)\n");
    printf("        one macrocell's COLUMN down the blocks\n");
    printf("  xc7300dump used <file.jed>\n");
    printf("        macrocells in use, MC1..MC9, all four FBs\n");
    printf("  xc7300dump readback <dump.prg> <out.jed> <template.jed>\n");
    printf("        a PROGRAMMER DUMP of a real part -> a JEDEC.\n"
           "        The template is any MAKEJED bitmap for the device: the\n"
           "        fitter's record set says which addresses are which\n"
           "        structure, and a real chip has data elsewhere\n");
    printf("  xc7300dump selftest [--fail]\n");
    printf("        structural invariants of the device profiles;\n"
           "        --fail reintroduces a known defect and MUST report it\n");
    printf("  xc7300dump devices\n");
    printf("        the device table and what is measured vs unknown\n");
    printf("  xc7300dump pins <device> <package>\n");
    printf("        pin -> macrocell map, where known\n");
    printf("  xc7300dump vmhroutes <file.vmh> | --selftest\n");
    printf("        dumps 'bits'/'routes' framed records\n"
           "        -- COUNT/BYTE-level diagnostic only, not a\n"
           "        route decoder; --selftest checks the parser itself\n");
}

/* Split "XC7336-5PC44" into family and package. */
void split_part(const char *s, char *fam, size_t fl,
                       char *pkg, size_t pl) {
    size_t i = 0;
    fam[0] = pkg[0] = '\0';
    while (s[i] && s[i] != '-' && i < fl - 1) { fam[i] = s[i]; i++; }
    fam[i] = '\0';
    if (s[i] == '-') {
        /* skip the speed grade digits, keep the package letters+digits */
        size_t j = i + 1, k = 0;
        while (s[j] && s[j] >= '0' && s[j] <= '9') j++;
        while (s[j] && k < pl - 1) { pkg[k++] = s[j++]; }
        pkg[k] = '\0';
    }
}

/* report the TARGET the way the xc9500 tool does -- what was asked
 * for, what the JEDEC actually carries, and what the two resolve to.
 *
 * This tool has no `-p` switch: the device always comes FROM the bitmap. So
 * "requested" is honestly reported as `(none -- taken from the JEDEC)` rather
 * than echoing the found value back and implying a check that did not happen.
 *
 * The package can genuinely be absent -- a bare `XC7336` header parses with
 * an empty package, and `split_part` leaves it so. That case prints
 * **not found**, because a target resolved without a package is a target the
 * pinout cannot be trusted for. */
/* THE TARGET GATE.
 *
 * Rule: a bitmap that does not state its package must not be decoded
 * on an assumption -- the tool must STOP and ask for one. And a device the
 * tool does not support must be named as unsupported, not silently carried
 * as "UNKNOWN".
 *
 * Before v2.23 both cases proceeded. A header of bare `XC7354` decoded and
 * wrote `733644 XC7354` into the `.VMF` -- a target with no package, whose
 * PINOUT IS UNDEFINED, since every pin table in this project is per-package.
 * The output looked ordinary and was unusable.
 *
 * `g_package` is set by `--package PC84` and overrides an absent one. It
 * cannot override a package the JEDEC states: a bitmap that says PC68 is a
 * PC68 bitmap, and letting a switch contradict it would invent a target.
 *
 * Returns 0 to proceed, non-zero to stop. */
const char *g_package = NULL;

/* GEOMETRY vs DATA. Reading one device's fuse map with another's
 * geometry reads PAST THE END of the array -- at best garbage, at worst a
 * crash. `info` printed "*** QF MISMATCH ***" and every other command carried
 * on regardless: a 69,984-fuse file labelled XC7354 (which needs 243,648)
 * decoded and WROTE A .PLD.
 *
 * Refuse instead, and say both numbers. A short file is usually a truncated
 * download or a device mislabelled by hand; either way nothing downstream can
 * be trusted. */
int geometry_gate(const JedFile *jf) {
    char fam[64], pkg[32];
    const device_t *dev;
    split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
    dev = fam[0] ? device_find(fam) : NULL;
    if (!dev || !dev->qf) return 0;              /* unknown device: elsewhere */
    if ((long)jf->count == dev->qf) return 0;
    fprintf(stderr,
      "ERROR: %s expects %ld fuses, this JED has %lu. Refusing --\n"
      "  decoding one device's fuse map with another's geometry reads past\n"
      "  the end of the data.\n"
      "\n"
      "  Most likely the file is TRUNCATED, or its device line does not match\n"
      "  its contents. `xc7300dump info %s` shows both numbers.\n",
      jf->device, dev->qf, (unsigned long)jf->count,
      "<file.jed>");
    return 2;
}

int target_gate(const JedFile *jf, int needs_pinout) {
    char fam[64], pkg[32];
    const device_t *dev;
    split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
    dev = fam[0] ? device_find(fam) : NULL;

    if (!fam[0]) {
        fprintf(stderr,
          "ERROR: no device string in '%s'.\n"
          "  The JEDEC carries no XC7xxx part name, so there is nothing to\n"
          "  resolve a target from. This is not a supported input.\n", jf->device);
        return 2;
    }
    if (!dev) {
        fprintf(stderr,
          "ERROR: DEVICE NOT SUPPORTED -- '%s'.\n"
          "  Supported: XC7318 XC7336 XC7336Q XC7354 XC7372 XC73108 XC7236A.\n"
          "  XC73144 is a real part and is NOT decodable.\n"
          "  Run `xc7300dump devices` for the table.\n", fam);
        return 2;
    }
    if (!needs_pinout) return 0;
    if (pkg[0]) return 0;
    if (g_package && g_package[0]) return 0;

    fprintf(stderr,
      "ERROR: NO PACKAGE in '%s', and this command needs the pinout.\n"
      "  The JEDEC states the model (%s) but no package, so the pin ->\n"
      "  macrocell map is undefined -- every pin table here is per-package.\n"
      "  This tool will NOT guess one.\n"
      "\n"
      "  Supply it:   xc7300dump <cmd> <file.jed> --package PC84\n"
      "\n"
      "  Packages for %s: run `xc7300dump pins %s <PKG>` to see a pinout.\n",
      jf->device, fam, fam, fam);
    return 2;
}

void print_target(const JedFile *jf) {
    char fam[64], pkg[32];
    split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
    printf("model requested   : (none -- taken from the JEDEC)"
           "   found in jed: %s\n", fam[0] ? fam : "not found");
    printf("package requested : %-28s   found in jed: %s\n",
           (g_package && g_package[0]) ? g_package : "(none -- taken from the JEDEC)",
           pkg[0] ? pkg : "**not found**");
    if (!pkg[0] && g_package && g_package[0]) {
        printf("target            : %s-%s  (package SUPPLIED on the command line)\n\n",
               fam, g_package);
        return;
    }
    if (fam[0] && pkg[0]) printf("target            : %s-%s\n\n", fam, pkg);
    else if (fam[0])      printf("target            : %s  "
                                 "(NO PACKAGE -- pinout unavailable)\n\n", fam);
    else                  printf("target            : **UNRESOLVED**\n\n");
}

static int cmd_info(const char *path) {
    JedFile jf;
    size_t i, ones = 0, zeros = 0;
    char fam[64], pkg[32];
    const device_t *dev;

    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    device_is_73144_note(&jf);
    print_target(&jf);
    if (geometry_gate(&jf)) { jed_free(&jf); return 2; }
    if (target_gate(&jf, 0)) { jed_free(&jf); return 2; }
    for (i = 0; i < jf.count; i++) {
        if (jf.fuses[i]) ones++; else zeros++;
    }
    split_part(jf.device, fam, sizeof fam, pkg, sizeof pkg);
    dev = device_find(fam);

    printf("file            : %s\n", path);
    printf("part            : %s\n", jf.device[0] ? jf.device : "(none stated)");
    printf("family          : %s\n", dev ? dev->name : "UNKNOWN");
    if (dev) {
        printf("  function blocks: %d\n", dev->n_fbs);
        printf("  macrocells     : %d\n", dev->n_mcs);
        if (dev->qf) {
            printf("  expected QF    : %ld%s\n", dev->qf,
                   dev->qf_measured ? "  (measured)" : "  (UNVERIFIED)");
            if ((long)jf.count != dev->qf)
                printf("  *** QF MISMATCH: file says %lu ***\n",
                       (unsigned long)jf.count);
        } else {
            printf("  expected QF    : NOT KNOWN for this device\n");
        }
    }
    printf("package         : %s\n", pkg[0] ? pkg : "(unknown)");
    printf("\n");
    printf("QF (fuse count) : %lu\n", (unsigned long)jf.count);
    printf("default state   : F%d\n", jf.default_state);
    printf("L records       : %lu\n", (unsigned long)jf.n_records);
    printf("highest address : %lu  (%.1f%% of QF explicitly covered)\n",
           (unsigned long)jf.max_addr,
           jf.count ? 100.0 * (double)jf.max_addr / (double)jf.count : 0.0);
    printf("fuses set   (1) : %lu\n", (unsigned long)ones);
    printf("fuses clear (0) : %lu   (%.2f%% of the array)\n",
           (unsigned long)zeros,
           jf.count ? 100.0 * (double)zeros / (double)jf.count : 0.0);
    if (jf.stated_checksum >= 0)
        printf("stated checksum : C%04lX  (NOT VERIFIED -- see README)\n",
               jf.stated_checksum);
    jed_free(&jf);
    return 0;
}

/* Which macrocell/row/slot a differing bit belongs to, when the record it
 * sits in is a known group base. Returns 1 if it could be named. */
/* plain-typed shim: `DevProfile` is declared further down the file. */
static int fb_of_rowbit_jed(const JedFile *jf, int bit);


/* compare RECORD BY RECORD. The flat overlay made designs with
 * different equations look identical, because a record's five groups are one
 * macrocell's five product-term rows and adjacent macrocells' records
 * legitimately describe the same addresses. */
static int name_bit(const JedFile *jf, int rec_off, int group, int bit,
                    int blk, char *out, size_t cap);

/* PER-RECORD comparison, re-applied. The full argument, because it
 * was reverted once:
 *
 * made `rdiff` walk the RESOLVED `fuses[]` array to kill "phantom"
 * differences. Measurement showed those phantoms were REAL: `PP`/`PN` differ in
 *   exactly one property (a polarity, confirmed by decoding both) and a
 *   polarity change costs hundreds of fuses -- the known pair `P3A`/`P3B`
 *   costs 314 per record. `PP`/`PN` reporting ONE flat fuse was the flat view
 *   hiding 101 of 102.
 *
 *   The decoder itself is record-based: `jed_owned_ex` matches an EXACT record
 *   address and reads that record's bits, never `fuses[]`. So a flat `rdiff`
 *   cannot see changes the decoder acts on -- `U1`/`U2` differ by one input
 *   pin, decode to different equations, and flat-diff to ZERO.
 *
 * And the per-record comparison is what produced the bitmap
 *   results: 37 of 38, 13 of 13 and 6 of 6 IDENTICAL against saturated
 *   signature fields. A comparison that manufactured phantoms could not do
 *   that.
 *
 * Records are matched BY ADDRESS, which is also what avoids the original
 * complaint -- that version paired records by POSITION and compared stale
 * spans. No XC7300 file has duplicate record addresses (2430 records, 0
 * duplicates, measured), so the match is unambiguous. */
static const JedRec *rec_at(const JedFile *f, size_t addr) {
    size_t i;
    for (i = 0; i < f->n_recs; i++) if (f->recs[i].addr == addr) return &f->recs[i];
    return NULL;
}

static int cmd_diff_recs(const char *pa, const char *pb, long maxshow) {
    JedFile a, b;
    size_t i, diffs = 0, shown = 0, only = 0;
    if (jed_load(pa, &a) != 0) { fprintf(stderr, "cannot read %s\n", pa); return 1; }
    device_is_73144_note(&a);
    if (jed_load(pb, &b) != 0) { fprintf(stderr, "cannot read %s\n", pb); jed_free(&a); return 1; }
    printf("A: %-28s %s\nB: %-28s %s\n\n", pa, a.device, pb, b.device);
    for (i = 0; i < a.n_recs; i++) {
        const JedRec *ra = &a.recs[i];
        const JedRec *rb = rec_at(&b, ra->addr);
        size_t k, n;
        if (!rb) { only++; continue; }
        n = ra->nbits < rb->nbits ? ra->nbits : rb->nbits;
        for (k = 0; k < n; k++) {
            if (ra->bits[k] == rb->bits[k]) continue;
            diffs++;
            if (maxshow < 0 || shown < (size_t)maxshow) {
                char nm[128];
                int off = (int)(ra->addr % 128), blk = (int)(ra->addr / 128);
                if (!name_bit(&a, off, (int)(k / 8), (int)(k % 8), blk, nm, sizeof nm))
                    snprintf(nm, sizeof nm, "(unnamed region)");
                printf("  record L%05lu group %lu bit %lu : %d -> %d   %s\n",
                       (unsigned long)ra->addr, (unsigned long)(k / 8),
                       (unsigned long)(k % 8), ra->bits[k], rb->bits[k], nm);
                shown++;
            }
        }
    }
    if (only) printf("\n  records in A with no counterpart in B : %lu\n",
                     (unsigned long)only);
    printf("\ndiffering record bits : %lu\n", (unsigned long)diffs);
    jed_free(&a); jed_free(&b);
    return 0;
}

/* batch naming lookup, reusing `name_bit()` DIRECTLY
 * (rule 4: one address, one place) -- no decode, no mutation, no second
 * file. Reads lines "addr block rec_off group bit" (the exact format
 * src/jed.c's own `XC73_FUSE_READ_LOG` instrumentation writes) from
 * `tuples_path` and prints "addr NAMED name" or "addr UNNAMED" for each,
 * against the device profile of `jed_path` (only its device string
 * matters -- `name_bit` needs `profile_for(jf)`/`device_row_per_record(jf)`,
 * not this file's own fuse content). */
static int cmd_namebits(const char *jed_path, const char *tuples_path) {
    JedFile jf;
    FILE *f;
    char line[256];
    if (jed_load(jed_path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", jed_path); return 1;
    }
    f = fopen(tuples_path, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", tuples_path); jed_free(&jf); return 1; }
    while (fgets(line, sizeof line, f)) {
        long addr, block, rec_off, group, bit;
        char nm[128];
        if (sscanf(line, "%ld %ld %ld %ld %ld", &addr, &block, &rec_off,
                   &group, &bit) != 5)
            continue;
        if (name_bit(&jf, (int)rec_off, (int)group, (int)bit, (int)block,
                      nm, sizeof nm))
            printf("%ld NAMED %s\n", addr, nm);
        else
            printf("%ld UNNAMED\n", addr);
    }
    fclose(f);
    jed_free(&jf);
    return 0;
}

static int cmd_diff(const char *pa, const char *pb, long maxshow) {
    JedFile a, b;
    size_t i, n, diffs = 0, shown = 0;
    size_t first = 0, last = 0;
    int have_first = 0;

    if (jed_load(pa, &a) != 0) { fprintf(stderr, "cannot read %s\n", pa); return 1; }
    device_is_73144_note(&a);
    if (jed_load(pb, &b) != 0) { fprintf(stderr, "cannot read %s\n", pb); jed_free(&a); return 1; }

    if (a.count != b.count) {
        printf("NOTE: fuse counts differ -- %lu vs %lu. Comparing the overlap "
               "only.\n", (unsigned long)a.count, (unsigned long)b.count);
    }
    printf("A: %-28s %s\n", pa, a.device);
    printf("B: %-28s %s\n", pb, b.device);
    printf("\n");

    n = a.count < b.count ? a.count : b.count;
    for (i = 0; i < n; i++) {
        if (a.fuses[i] != b.fuses[i]) {
            diffs++;
            if (!have_first) { first = i; have_first = 1; }
            last = i;
            if (maxshow < 0 || (long)shown < maxshow) {
                printf("  %8lu  %d -> %d\n", (unsigned long)i,
                       a.fuses[i], b.fuses[i]);
                shown++;
            }
        }
    }
    if (maxshow >= 0 && diffs > shown)
        printf("  ... %lu more (use --max -1 for all)\n",
               (unsigned long)(diffs - shown));
    printf("\ndiffering fuses : %lu of %lu\n",
           (unsigned long)diffs, (unsigned long)n);
    if (diffs)
        printf("address span    : %lu .. %lu\n",
               (unsigned long)first, (unsigned long)last);
    jed_free(&a);
    jed_free(&b);
    return 0;
}

static int cmd_dump(const char *path, long from, long count) {
    JedFile jf;
    long i;
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    device_is_73144_note(&jf);
    for (i = from; i < from + count && i < (long)jf.count; i++) {
        if ((i - from) % 32 == 0) printf("\n%8ld  ", i);
        else if ((i - from) % 8 == 0) printf(" ");
        printf("%d", jf.fuses[i]);
    }
    printf("\n");
    jed_free(&jf);
    return 0;
}

/* The GROUP selects the macrocell and the BIT within it selects the function
 * block. Neither ordering is monotonic.
 *
 * read block 0 of this table as a MACROCELL-IN-USE flag. That label is
 * FALSIFIED: block 0 at group_base+fb_bit is the slot-0 COMPLEMENT
 * line of the macrocell's AND-array row, and it reads 1 only when the
 * implemented product term uses a complemented literal in slot 0. The five
 * designs that validated all had equations the fitter had inverted, so
 * they all happened to set it. On a design whose equations are NOT inverted it
 * reads 0 for a macrocell that IS in use -- measured 3 of 12 on RICH. */
typedef struct {
    const char *name;
    int nfb;            /* function blocks */
    int fbbit[12];      /* was [6]; XC7372 has 8 blocks, XC73108 has 12 */       /* FB n (1-based) -> bit within the group */
    int fast_upto;      /* FB 1..fast_upto are FAST; above are High-Density */
    int row_g_fast;     /* first row group, FAST blocks */
    int row_g_hd;       /* first row group, High-Density blocks */
    int split_mc1;      /* XC7336 splits MC1 across records 88 and 90 */
    /* configuration block ranges. The OFFSET/GROUP rule is identical
     * on every device -- mc>=5 uses offset 96 group 9-mc, mc<5 uses offset
     * 104 group 4-mc -- and the BIT is the function-block bit. What changes
     * is the BLOCK, and on XC7354 the Fast and High-Density blocks use
     * entirely separate ranges. -1 = not measured on this device. */
    int cfg_fast[8];    /* OEused, OEsel, reg, fclk, toggle, pwr, set, reset */
    int cfg_hd[8];
    int cfg_fbbit[12];   /* config FB bit map for the HIGH-DENSITY range */
    /* on XC7354 the FAST range uses a DIFFERENT FB bit assignment
     * from the High-Density range, and reusing one array for both was wrong.
     * Measurement showed FB1 -> bit 3 and FB2 -> bit 2 in the FAST range while
     * the row/UIM map gives FB1 -> 0, FB2 -> 1; the profile carried the row
     * map. -1 = the block is not in this range, or was never measured. */
    int cfg_fbbit_fast[12];
    /* `cfg_fast`/`cfg_hd` hold only eight attributes and had no slot
     * for the product-term ALLOCATION and POLARITY bits, so cmd_config
     * hardcoded them. Every hardcoded address is an address written TWICE,
     * and methodology rule 4 exists because the two copies drift. */
    int cfg_fbinuse;    /* blk: read AT MC8 ONLY. 0 = this FUNCTION BLOCK has
                         * logic placed in it. Per-BLOCK, not per-macrocell. */
    int cfg_localfb;    /* blk: 0 = this macrocell feeds another in the SAME
                         * function block, over the FFB's own feedback path
                         * rather than through the UIM */
    int cfg_alloc;      /* blk: logic in rows 1..4 (1) or row0 (0), */
    int cfg_polarity;   /* blk: the fitter inverted the equation, */
    /* The RAW window to print. It must be WIDER than the named attributes:
     * blocks 11, 16, 17, 22 and 23 are deliberately shown unnamed,
     * and the epilogue below discusses them by number.
     * Deriving the window from the named blocks alone silently dropped every
     * block the epilogue talks about. -1 = the extent is NOT established on
     * this device, and the printed span is then the located one. */
    int cfg_raw_lo, cfg_raw_hi;
    /* The UIM crosspoint block extent. XC7336's 48..125 is NOT a
     * family constant -- on XC7354, record-bits in the UIM offsets vary as
     * high as block 234, so scanning an XC7336 window there would
     * silently truncate. -1 = the extent is NOT established on this device. */
    int uim_blk_lo, uim_blk_hi;
    /* XC73108 packs ONE ROW per 16-bit record at OFFSET STRIDE 4,
     * where every smaller part packs FIVE rows as five GROUPS of one 40-bit
     * record. `row_per_record` selects that addressing. */
    int row_per_record;
} DevProfile;
static const DevProfile prof_7336 = { "XC7336", 4, {0,2,1,3,-1,-1}, 4, 0, 0, 1,
    /* XC7336 keeps ONE range for all four FFBs, and its config bit
     * map is validated -- 34 designs, 0 mismatch. Both entries are the same
     * map on purpose. */
    {12,13,14,15,21,17,20,20}, {-1,-1,-1,-1,-1,-1,-1,-1}, {0,2,1,3,-1,-1},
    /* The UIM region is blocks 64..125. The scan used
     * to start at 48, which is below the block the slot rule is defined
     * from (i = 2*(blk-64)), so blocks 48..63 yielded NEGATIVE slots. No
     * fitter bitmap has records there, so it never showed until a real
     * readback did. */
    {0,2,1,3,-1,-1}, 23, 16, 18, 19, 11, 23, 64, 125 };
/* row_g_fast was 1 and is now 0. An early reading had "FAST blocks start at
 * GROUP 1" from a scanner that located the FIRST single-literal row of the
 * probe `/Q0 := /A + /B`. That probe has TWO terms, and a two-term equation
 * is allocated to rows 1..4 (blk18=1) -- so group 1 is where the
 * fitter PUT the first term, not where the row window BEGINS. Every macrocell
 * group base carries a 40-BIT record, which is exactly five 8-fuse groups, so
 * a five-row window can only start at group 0: with g0=1 row4 lands at group
 * 5, past the end of the record, where jed_owned() returns the default 1 and
 * the row reads DISABLED unconditionally. Group 0 was never read at all, so a
 * SINGLE-TERM macrocell (blk18=0, logic in row0) decoded as entirely empty
 * and was reported UNUSED. Measured on PORT_4, the only dense XC7354 design
 * in the corpus: CLR_RFRQ at FB1_MC1 and DONE_INV at FB1_MC6 now decode as
 * the 5- and 6-literal terms the fitter's own .EQN states. */
static const DevProfile prof_7354 = { "XC7354", 6, {0,1,2,5,4,3,-1,-1,-1,-1,-1,-1}, 2, 0, 0, 0,
    /* measured: FAST blocks 0..5, HIGH-DENSITY 10..46 */
    { 0, 1, 2, 3, 4, 5,-1,-1}, {17,18,45,46,21,10,41,21},
    /* HIGH-DENSITY range bit map. FB3=2, FB5=4, FB6=3 are matched
     * pairs; FB5 is the ONLY bit of eight that
     * tracks registered-vs-combinatorial. **FB4 was NEVER measured** -- it
     * did not fit at MC5 and every FB4 macrocell in the corpus is
     * registered, so all eight bits agree vacuously. It is -1, not 5: 5 came
     * from the ROW map and no config measurement supports it. */
    /* blk41/blk42 are a TWO-BIT ASYNC field on a High-Density
     * macrocell, not the single "async set" bit listed:
     *     (1,1) no async term   (1,0) RESET   (0,1) SET
     * 90 of 90 designs crossing {none, RSTF, SETF} with {1,2,4} terms in
     * FB3..FB6 -- FAST blocks are excluded because PLUSASM refuses `.RSTF`
     * there, cl447), which is why the attempt could not
     * work. cfg_hd[6] carries blk41 and blk42 is named beside it. */
    /* HIGH-DENSITY range bit map, ALL FOUR BLOCKS NOW MEASURED.
     * FB3=2, FB5=4, FB6=3 were matched pairs; **FB4=5 is measured
     * here for the first time** and was -1 until now.
     *
     * The instrument is one combinatorial/registered matched pair per block,
     * placed by PARTITION and CONFIRMED IN THE FITTER'S OWN `.MAP` before a
     * fuse was read, then compared with `rdiff`. Every pair moves EXACTLY ONE
     * bit, at offset 96 group 4 (MC5) or offset 104 group 3 (MC1), attribute
     * block 45 -- and the bit INDEX differs per function block, which is the
     * discrimination measurement showed this corpus could not supply. FB3, FB5 and
     * FB6 reproduce their previously measured values, so they are the
     * control: an instrument that invented FB4 would have had to invent
     * three known answers correctly first.
     *
     * FB4 could not be probed at MC5 and it still cannot: FB4_MC4,
     * MC5 and MC6 have NO PACKAGE PIN on PC68, so `OUTPUTPIN` there
     * fails with `es31: Could not fit equation 'Q' into FB4-5!`. MC1 has a
     * pad, exercises the offset-104 half of the rule, and fits.
     *
     * NOTE this makes the HD range identical to the ROW map on FB3..FB6.
     * That is a MEASUREMENT that happens to coincide, not the row map carried
     * across -- carrying it is exactly what did wrong, and the FAST
     * range still differs (config FB1=3, FB2=2 against row FB1=0, FB2=1). */
    {-1,-1, 2, 5, 4, 3},
    /* FAST range bit map, re-confirmed. FB1=3 is the only
     * bit of eight that tracks registered-vs-combinatorial over 21 samples
     * including 2 combinatorial. FB2=2 rests on the matched pair
     * ALONE: every FB2 macrocell in the corpus is registered, so this corpus
     * cannot re-derive it. FB3..FB6 are not in this range. */
    { 3, 2,-1,-1,-1,-1}, -1, -1,
    /* POLARITY on a HIGH-DENSITY macrocell is carried by blocks
     * 21..24 MOVING AS ONE FOUR-BIT FIELD -- 0000 not inverted, 1111
     * inverted -- over 85 designs, 40 inverted and 45 kept, measured INSIDE
     * The HD block type alone so there is no block-type confound.
     * Only TWO of the field's sixteen codes appeared, because none of these
     * designs took an ALU form; the field is presumed the ALU's function
     * generator and the other fourteen codes are NOT measured. blk21 is the
     * representative bit.
     * On FAST blocks inversion NEVER VARIED -- 60 designs, all inverted --
     * so no FAST polarity bit is claimed, and this is one value per block
     * type rather than a proven constant.
     * ALLOCATION is still NOT located on either block type. */
    -1, 21,
    /* "The extents of both ranges are unknown." Not invented. */
    -1, -1,
    /* MEASURED. The pin-source campaign put crosspoints as low as
     * block 64 and as high as block 204, so the scan runs 64..239. XC7336's
     * 48..125 does NOT transfer, as already showed. */
    64, 239 };
/* XC7372 is XC7354's structure with more blocks -- its records reach
 * block 253 where XC7354 stops at 239, so the UIM scan missed FB5 and FB6
 * entirely and their feedback crosspoints simply did not appear. Same profile
 * otherwise. */
static DevProfile prof_7372;
static int prof_7372_init;
static DevProfile prof_73108;
static int prof_73108_init;
static DevProfile prof_7236a;
static int prof_7236a_init;
static DevProfile prof_7318;
static int prof_7318_init;

static const DevProfile *profile_for(const JedFile *jf) {
    if (strstr(jf->device, "XC7236A")) {
        if (!prof_7236a_init) {
            /* XC7236A was falling through to
             * `prof_7336` -- 4 blocks, 36 macrocells, geometry that DOES
             * match XC7336 -- but a trivial 1-macrocell design's own
             * `.EQN` says "; High density function block" and its `.MAP`
             * reports "UIM inputs - 0/21  Shared pterms - 0/12", the
             * SAME shape as an XC7354/XC7372 HIGH-DENSITY block, not
             * XC7336's FFB shape ("Total inputs -n/24"). XC7236A is FOUR
             * HIGH-DENSITY BLOCKS AND ZERO FAST BLOCKS -- an architecture
             * this codebase has never modelled (every other device is
             * either all-FAST or a FAST+HD mix).
             *
             * The reported symptom (".MAP says FB2-7, decoder says
             * FB3_MC7") is prof_7336's `fbbit[]` -- MEASURED on XC7336,
             * where FB2 and FB3 do not sit at the row bits their number
             * suggests. Four single-macrocell designs, one per block,
             * `rdiff`'d pairwise against FB1 (the same method
             * used for XC7372): bit0->FB1, bit1->FB2, bit2->FB3,
             * bit3->FB4 -- a plain LINEAR order, unlike XC7336's
             * {1,3,2,4}. CONFIRMED, not guessed: `PARTITION FBn_7` in
             * the source, `rdiff` against a same-macrocell FB1 control,
             * one differing bit family per pair.
             *
             * fast_upto=0 (inherited from prof_7354's HD-only formulas,
             * NOT prof_7336's) follows directly from the same .EQN/.MAP
             * evidence. cfg_fbbit[] below is INHERITED FROM XC7354
             * WHOLESALE, the caution applies verbatim: NOT
             * measured on this device, and XC7354's own FB4 was never
             * measured either (T6, not fact) -- registered/clocked
             * XC7236A designs may decode wrong even once combinational
             * ones round-cycle. */
            prof_7236a = prof_7354;
            prof_7236a.name = "XC7236A";
            prof_7236a.nfb = 4;
            prof_7236a.fast_upto = 0;
            {   static const int rb[4] = {0,1,2,3};
                int i;
                for (i = 0; i < 4; i++) {
                    prof_7236a.fbbit[i] = rb[i];
                    prof_7236a.cfg_fbbit[i] = prof_7354.cfg_fbbit[i + 2];
                }
                for (i = 4; i < 12; i++) {
                    prof_7236a.fbbit[i] = -1;
                    prof_7236a.cfg_fbbit[i] = -1;
                }
                for (i = 0; i < 12; i++) prof_7236a.cfg_fbbit_fast[i] = -1;
            }
            prof_7236a_init = 1;
        }
        return &prof_7236a;
    }
    if (strstr(jf->device, "XC73108")) {
        if (!prof_73108_init) {
            /* EVERY TABLE BELOW IS MEASURED ON THIS DEVICE.
             * No field is inherited without a matched pair confirming it --
             * the first device in this project to reach a profile with no
             * T6 entry (method 1h). */
            prof_73108 = prof_7354;
            prof_73108.nfb = 12;
            prof_73108.row_per_record = 1;
            /* row bit map, one design per block, 12/12.
             * It is NOT XC7372's -- only FB1 and FB2 agree. */
            {   static const int rb[12] = {0,1,8,4,2,6,10,11,7,3,5,9};
                int i; for (i = 0; i < 12; i++) prof_73108.fbbit[i] = rb[i];
                /* The UIM SLOT bit IS the row bit, so one table
                 * serves both -- measured on both sides, unlike */
                for (i = 0; i < 12; i++) prof_73108.cfg_fbbit[i] = rb[i];
            }
            /* The CONFIG MAP transfers from XC7354 WHOLESALE --
             * registered blk2/blk45, OE blk0/blk17-18, polarity blk21..24,
             * async blk41 -- established with sixteen matched pairs. */
            /* XC73108's UIM spans blocks 2..381 -- the macrocell
             * slot records sit at blk2 and blk10, far
             * below the 64 every smaller device uses. */
            prof_73108.uim_blk_lo = 2;
            prof_73108.uim_blk_hi = 381;        /* record scan */
            prof_73108_init = 1;
        }
        return &prof_73108;
    }
    if (strstr(jf->device, "XC7372")) {
        if (!prof_7372_init) {
            prof_7372 = prof_7354;
            prof_7372.uim_blk_hi = 253;
            /* EIGHT function blocks, and the row bit map is not
             * XC7354's six with two appended. Measured by matched pairs --
             * one macrocell per block, identical logic, rdiff against FB1:
             *   FB1 0  FB2 1  FB3 2  FB8 3  FB7 4  FB4 5  FB5 6  FB6 7 */
            prof_7372.nfb = 8;
            prof_7372.fbbit[0] = 0; prof_7372.fbbit[1] = 1;
            prof_7372.fbbit[2] = 2; prof_7372.fbbit[3] = 5;
            prof_7372.fbbit[4] = 6; prof_7372.fbbit[5] = 7;
            prof_7372.fbbit[6] = 4; prof_7372.fbbit[7] = 3;
            /* The CONFIG bit map was INHERITED from XC7354 and never
             * measured here. It is NOT the row map and XC7354's values are
             * WRONG for FB5 and FB6. Measured, registered-vs-combinatorial at
             * every block, one macrocell each:
             *   FAST  blk2 : FB1 3  FB2 2
             *   HD   blk45 : FB3 2  FB4 5  FB5 6  FB6 7  FB7 4  FB8 3
             * XC7354 has FB5 4 and FB6 3 -- those values belong to FB7 and
             * FB8 here, so an inherited map would have read the WRONG BIT for
             * half the High-Density blocks. */
            prof_7372.cfg_fbbit[0] = -1; prof_7372.cfg_fbbit[1] = -1;
            prof_7372.cfg_fbbit[2] =  2; prof_7372.cfg_fbbit[3] =  5;
            prof_7372.cfg_fbbit[4] =  6; prof_7372.cfg_fbbit[5] =  7;
            prof_7372.cfg_fbbit[6] =  4; prof_7372.cfg_fbbit[7] =  3;
            prof_7372.cfg_fbbit_fast[0] = 3; prof_7372.cfg_fbbit_fast[1] = 2;
            prof_7372.cfg_fbbit_fast[2] = -1; prof_7372.cfg_fbbit_fast[3] = -1;
            prof_7372.cfg_fbbit_fast[4] = -1; prof_7372.cfg_fbbit_fast[5] = -1;
            prof_7372.cfg_fbbit_fast[6] = -1; prof_7372.cfg_fbbit_fast[7] = -1;
            /* TOGGLE on a High-Density block is blk24 here, NOT
             * XC7354's blk21. Measured: the fitter builds `Q := Q.D1 XOR
             * Q.D2` and exactly ONE configuration bit moves, blk24 at the
             * block's own config bit. blk21..24 co-move for POLARITY on both
             * devices, which is why a single inherited value looked safe --
             * the group is NOT monolithic and toggle touches only blk24.
             * SECOND inherited value found wrong, after the config bit map.
             * POWER (blk5 FAST / blk10 HD) and TOGGLE-FAST (blk4) DO match. */
            /* MEASURED as blk24, but its SENSE IS INVERTED relative to
             * XC7354's blk21: a D-type flip-flop reads TOGGLE and a T-type
             * reads D-type. Setting the block alone would ship a CONFIDENT
             * WRONG ANSWER, which ranks worse than none, so the field
             * is DISABLED until the inversion is carried properly. */
            prof_7372.cfg_hd[4] = 24;
            /* `row_g_fast = 1` was TESTED here. It makes K0055's
             * missing export term appear -- FB1_MC1 reads three rows instead
             * of two -- and takes the campaign from 56 PASS to 7. NOT
             * CARRIED. The row base is right; K0055's third exported term
             * comes from somewhere else. */
            /* XC7354's cfg_hd[7] (async RESET) is blk21, the
             * POLARITY block -- three purposes on one number, which
             * says to distrust. Setting the pair to {41,42} here changed
             * NOTHING measurable (MC1 round cycle stayed 8 of 12), so it is
             * NOT carried: an unverified change that buys no evidence is a
             * guess, and rule #1 forbids shipping it. The four failures are
             * REGISTERED MC1 on HIGH-DENSITY blocks and remain undiagnosed. */
            /* The async pair is {41,42} here. XC7354 has {41,21} and
             * blk21 is its POLARITY block -- three purposes on one number,
             * which measurement showed to distrust. Measured at MC1 and MC5 on two
             * blocks, identical encoding to XC7354 otherwise:
             *     none blk41=1 blk42=1   SET 0,1   RESET 1,0
             * Async PRESENCE changes NO config bit -- it is carried by the
             * ROWS alone; only the SENSE lives here. */
            prof_7372.cfg_hd[6] = 41; prof_7372.cfg_hd[7] = 42;

            /* EVERYTHING BELOW IS STILL INHERITED FROM XC7354 AND
             * HAS NOT BEEN MEASURED ON THIS DEVICE -- T6, not fact:
             *     cfg_fast/cfg_hd entries for TOGGLE and POWER
             * (measurement showed OE select blk1/blk18 and FCLK select
             *  blk3/blk46 -- both match XC7354. A selector needs BOTH its
             *  choices live in one design or the fitter collapses them.)
             *     row_g_fast, row_g_hd, split_mc1
             *     cfg_fbinuse, cfg_localfb, cfg_alloc
             *     uim_blk_lo
             * Of the eight fields that HAVE been checked, four matched
             * XC7354 at the same block numbers and the config bit map did
             * not. Do not treat the rest as verified because the profile
             * compiles. And `CLKF = CL` produces a bitmap IDENTICAL to
             * `CLKF = CK` here -- FastCLK1 is UNRESOLVED. */
            prof_7372_init = 1;
        }
        return &prof_7372;
    }
    if (strstr(jf->device, "XC7354")) return &prof_7354;
    if (strstr(jf->device, "XC7318")) {
        if (!prof_7318_init) {
            /* XC7318 fell through to
             * `prof_7336` (FOUR function blocks) since this profile has
             * existed, though every XC7318 `.RES` this project has ever fit
             * reports "Function Blocks 2 2 0" -- `device.c`'s own device
             * table already knows this ({"XC7318", 2, 18,...}, but
             * that table was never wired into `profile_for`'s own DevProfile.
             * The fix (below, in the FUSE-NAMING path only) clamped
             * `nfb_lim` from `device_find()->n_fbs` so a fuse could not be
             * PRINTED as `FB3_...`/`FB4_...` on this device -- a real fix,
             * but of the SYMPTOM (a wrong name), not the CAUSE: `row_fuse`,
             * `fb_of_rowbit` (and therefore `cmd_uim`'s crosspoint scan and
             * `cmd_structuremap`) all still carried nfb=4 and could READ
             * FB3/FB4 address space that does not exist on the real die --
             * an exact instance of this project's own "workaround written
             * against a symptom outlives the fix for its cause" trap.
             * Fixing nfb at
             * the PROFILE makes every reader's own existing `fb > p->nfb`
             * guard (row_fuse's, fb_of_rowbit's loop bound, structuremap's
             * own FB loop) refuse FB3/FB4 uniformly -- the same discipline
             * prof_7236a/prof_73108/prof_7372 already use for their own real
             * geometry, rather than a second, naming-only patch.
             *
             * Same die otherwise: measurement showed 97.0% of every address
             * ANY XC7318 design/pair ever changes is changed by an XC7336
             * design/pair at the IDENTICAL address (the tool's own model was
             * already address-for-address identical for FB1/FB2, since
             * XC7318 had no profile of its own to differ). Clone prof_7336
             * and restrict nfb to the real FB count; FB1/FB2 keep prof_7336's
             * own measured fbbit/cfg_fbbit values (bits 0/2), the pair a
             * real XC7318 design actually places into. The
             * device-table clamp (above, in the naming path) is left in
             * place as harmless, now-redundant defense-in-depth -- it is a
             * no-op once nfb is already 2 at the source. */
            prof_7318 = prof_7336;
            prof_7318.name = "XC7318";
            prof_7318.nfb = 2;
            prof_7318_init = 1;
        }
        return &prof_7318;
    }
    /* EXPERIMENT: XC7372's fuse map has the SAME OFFSET PATTERN as
     * XC7354 (0,8,16,24,32,40,48,56...) and measurement showed 8 of 8 group
     * bases identical. Try the XC7354 profile on it -- method 1b, check
     * whether the structure simply REPEATS before measuring it afresh. */
    return &prof_7336;
}

/* dump every DevProfile field for a device string,
 * VERBATIM from the struct the decoder itself uses (rule 4 -- no
 * transcription of the positional initializers by hand into a second
 * document, which is exactly the kind of copy that drifts). Builds a
 * minimal JedFile carrying only the device string (profile_for/
 * device_row_per_record read nothing else), so no real .jed is needed. */
static int cmd_profiledump(const char *devname) {
    JedFile jf;
    const DevProfile *p;
    int i;
    memset(&jf, 0, sizeof jf);
    snprintf(jf.device, sizeof jf.device, "%s", devname);
    p = profile_for(&jf);
    printf("name %s\n", p->name);
    printf("nfb %d\n", p->nfb);
    printf("fast_upto %d\n", p->fast_upto);
    printf("row_g_fast %d\n", p->row_g_fast);
    printf("row_g_hd %d\n", p->row_g_hd);
    printf("split_mc1 %d\n", p->split_mc1);
    printf("row_per_record %d\n", p->row_per_record);
    printf("uim_blk_lo %d\n", p->uim_blk_lo);
    printf("uim_blk_hi %d\n", p->uim_blk_hi);
    printf("cfg_fbinuse %d\n", p->cfg_fbinuse);
    printf("cfg_localfb %d\n", p->cfg_localfb);
    printf("cfg_alloc %d\n", p->cfg_alloc);
    printf("cfg_polarity %d\n", p->cfg_polarity);
    printf("cfg_raw_lo %d\n", p->cfg_raw_lo);
    printf("cfg_raw_hi %d\n", p->cfg_raw_hi);
    for (i = 0; i < 12; i++) printf("fbbit[%d] %d\n", i + 1, p->fbbit[i]);
    for (i = 0; i < 8; i++) printf("cfg_fast[%d] %d\n", i, p->cfg_fast[i]);
    for (i = 0; i < 8; i++) printf("cfg_hd[%d] %d\n", i, p->cfg_hd[i]);
    for (i = 0; i < 12; i++) printf("cfg_fbbit[%d] %d\n", i + 1, p->cfg_fbbit[i]);
    for (i = 0; i < 12; i++) printf("cfg_fbbit_fast[%d] %d\n", i + 1, p->cfg_fbbit_fast[i]);
    return 0;
}

/* The macrocell group-base table is IDENTICAL on XC7336 and XC7354 --
 * MC1=88 MC2=80 MC3=72 MC4=64 MC5=56 MC6=48 MC7=24 MC8=32 MC9=40, the
 * non-monotonic order, confirmed on a second device (49 of 54
 * macrocells measured directly there). What differs per device is only the
 * FB bit map and the group at which a macrocell's rows begin. */

static const int mc_group[] = { 24, 32, 40, 48, 56, 64, 72, 80 };
static const int mc_of_group[] = {  7,  8,  9,  6,  5,  4,  3,  2 };
static const int fb_of_bit[]   = {  1,  3,  2,  4 };

/* The bitmap is COLUMN-MAJOR per macrocell.
 *
 *     address = block*128 + group_base(macrocell) + fb_bit(function block)
 *
 * This is the ONE place that arithmetic is written down. Every consumer must
 * call these two helpers rather than recompute an address -- re-deriving fuse
 * addresses broke three consecutive sessions on the XC9500 project
 * rule 4).
 *
 * MC1 (group 88) is included here on purpose. What remains unsolved is
 * the MEANING of the block-0 in-use bits at group 88, NOT the addressing:
 * read the FB1_MC1 and FB2_MC1 columns at 88/90 on the 128 stride and
 * they behaved like every other column. Callers that decode the in-use flag
 * must still exclude MC1; callers that only address a column need not. */
static int group_base(int mc) {
    int g;
    if (mc == 1) return 88;                 /* MC1 identifier group */
    for (g = 0; g < (int)(sizeof(mc_group)/sizeof(mc_group[0])); g++)
        if (mc_of_group[g] == mc) return mc_group[g];
    return -1;
}

/* The INVERSE of the profile's row bit map. `fb_of_bit[]` is the
 * XC7336 map written a SECOND time as a bare array, which is the shape that
 * produced -- two copies of one table, free to drift. Anything that
 * turns a bit back into a function block must ask the profile. Returns 0 when
 * the bit indexes no function block on this device. */
static int fb_of_rowbit(const DevProfile *p, int bit);
static int fb_of_rowbit_jed(const JedFile *jf, int bit) {
    return fb_of_rowbit(profile_for(jf), bit);
}

static int fb_of_rowbit(const DevProfile *p, int bit) {
    int fb;
    for (fb = 1; fb <= p->nfb; fb++)
        if (p->fbbit[fb - 1] == bit) return fb;
    return 0;
}

/* this looped `b < 4` over a HARDCODED XC7336 table, so on a
 * six-block device it could only ever return bits 0..3 and never saw FB4 or
 * FB5 at all -- which is why `uim_sources_for_line` found no crosspoints in
 * bank 0 for those blocks and every literal fed through them failed to
 * resolve. The profile has carried the right map */
static int fb_bit_p(const DevProfile *p, int fb) {
    if (fb < 1 || fb > p->nfb) return -1;
    return p->fbbit[fb - 1];
}

static int fb_bit(int fb) {
    int b;
    for (b = 0; b < 4; b++)
        if (fb_of_bit[b] == fb) return b;
    return -1;
}

/* Read a column at a RAW offset within the 128-fuse block. Needed because the
 * MC1 group (88..93) does not follow the group_base+fb_bit layout,
 * so its columns cannot be named through group_base() yet. */
/* A macrocell placed at group g owns FIVE product-term rows, at the
 * five group bases from g upward in ADDRESS order. Each row is a
 * column: 24 slots x 2 polarities, even=complement, odd=true.
 *
 * A row of 48 ones ANDs to constant 0 -- the identity for the OR that follows
 * -- and is reported DISABLED. Read through the OWNING RECORD; the
 * old flat reconstructions could not see these rows at all.
 *
 * The slot index is a per-design signal index and is only stable when the
 * pinlist is FROZEN. Slots are therefore NOT given signal names --
 * naming them needs the UIM source table. An unnamed slot is
 * honest; a wrong
 * signal name in a decompiler is not. */
/* A 40-bit L record starting at group base g holds, in its five
 * 8-fuse groups, the FIVE PRODUCT-TERM ROWS of the macrocell whose group base
 * is g -- one block (one literal-slot position) per record. MC1 is split: an
 * 8-bit record at 88 carries row0, and a 32-bit record at 90 carries rows 1..4.
 *
 * That is why the flat overlay was wrong in BOTH directions:
 * records for adjacent macrocells legitimately describe the same addresses,
 * because the product-term rows are a shared pool the fitter allocates from.
 * Flattening loses which record owns the value. Read the owning record. */
static int mc_group_base(int mc) {
    switch (mc) {
    case 7: return 24; case 8: return 32; case 9: return 40;
    case 6: return 48; case 5: return 56; case 4: return 64;
    case 3: return 72; case 2: return 80; case 1: return 88;
    }
    return -1;
}

/* One fuse of one product-term row. THE single place this is computed. */
static int row_fuse(const JedFile *jf, int fb, int mc, int row, int blk) {
    const DevProfile *p = profile_for(jf);
    int bit, g0;
    if (fb < 1 || fb > p->nfb) return 1;
    bit = p->fbbit[fb - 1];
    if (bit < 0) return 1;
    g0 = (fb <= p->fast_upto) ? p->row_g_fast : p->row_g_hd;
    /* XC7372's MC1 record is FORTY-EIGHT bits -- SIX groups --
     * where every other macrocell's is forty (MEASURED: offset 88 width 48
     * x48, all other offsets width 40; XC7354's offset-88 record is 40).
     * K0055's MC1 export chain is one term short, and its donor reads TWO
     * rows where the fitter exports THREE, so a row IS being missed.
     *
     * TESTED AND NOT CARRIED:  device-wide (56 PASS -> 7)
     * and the same shift applied to MC1 ALONE (56 PASS -> 21). The extra
     * group is real; which row it holds is NOT established, and neither
     * shift is it. */
    if (p->split_mc1 && mc == 1)
        return (row == 0) ? jed_owned(jf, (size_t)blk, 88, 0, bit, 1)
                          : jed_owned(jf, (size_t)blk, 90, row - 1, bit, 1);
    if (p->row_per_record) {
        /* MEASURED. From the macrocell's base (= XC7372's + 4):
         *     base + 0   64-bit  the UIM interleave
         *     base + 4   16-bit  the FIFTH product term -- the ASYNC one
         *     base + 8   64-bit  the FOUR logic rows, 16 bits each
         * Within the 64-bit record, bit = row*16 + fbbit[fb]; blocks 0..47
         * are the AND-array columns. The four/one split is the macrocell
         * architecture, not an encoding quirk -- the datasheet says "four of
         * these are ORed together... the fifth drives the asynchronous Set".
         *
         * read the 16-bit record as an allocation pointer because a
         * single bit MOVED there as terms were added. It moves because it is
         * the async term being re-placed. */
        /* The base is XC7372's EXACTLY -- NO +4 shift. The
         * shift was measured on probes the fitter had COLLAPSED (a six-
         * literal minterm came back as `Q = F`), which land one macrocell
         * off. Verified six-literal probes put FB5_MC5 at offset 56 and
         * FB7_MC3 at 72 -- XC7372's MC5 and MC3 bases unchanged.
         *     base + 0   64-bit  FOUR logic rows, 16 bits each
         *     base + 4   16-bit  the FIFTH / ASYNC product term            */
        int base = mc_group_base(mc);
        int r = g0 + row;
        if (r >= 4)
            return jed_owned(jf, (size_t)blk, base + 4, 0, bit, 1);
        return jed_owned(jf, (size_t)blk, base, 2 * r, bit, 1);
    }
    return jed_owned(jf, (size_t)blk, mc_group_base(mc), g0 + row, bit, 1);
}

/* Fills lits[] and returns: 1 active, 0 all-zero (unused), -1 all-ones
 * (DISABLED -- every line of both polarities on, which ANDs to constant 0,
 * the identity for the OR that follows). */
static int row_decode(const JedFile *jf, int fb, int mc, int row,
                      char *lits, size_t cap) {
    int blk, ones = 0, k;
    size_t used = 0;
    for (blk = 0; blk < 48; blk++) ones += row_fuse(jf, fb, mc, row, blk);
    if (ones == 48) return -1;
    if (ones == 0)  return 0;
    lits[0] = 0;
    for (k = 0; k < 24; k++) {
        int e = row_fuse(jf, fb, mc, row, 2 * k);
        int o = row_fuse(jf, fb, mc, row, 2 * k + 1);
        if (!e && !o) continue;
        used += (size_t)snprintf(lits + used, cap - used, "%s%s%sslot%d",
                 used ? " * " : "", (e && o) ? "BOTH:" : "", e ? "/" : "", k);
        if (used >= cap - 32) break;
    }
    return 1;
}

/* The MACROCELL CONFIGURATION word.
 *
 * It lives in the records at offsets 96 and 104, in the same shape as the
 * product-term rows: GROUP selects the macrocell, BIT selects the function
 * block, BLOCK selects the attribute.
 *
 *     offset  96, groups 0..4 = MC9 MC8 MC7 MC6 MC5
 *     offset 104, groups 0..3 = MC4 MC3 MC2 MC1
 *
 * i.e. macrocell = 9 - index. Note this ordering is NOT the row groups'
 * order (24=MC7, 32=MC8, ...); it is plain descending. Do not reuse one
 * table for the other.
 *
 * Only blocks 11..23 vary. Seven are named below; the rest are measured to
 * move on nothing tried so far and are printed raw, unnamed. */
static int cfg_offset(int mc) { return (mc >= 5) ? 96 : 104; }
static int cfg_group(int mc)  { return (mc >= 5) ? (9 - mc) : (4 - mc); }

static int name_bit(const JedFile *jf, int rec_off, int group, int bit,
                    int blk, char *out, size_t cap) {
    int mc = -1, row = -1, fb = -1, i;
    static const int gb[9] = { 24, 32, 40, 48, 56, 64, 72, 80, 88 };
    static const int gb108[9] = { 28, 36, 44, 52, 60, 68, 76, 84, 92 };
    static const int mcs[9] = {  7,  8,  9,  6,  5,  4,  3,  2,  1 };
    /* XC73108's row bases are XC7372's PLUS FOUR, all nine
     * macrocells, consistent with the extra bits marking
     * the new interconnect paths). `name_bit` used the small-device table
     * unconditionally, so every XC73108 row fuse landed at an offset it did
     * not recognise -- 96 of the 98 unnamed fuses in an isolated polarity
     * pair, at offsets 76 and 84, which are MC3's and MC2's bases here. */
    const int *GB = device_row_per_record(jf) ? gb108 : gb;
    /* on a `row_per_record` device the FUNCTION BLOCK IS NOT IN
     * (group, bit). Measured: the same design moved FB5 -> FB6 changes 51
     * fuses at ONE position, `group 0 bit 2`, for BOTH blocks. If (group,bit)
     * selected the FB, moving it would turn one position off and another on.
     *
     * `fb_of_rowbit_jed` is the 4-FB map and cannot address twelve blocks
     * anyway. Naming the MACROCELL is sound here -- the ladder is
     * measured and `FB5_MC3` was confirmed against `pins` and the decode --
     * but the FB half was inherited from the small-device path and produced
     * `FB1_MC2` in a design that uses FB5 only.
     *
     * So name what is known and say what is not. A wrong name is worse than
     * no name: the `FB3` on a two-block XC7318 inflated coverage by
     * inventing hardware. */
    if (device_row_per_record(jf)) {
        int i2, mc2 = -1;
        if (bit < 0 || bit > 7) return 0;
        /* The UIM LINE-CLAIM family (
         * `x108_claims_line`), inverted. The probe found that the
         * "232-237" activity seen crossing A5->A6 is NOT a new
         * field -- it is THIS mechanism reallocating lines, confirmed by
         * feeding every moved fuse (W2..W8) through the
         * DECODER's OWN forward formula (`x108_line_base`, `x108_line_bank_hi`,
         * `x108_claims_line`'s `bit = 16*slot + cfg_fbbit[dfb-1]`) run in
         * REVERSE: every one of ~20 moved bits resolved to an exact
         * (source FB_MC, consumer FB, line), and the resulting reallocation
         * story (FB10 imports A1/A6/A7 unconditionally via K1/K6/K7, so the
         * DISTINCT signal count -- and hence every line number above it --
         * shifts whenever Z's second literal coincides with one of those)
         * is fully self-consistent across all seven designs. Both `LBASE`
         * and `D` are copies of `x108_line_base`/`x108_line_block`'s own
         * tables (rule 4: one address, one place -- name_bit is defined
         * before those functions, so the table is copied, not the address).
         *
         * This does NOT determine which LINE is a wired-AND vs an ordinary
         * single-source read, and it does not attempt blocks 3/5/7 offset
         * 72 -- confirmed by this same inversion to be OUTSIDE this family
         * (block too low for any `x108_line_base(fb)+d`) and left unnamed
         * rather than guessed (rule: a wrong name is worse than no name). */
        {
            static const int LBASE[12] =
                { 96,104,352,224,112,240,368,376,248,120,232,360 };
            static const int DD[9] = { 4,5,5,0,0,1,1,2,2 };  /* MC1..MC9 */
            static const int BLOW6[6] = { 32,36,40,44,48,52 };
            static const int BOFF6[6] = { 96,100,104,108,112,116 };
            const DevProfile *pp = profile_for(jf);
            int band = -1, bank_hi = -1, f4;
            for (f4 = 0; f4 < 6; f4++) {
                if (BLOW6[f4] == rec_off) { band = f4; bank_hi = 0; break; }
                if (BOFF6[f4] == rec_off) { band = f4; bank_hi = 1; break; }
            }
            if (pp && band >= 0) {
                int idx4 = group * 8 + bit, slot4 = idx4 / 16, fbit4 = idx4 % 16;
                int sfb4;
                for (sfb4 = 1; sfb4 <= 12 && slot4 <= 3; sfb4++) {
                    int d4 = blk - LBASE[sfb4 - 1], smc4 = -1, m4;
                    if (d4 != 0 && d4 != 1 && d4 != 2 && d4 != 4 && d4 != 5)
                        continue;
                    for (m4 = 1; m4 <= 9; m4++)
                        if (DD[m4 - 1] == d4 && ((m4 % 2) != 0) == bank_hi)
                            smc4 = m4;
                    if (smc4 < 0) continue;
                    {   int dfb4;
                        for (dfb4 = 1; dfb4 <= pp->nfb; dfb4++)
                            if (pp->cfg_fbbit[dfb4 - 1] == fbit4) {
                                snprintf(out, cap,
                                    "FB%d_MC%d claims UIM line %d of FB%d",
                                    sfb4, smc4, band * 4 + slot4, dfb4);
                                return 1;
                            }
                    }
                }
            }
        }
        if (blk < 0 || blk > 47) return 0;
        if (group >= 0 && group <= 4) {
            for (i2 = 0; i2 < 9; i2++)
                if (gb108[i2] == rec_off) mc2 = mcs[i2];
            if (mc2 >= 0) {
                snprintf(out, cap, "MC%d row%d %sslot%d  (FB not determined)",
                         mc2, group, (blk % 2) ? "" : "/", blk / 2);
                return 1;
            }
        }
        /*: the CONFIG WORD on a `row_per_record`
         * device was never named. The naming (below) uses
         * `cfg_offset`/`cfg_group` with `idx = group*8 + bit` -- the
         * addressing the DECODER uses for every OTHER device. But
         * `cfg_bit_x` reads a row_per_record config record as
         * FOUR groups of SIXTEEN, `idx = 16*grp + cfg_fbbit(fb)`, because a
         * 64-bit record does not divide into 5 groups of 8 the way a row
         * record does -- and MC5 gets its OWN offset, 100, at group 0. Two
         * different index arithmetics over the SAME linear position; this
         * mirrors `cfg_bit_x` instead of reusing the formula, which
         * is what the row family fix above already had to do once.
         *
         * `blk` is NOT derived here -- it is the caller's attribute block
         * (19, 41, 42, 45, 46, `cfg_polarity`, `cfg_fbinuse`, ...), the same
         * fixed constant every device uses. This only resolves WHICH
         * (fb, mc) owns a given (rec_off, group, bit) inside that block, the
         * same job does for the small-device family. Unverified by
         * a matched pair for THIS naming path specifically -- `cfg_bit_x`
         * itself is of 10) and this reuses its formula
         * exactly, not a new one (rule 4: one address, one place). */
        {
            const DevProfile *pp = profile_for(jf);
            int idx = group * 8 + bit, mc3, f3;
            if (pp) for (mc3 = 1; mc3 <= 9; mc3++) {
                int off3 = (mc3 == 5) ? 100 : cfg_offset(mc3);
                int grp3 = (mc3 == 5) ? 0   : cfg_group(mc3);
                int fbit;
                if (off3 != rec_off) continue;
                if (idx / 16 != grp3) continue;
                fbit = idx % 16;
                /* The comment: "No field is inherited without a
                 * matched pair confirming it" -- for `cfg_fbbit`, not for
                 * `cfg_fbbit_fast`, which prof_73108 never sets and which
                 * therefore stays XC7354's array. A round-trip test (FB5,
                 * MC3, product-term clock) found `cfg_fbbit_fast[1] == 2`
                 * COLLIDING with FB5's real, validated `cfg_fbbit[4] == 2`
                 * and this inversion picked FB2 first -- a wrong name for a
                 * real fuse. FAST FBs (1..fast_upto) are excluded from this
                 * inversion until `cfg_fbbit_fast` is independently
                 * confirmed for this device; the fuse is left unnamed
                 * rather than guessed. */
                for (f3 = pp->fast_upto + 1; f3 <= pp->nfb; f3++) {
                    if (pp->cfg_fbbit[f3 - 1] != fbit) continue;
                    snprintf(out, cap, "FB%d_MC%d config attr blk%d",
                             f3, mc3, blk);
                    return 1;
                }
            }
        }
        return 0;
    }
    /* this was `fbs[4] = {1,3,2,4}` -- XC7336's row-bit map, hard-
     * coded, with a `bit > 3` cutoff. On XC7354 the map is {0,1,2,5,4,3,-1,-1,-1,-1,-1,-1},
     * so `rdiff` NAMED FB2's fuses as FB3 and FB3's as FB2, and could not
     * name FB4 or FB5 at all -- it printed them as "(unnamed region)".
     *
     * `rdiff` is the instrument tells the reader to trust instead
     * of hand-parsing, so a wrong LABEL there is worse than a wrong value
     * elsewhere. Ninth instance of a hardcoded four-block assumption, after
     * eight others. The map now comes from the profile. */
    /* Only the five-row model is named. Records outside it (longer payloads,
     * offsets 96/104/112, blocks past 47) are real data in an UNDECODED
     * region and must NOT be given a row/slot name they have not earned. */
    if (bit < 0 || bit > 7) return 0;
    /* XC7372's MC1 SIXTH product-term row (rec_off
     * 88, group 5) -- already correctly READ and EMITTED by row_fuse()/
     * row_decode() via mc_priv_rows_x()'s own return of 6 for a BURIED,
     * export-donor MC1 (the extensive matched-
     * pair history: 1 term->group0, 2->{2,5}, 3->{2,3,5}, 4->{2,3,4,5}),
     * but never NAMED here -- this function's own group<=4 guard below
     * predates that discovery entirely. Diagnostic-only: decode's own
     * emission is unaffected either way, confirmed by the full
     * regression run before shipping. Checked BEFORE the
     * generic guard, not after, since group==5 would otherwise never
     * reach this function's own row-family lookup at all. */
    if (group == 5 && rec_off == 88 && blk >= 0 && blk <= 47 &&
        strstr(jf->device, "XC7372")) {
        int fb5 = fb_of_rowbit_jed(jf, bit);
        if (fb5) {
            snprintf(out, cap, "FB%d_MC1 row5 (export-donor 6th row) %sslot%d",
                     fb5, (blk % 2) ? "" : "/", blk / 2);
            return 1;
        }
    }
    if (group < 0 || group > 4) return 0;
    /* The CONFIG word was never named. `name_bit` knew only the ROW
     * table (offsets 24..88); offsets 96 and 104 are the configuration word:
     * groups 0..4 = MC9..MC5 at 96, groups 0..3 = MC4..MC1 at 104)
     * and they were 16 of the 80 unnamed fuses, with offset 84 -- MC2's own
     * base on row_per_record devices -- another 47.
     *
     * Nothing new is measured here. `cfg_offset`/`cfg_group` and the profile's
     * FB bit are what the DECODER reads; this inverts them by scan, which is
     * the periodicity argument applied: the word repeats per macrocell and
     * only the owner changes. */
    if (rec_off == 96 || rec_off == 104) {
        const DevProfile *pp = profile_for(jf);
        int f2, m2, nfb_lim;
        if (!pp) return 0;
        /* `96` and `104` are NOT exclusive to the
         * config word -- they are also two of the six UIM CROSSPOINT
         * offsets (`uim_offs[]`: `{32,40,48,96,104,112}`), and
         * this branch used to name ANY record at rec_off 96/104 as a
         * macrocell's "config attr", regardless of which BLOCK it sat
         * in. A dense-design mismatch sweep found this naming a real
         * UIM crosspoint fuse (blocks 80, 81, 90, 96-100, 108, 116,
         * 124, 228, 236 on XC7354's own dense reproducer -- ALL >= its
         * profile's own measured `uim_blk_lo` of 64) as e.g.
         * "FB2_MC7 config attr blk80", when the actual .PLD change
         * landed on a DIFFERENT function block's macrocells entirely
         * (a UIM crosspoint source/destination bit, not a config
         * word at all -- "the SOURCE IS NOT NAMED" own
         * comment, so the CORRECT behaviour is no name, not a
         * plausible-looking wrong one). The true config-word region
         * sits below the crosspoint region on every device measured
         * (max real config block ~46, `uim_blk_lo` >= 64) -- guard on
         * that boundary, already carried by the profile, rather than
         * inventing a new one (rule 4). */
        if (pp->uim_blk_lo > 0 && blk >= pp->uim_blk_lo) return 0;
        nfb_lim = pp->nfb;
        /* `nfb` comes from the PROFILE, and XC7318 shares
         * `prof_7336` -- four blocks against its real two. Naming a fuse
         * `FB3_MC3` on a part whose fitter reports "Function Blocks 2 2 0"
         * and uses only FB1/FB2 when saturated at 18 of 18 macrocells is
         * simply wrong. Bound by the DEVICE TABLE, which is per part. */
        {   char _f[64], _p[32]; const device_t *dv;
            split_part(jf->device, _f, sizeof _f, _p, sizeof _p);
            dv = _f[0] ? device_find(_f) : NULL;
            if (dv && dv->n_fbs > 0 && dv->n_fbs < nfb_lim) nfb_lim = dv->n_fbs;
        }
        /* `FB2_MC5 config attr blk21` was reported for
         * a bit that MOVES FB3_MC1's AND FB3_MC5's polarity (matched pair,
         * XC7354 dense design, address 2818/2819) -- a wrong name, not a
         * missing one (worse than no name at all). Root cause, confirmed by reading `prof_7354`'s own
         * literal arrays: `cfg_fbbit_fast[FB2-1] == 2` (the FAST range's
         * OWN bit assignment) collides with `cfg_fbbit[FB3-1] == 2` (the
         * UNRELATED High-Density range's bit assignment) -- the profile's
         * own comment already documents this exact coincidence ("FB1=3,
         * FB2=2" FAST against "FB3=2" HD) but the loop below used to pick
         * WHICHEVER f2 it reached first (always the FAST one, since it
         * counts up from 1), never checking which RANGE `blk` itself
         * actually belongs to. `blk` is the caller's own attribute-block
         * number (21 here) and the two ranges' block numbers are
         * DISJOINT by construction (`cfg_fast` 0..5, `cfg_hd`/
         * `cfg_polarity`/`cfg_fbinuse`/`cfg_localfb`/`cfg_alloc` in the
         * teens/twenties/forties on every device measured) -- so `blk`'s
         * own range membership disambiguates the collision directly, no
         * new measurement needed, same data the profile already carries
         * (rule 4: one address, one place). Only NARROWS the search when
         * membership is unambiguous (in exactly one range); anything
         * neither/both classifies falls back to the ORIGINAL exhaustive
         * scan, so no case this code used to name changes name. */
        {   int in_fast = 0, in_hd = 0, q2;
            for (q2 = 0; q2 < 8; q2++) {
                if (pp->cfg_fast[q2] == blk) in_fast = 1;
                if (pp->cfg_hd[q2] == blk) in_hd = 1;
            }
            if (blk == pp->cfg_polarity || blk == pp->cfg_fbinuse ||
                blk == pp->cfg_localfb || blk == pp->cfg_alloc)
                in_hd = 1;
            for (f2 = 1; f2 <= nfb_lim; f2++) {
                int is_fast_f2 = (f2 <= pp->fast_upto);
                if (in_fast != in_hd) {   /* unambiguous -- narrow */
                    if (in_fast && !is_fast_f2) continue;
                    if (in_hd && is_fast_f2) continue;
                }
                {   int fbit = is_fast_f2 ? pp->cfg_fbbit_fast[f2 - 1]
                                          : pp->cfg_fbbit[f2 - 1];
                    if (fbit != bit) continue;
                }
                for (m2 = 1; m2 <= 9; m2++)
                    if (cfg_offset(m2) == rec_off && cfg_group(m2) == group) {
                        snprintf(out, cap, "FB%d_MC%d config attr blk%d", f2, m2, blk);
                        return 1;
                    }
            }
        }
        return 0;
    }
    if (blk < 0 || blk > 47) return 0;
    fb = fb_of_rowbit_jed(jf, bit);
    if (!fb) return 0;            /* no function block at this bit */
    if (rec_off == 90) { mc = 1; row = group + 1; }
    else if (rec_off == 88) { mc = 1; row = 0; }
    else {
        for (i = 0; i < 9; i++) if (GB[i] == rec_off) { mc = mcs[i]; row = group; }
    }
    if (mc < 0) {
        /* The SHARED PRODUCT-TERM POOL's own
         * CONTENT bits (`shared_pt[]`, read via shared_pt_fuse()/
         * shared_pt_state()/shared_pt_slot_used(), use
         * rec_off/group pairs OUTSIDE the per-macrocell row-base table
         * above (off 0/8/16, none of GB[]/88/90) -- genuinely read and
         * consumed by decode for any HD macrocell needing more than 5
         * private product terms, on every HD-capable device, but never
         * named here because this table predates the shared-pt
         * mechanism entirely and `cmd_structuremap`'s own forward model
         * never enumerated it either -- confirmed the sole
         * source of 942 of the 2,926 read-but-unnamed addresses named
         * (XC7354 470, XC7372 389, plus XC73108's own share).
         * Copied locally, not re-derived (rule 4) -- the SAME 12-entry
         * table `shared_pt_claimed()`/`shared_pt_fuse()` already use,
         * a few hundred lines below where this function is defined. */
        static const struct { int off, grp; } spt[12] = {
            {0,0},{0,1},{0,2},{0,3},{0,4},{8,0},{8,1},{8,2},
            {16,0},{16,1},{8,3},{8,4}
        };
        int si;
        for (si = 0; si < 12; si++)
            if (spt[si].off == rec_off && spt[si].grp == group) {
                snprintf(out, cap, "FB%d shared-pool row%d %sslot%d", fb, si,
                         (blk % 2) ? "" : "/", blk / 2);
                return 1;
            }
        return 0;
    }
    snprintf(out, cap, "FB%d_MC%d row%d %sslot%d", fb, mc, row,
             (blk % 2) ? "" : "/", blk / 2);
    return 1;
}

/* THE CONFIG FB BIT COMES FROM THE PROFILE, not from fb_bit().
 *
 * fb_bit() is the ROW / UIM-destination map. On XC7336 the configuration word
 * happens to use the same one, which is why calling it here was invisible for
 * forty sections. On XC7354 it is NOT the same -- measurement showed FB1 -> 3
 * and FB2 -> 2 in the FAST config range against 0 and 1 for the rows -- and
 * because nothing in the C ever read `cfg_fbbit`, the wrong map sat in the
 * profile with no code path able to expose it.
 *
 * Returns -1 when the function block's config bit is NOT MEASURED on this
 * device. A caller must print that as unknown, never as a 0. */
static int cfg_bit_x(const JedFile *jf, int fb, int mc, int blk, int *status) {
    const DevProfile *p = profile_for(jf);
    int bit;
    if (blk < 0) return -1;                       /* attribute not located   */
    if (fb < 1 || fb > p->nfb) return -1;
    bit = (fb <= p->fast_upto) ? p->cfg_fbbit_fast[fb - 1]
                               : p->cfg_fbbit[fb - 1];
    if (bit < 0) return -1;                       /* FB never measured       */
    /* on XC73108 a config record is 64 bits = FOUR groups of
     * sixteen, so `cfg_group(5) = 9-5 = 4` addresses a group that does not
     * exist and the read landed on a neighbouring block's bit -- `config`
     * reported REGISTERED for a combinatorial macrocell and the emitter
     * wrote a spurious clock, failing the round cycle on PIN SETS DIFFER.
     * MC5 has its OWN offset, 100, at group 0. Measured 10 of 10: FBn_MC5
     * combinatorial vs registered moves exactly `(blk45, 100, cfg_bit(fb))`
     * for every one of ten function blocks.
     * The same anomaly, and the same remedy, as the UIM crosspoint.
     * */
    if (profile_for(jf)->row_per_record) {
        /* A config record is 64 bits = FOUR groups of SIXTEEN, so
         * the group multiplier is 16, not 8. `jed_owned_ex` computes
         * `group*8 + bit`, which is right for an 8-group 64-bit record and
         * wrong here -- it read a neighbouring block's bit for every
         * macrocell whose group is not 0. Measured 9 of 9 on FB4:
         *
         *   MC1 (104,52) MC2 (104,36) MC3 (104,20) MC4 (104,4)
         *   MC5 (100, 4)
         *   MC6 ( 96,52) MC7 ( 96,36) MC8 ( 96,20) MC9 ( 96,4)
         *
         * i.e. `bit = 16*cfg_group(mc) + cfg_bit(fb)`, with MC5 at its own
         * offset 100 group 0 -- `cfg_group(5) = 4` has no fifth group.
         * Passed as (group, bit) = (idx/8, idx%8) so the 8x accessor lands
         * on the right index. */
        int off = (mc == 5) ? 100 : cfg_offset(mc);
        int grp = (mc == 5) ? 0    : cfg_group(mc);
        int idx = 16 * grp + bit;
        if (idx > 63) return -1;
        return jed_owned_ex(jf, (size_t)blk, off, idx / 8, idx % 8, 1, status);
    }
    return jed_owned_ex(jf, (size_t)blk, cfg_offset(mc), cfg_group(mc),
                        bit, 1, status);
}




/* SEMANTIC ATTRIBUTE LOOKUP for the emitter. `decode.c` hardcoded
 * XC7336's attribute block numbers (12..21). On XC7354 they differ, and differ
 * AGAIN between FAST and High-Density blocks, so those literals were a device
 * assumption baked into eight lines. Index order matches
 * DevProfile.cfg_fast / cfg_hd:
 *   0 OE used  1 OE select  2 registered  3 FastCLK  4 toggle  5 power
 *   6 async set  7 async reset                                          */

/* polarity is blk19 on XC7336 and the blk21-24 FIELD on XC7354.
 * The emitter read blk19 for every device, so every registered
 * XC7354 design rebuilt INVERTED. */
/* THE PIN/UIM SPLIT.
 *
 * When a UIM wired-AND node takes a BONDED macrocell NEGATED, the fitter
 * stores the complement in the macrocell -- blk21..24 = 1111, which
 * `cfg_polarity_bit` already reports -- and the UIM reads that stored value.
 * But the PIN must still drive the TRUE equation, and blk19 is what says so.
 *
 * MEASURED, six designs, three shapes:
 *
 *     design  blk19  blk21..24   what the pin drives
 *     HAP/HBP   1      0000      the equation          (positive UIM node)
 *     HAN/HBN   **0**  1111      the equation          <- THE SPLIT
 *     JP        1      0000      the equation          (no UIM node)
 *     JN        1      **1111**  the COMPLEMENT        (ordinary inversion)
 *
 * `JN` is the discriminator: polarity inverted with blk19 = 1 is an ORDINARY
 * inversion, which the decoder already handles and which round-cycles 2 of 2.
 * Only blk19 = 0 alongside an inverted polarity is the split.
 *
 * Returns 1 when this macrocell stores the complement while its pin drives
 * the true equation. */
int cfg_pin_uim_split(const JedFile *jf, int fb, int mc) {
    const DevProfile *p = profile_for(jf);
    if (!p || p->nfb != 12) return 0;          /* measured on XC73108 only */
    if (!cfg_polarity_bit(jf, fb, mc)) return 0;
    return cfg_bit(jf, fb, mc, 19) ? 0 : 1;
}

int cfg_polarity_bit(const JedFile *jf, int fb, int mc) {
    const DevProfile *p = profile_for(jf);
    int b = (p->cfg_polarity >= 0) ? p->cfg_polarity : 19;
    /* `cfg_polarity` is blk21 on XC7354, and measurement showed that
     * field on HIGH-DENSITY macrocells ONLY -- it says so, and it says of the
     * FAST blocks: "inversion NEVER VARIED -- 60 designs, all inverted -- so
     * no FAST polarity bit is claimed". Reading blk21 there anyway is reading
     * the HD field at a FAST address, and it returns 0 often enough to emit a
     * NOT-INVERTED equation for a macrocell the fitter always inverts.
     *
     * 44 generated probes on FB1 and FB2, MC1 and MC5, 1-3 terms, registered
     * and combinatorial, asking for BOTH polarities: the fitter produced
     * `/Q` in ALL FORTY-FOUR. With the 60 that is 104 designs and no
     * variation, and unlike the sample this one ASKED FOR THE OTHER
     * VALUE and was refused -- an absence that was actively falsifiable and
     * survived (the rule, applied the right way round).
     *
     * On an XC7354 FAST block the output is INVERTED, full stop. XC7336 is
     * untouched: its FFBs read blk19, which measurement showed on that part. */
    /* FIFTEENTH equality guard. Measurement showed that a FAST
     * macrocell's output is ALWAYS INVERTED -- 48 probes, both polarities
     * requested, all 48 returned `/Q`. XC7372's FB1/FB2 are FAST too, and
     * with `nfb == 6` they fell through to the High-Density field read at a
     * FAST address, so every FAST macrocell decoded with the WRONG POLARITY. */
    if (p->nfb >= 6 && fb <= p->fast_upto) return 1;   /* 1 = INVERTED */
    return cfg_bit(jf, fb, mc, b);
}

int cfg_attr(const JedFile *jf, int fb, int mc, int which) {
    const DevProfile *p = profile_for(jf);
    const int *t = (fb <= p->fast_upto) ? p->cfg_fast : p->cfg_hd;
    if (which < 0 || which > 7 || t[which] < 0) return -1;
    return cfg_bit(jf, fb, mc, t[which]);
}

int cfg_bit(const JedFile *jf, int fb, int mc, int blk) {
    return cfg_bit_x(jf, fb, mc, blk, NULL);
}

/* The UIM crosspoint region, blocks 48..125.
 *
 *   (block, offset)  = the SOURCE feeding the UIM
 *   group            = which UIM OUTPUT LINE into that function block
 *   bit              = the DESTINATION function block, +0=FB1 +1=FB3 +2=FB2
 *                      +3=FB4 -- the SAME map as the rows and the
 *                      configuration word
 *
 * A set bit is a connection. The datasheet's SMARTswitch means several
 * sources may legitimately drive one output line (wired-AND), so multiple
 * set bits in one (group,bit) are LEGAL, not an error.
 *
 * The SOURCE IS NOT NAMED. Mapping (block,offset) to a pin or macrocell is
 * measured for 13 sources only and the index rule is not established.
 * Printing a guess here would be the silent lie this project
 * forbids. */
static const int uim_offs[6] = { 32, 40, 48, 96, 104, 112 };
/* XC73108's SIXTH reader of the UIM bank layout. The scan was
 * converted; this PRINT path carries its own copy with
 * `bank_of[6]` and `half_of[6]` and was still on three banks. Twelve offsets
 * at stride 4, four lines per bank. */
static const int uim_offs12[12] = { 32, 36, 40,  44,  48,  52,
                                    96, 100, 104, 108, 112, 116 };

/* DEVICE PROFILES. The row geometry (group bases on the 8-stride,
 * 24 slots x 2 polarities over 48 blocks) is IDENTICAL across devices; what
 * changes is the FUNCTION-BLOCK BIT MAP and, on High-Density blocks, the
 * group at which a macrocell's rows begin.
 *
 *   XC7318/XC7336   4 FFBs      +0 FB1  +1 FB3  +2 FB2  +3 FB4
 *   XC7354          FB1,FB2 FAST; FB3..FB6 HIGH-DENSITY
 *                   FB1=0 FB2=1 FB3=2 FB6=3 FB5=4 FB4=5
 *                   FAST rows start at group 1, HIGH-DENSITY at group 0
 *
 * Measured on XC7354 at MC1, MC5 and MC9 of all six blocks.
 * MC2,3,4,6,7,8 are NOT yet fitted there -- the warning about recording
 * a rule with gaps in the sample applies, so XC7354 stays behind the guard. */

/* there are TWO source bands.
 *   slots  0..56   an I/O PIN used as an input. Rule below, verified.
 *   slots 64+      a MACROCELL OUTPUT used as an internal source. NOT mapped;
 *                  four points measured (FB1_MC1->73, FB2_MC3->107,
 *                  FB3_MC9->85, FB4_MC4->112) and a linear fit to them
 *                  CONTRADICTS the pairings implied by a chained design.
 *                  Left unnamed on purpose.
 *
 * source slot index i = 2*(block-64) + (offset==32 ? 0: 1).
 * For MACROCELL FEEDBACK sources:  i = 16*fb_index + (mc-1)
 * with fb_index the SAME FB order as everything else: FB1=0 FB3=1 FB2=2 FB4=3.
 * Verified on 17 macrocells with three advance predictions that held.
 *
 * AMBIGUITY, and it is why the name is qualified: a dedicated input pin
 * (PC44 pin 28) measures to slot 32, which the rule also assigns to FB2_MC1.
 * The bitmap cannot distinguish them, so the name is printed as "by rule"
 * and the caveat is printed once. */

/* THE XC7354 UIM PIN-SOURCE TABLE, measured by a 124-design campaign
 * in which EVERY design forces its single input onto ONE package pin with a.
 * VMF -- sweeping PINS, not names. 49 pins each produced EXACTLY
 * ONE crosspoint, 49 distinct slots, no collision, identical at one and two
 * product terms.
 *
 *   slot = BASE(fb) + (mc-1)   for a macrocell PAD of FB1..FB5
 *   BASE = { FB1 0, FB2 16, FB3 32, FB4 272, FB5 256 }
 *   holds on all 35 measured macrocell pads, 0 exceptions.
 *
 * FB6 does NOT follow it and is a measured table. Its band starts at 48, but
 * slots 48,49,50 are taken by the DEDICATED INPUT pins 3, 5 and 11, and FB6's
 * pads sit above them. Whether that displacement is the cause is NOT
 * established -- the six entries are recorded as measured.
 *
 * MC3, MC4 and MC5 of FB6 are pins 8, 9 and 10, which are FastClock pins:
 * PLUSASM refuses `Signal assigned to FastClock pin ... not used`, so their
 * pin-band slots CANNOT be measured this way and are absent, not zero. */
/* ONE BAND INDEX EXPLAINS BOTH SOURCE BANDS, and the formulae are
 * The XC7336 rules -- with a per-device B:
 *
 *     B = { FB1 0, FB2 1, FB3 2, FB6 3, FB5 16, FB4 17 }
 *     pin band              slot = 16*B + (mc-1)
 *     macrocell-output band slot = 64 + 16*B + ((mc+8) mod 12)
 *
 * The second was measured on 54 of 54 macrocells with 0 misses, every
 * placement confirmed in the FITTER'S OWN .MAP before the bitmap was read.
 * The first holds on all 39 measured pads of FB1..FB5; FB6's pads do not
 * follow it and stay a table. */
static int xc7354_band(int fb) {
    switch (fb) {
    case 1: return 0;  case 2: return 1;  case 3: return 2;
    case 6: return 3;  case 5: return 16; case 4: return 17;
    default: return -1;
    }
}

static int xc7354_pin_slot_base(int fb) {
    int b = xc7354_band(fb);
    if (fb == 6 || b < 0) return -1;    /* FB6 is a table, not a rule */
    return 16 * b;
}

static const struct { int slot, fb, mc; } xc7354_fb6[] = {
    { 51, 6, 1 }, { 52, 6, 2 }, { 53, 6, 6 },
    { 54, 6, 7 }, { 55, 6, 8 }, { 56, 6, 9 },
};

/* Dedicated INPUT pins, measured in the same campaign. These are package pins
 * with no macrocell behind them, so they name a PIN and not a macrocell.
 * PC68 ONLY -- */
static const struct { int slot, pin; } xc7354_inpin[] = {
    { 32, 43 }, { 33, 65 }, { 34, 67 }, { 35, 68 },
    { 37,  2 }, { 48,  3 }, { 49,  5 }, { 50, 11 },
};

/* XC7354-PC44/PC84 dedicated inputs, MEASURED -- the
 * consumer-side crosspoint address for each pad (block/offset, the
 * own method for XC73108) resolved directly to the SAME device-wide SLOT
 * NUMBER space `xc7354_inpin` above already uses (`cand = 2*(blk-64)+half`,
 * this file's own established formula), confirmed by direct computation, not
 * assumed: PC44's pin43->slot34 EQUALS PC68's own slot34->pin67 mapping,
 * same slot, different package pin -- the SAME 8-slot device-wide space
 * (32,33,34,35,37,48,49,50), with PC44/PC84's smaller packages bonding only
 * 6 of those 8 slots to physical pins (34,35,37,48,49,50 -- slots 32/33 are
 * simply not brought out on either package). Six of six pads, confirmed
 * pad-intrinsic by FOUR independent consumers on the representative pad (no
 * block/offset movement across FB1/FB2/FB4/FB6 consumers, only the
 * consumer-dependent INDEX moved, exactly PQ160's own shape).
 */
/* slots 32/33 ADDED (pins 28/42) -- a later change proved
 * these are real, physically bonded, UIM-routable dedicated-input pads
 * (P44X28/P44X42 fit cleanly on the real fitter), excluded from
 * The original 6-of-8 discovery only because that discovery
 * probe's own auto-router claimed pins 28/42 for its own I1/I2. PC44
 * now bonds all 8 of the device-wide 8-slot family (the
 * corrected count). Directly resolves 20 of the 200-design blind-spot
 * corpus's 39 "OTHER" refusals (all 20 XC7354-PC44 members, traced:
 * 14 hit slot32, 6 hit slot33 -- zero hit anything else). */
static const struct { int slot, pin; } xc7354_inpin_pc44[] = {
    { 32, 28 }, { 33, 42 },
    { 34, 43 }, { 35, 44 }, { 37,  2 }, { 48,  3 }, { 49,  4 }, { 50,  7 },
};
/* slots 32/33 ADDED (pins 53/81) -- same rationale
 * as PC44 above: P84X53/P84X81 fit cleanly), same excluding
 * artifact (the discovery probe's own I1/I2). PC84 now bonds all 8 of
 * 8 too. */
static const struct { int slot, pin; } xc7354_inpin_pc84[] = {
    { 32, 53 }, { 33, 81 },
    { 34, 83 }, { 35, 84 }, { 37,  2 }, { 48,  3 }, { 49,  5 }, { 50, 13 },
};

/* The FULL device-wide dedicated-input slot space --
 * from the one package (PC68) that bonds all 8 of them. A later change proved
 * directly (P44X28/P44X42/P84X53/P84X81/P68X64/P68X65: all real designs,
 * all fit cleanly on the real fitter) that a slot in this set is
 * STRUCTURALLY a dedicated-input-pad address on EVERY package of this
 * device, never a legitimate "read this macrocell's own pin" reference
 * -- regardless of whether the CURRENT package's own measured table
 * (xc7354_inpin_pc44/pc84 above) happens to have an entry for it yet.
 * Used to REFUSE rather than silently fall through to the generic
 * pin-band interpretation, which was proven wrong: right by accident on
 * PC44's slot 32/33 ("no package pin"), silently WRONG on PC84's slot
 * 32/33, giving pins 77/79 instead of the design's real 53/81. */
static const int xc7354_dedin_slots[] = { 32, 33, 34, 35, 37, 48, 49, 50 };

/* XC7372 / PC84 dedicated inputs, MEASURED -- 12 probes, one per
 * input pad, frozen by `.VMF`, slot read from `uim`. TWELVE dedicated inputs
 * against XC7354's eight, and they land in THE SAME TWO SLOT BANDS:
 *
 *     pins 79..84 -> slots 32..37        pins 2..7 -> slots 48..53
 *
 * XC7354 uses 32..35 and 37 of the first band and 48..50 of the second, so
 * XC7372 is that structure with the bands FILLED OUT, not a new layout. This
 * is method 1b holding at the slot level as well as the fuse-map level. */
/* XC7372 macrocell-output slots, MEASURED -- 59 of 59 probes, a
 * REGISTERED buried source per macrocell (a combinatorial one is INLINED by
 * The fitter and the probe measures nothing, the trap) read by a
 * consumer in FB1, slot taken from `uim`.
 *
 *   FB1 64   FB2 80   FB3 96   FB8 112      <- stride 16
 *   FB7 320  FB4 336  FB5 352  FB6 368      <- stride 16, second region
 *
 *   MC4..MC9 -> base + 0..5        MC1..MC3 -> base + 9..11
 *
 * Perfectly regular, and the same shape XC7354 uses -- method 1b holding at
 * the slot level once the UIM scan reached block 253. */
static const int xc7372_fb_base[9] = { 0, 64, 80, 96, 336, 352, 368, 320, 112 };

/* XC7372's I/O PAD input band -- a macrocell's PIN used as an INPUT,
 * which is a different band from its OUTPUT. 37 probes, one per pad, the pad
 * frozen as an `INPUTPIN`:
 *
 *   FB1 0    FB2 16   FB3 32   FB8 48       slot = base + (mc - 1)
 *   FB7 256  FB4 272  FB5 288  FB6 304
 *
 * Stride 16 throughout, and the bases are XC7354's `16 * band` values with
 * two more blocks appended. The dedicated-input slots 32..37 and 48..53
 * OVERLAP FB3's and FB8's bands; the dedicated-input table wins, exactly as
 * it does on XC7354. */
static const int xc7372_pad_base[9] = { 0, 0, 16, 32, 272, 288, 304, 256, 48 };

static int xc7372_pad_cell(int slot, int *fb, int *mc) {
    int f;
    for (f = 1; f <= 8; f++) {
        int d = slot - xc7372_pad_base[f];
        if (d >= 0 && d <= 8) { *fb = f; *mc = d + 1; return 1; }
    }
    return 0;
}

static int xc7372_slot_cell(int slot, int *fb, int *mc) {
    int f;
    for (f = 1; f <= 8; f++) {
        int b = xc7372_fb_base[f], d = slot - b;
        if (d >= 0 && d <= 5)  { *fb = f; *mc = d + 4;  return 1; }
        if (d >= 9 && d <= 11) { *fb = f; *mc = d - 8;  return 1; }
    }
    return 0;
}

/* XC7372's registered-input-pad config addresses, MEASURED -- 24
 * probes, each pad plain against `RCLK = RC`, everything else frozen. Blocks
 * 13 and 14 exactly as XC7354: 14 is the REGISTER, 13 the CLOCK
 * ENABLE), and the address walks the macrocell rule:
 *     pins  2..7  -> FB1, mc 1..6      bit 3 (FB1's FAST config bit)
 *     pins 79..84 -> FB2, mc 1..6      bit 2 (FB2's)
 * `input_pad_registered` returned 0 for `nfb != 6`, so XC7372 never detected
 * one and every registered-input design rebuilt without its RCLK. */
static const struct { int pin, fb, mc; } xc7372_inpad_cfg[] = {
    {  2, 1, 1 }, {  3, 1, 2 }, {  4, 1, 3 },
    {  5, 1, 4 }, {  6, 1, 5 }, {  7, 1, 6 },
    { 79, 2, 1 }, { 80, 2, 2 }, { 81, 2, 3 },
    { 82, 2, 4 }, { 83, 2, 5 }, { 84, 2, 6 },
};

/* PC84 ONLY -- */
static const struct { int slot, pin; } xc7372_inpin[] = {
    { 32, 79 }, { 33, 80 }, { 34, 81 }, { 35, 82 }, { 36, 83 }, { 37, 84 },
    { 48,  2 }, { 49,  3 }, { 50,  4 }, { 51,  5 }, { 52,  6 }, { 53,  7 },
};

/* XC7372-PC68 dedicated inputs, MEASURED -- SAME device-
 * wide slot space as PC84's table above (`cand = 2*(blk-64)+half`, computed
 * directly from measured (block,offset) pairs, not assumed), a different
 * 8-of-12-slot subset brought out to different physical pins on this
 * smaller package. Eight of eight pads, confirmed pad-intrinsic by four
 * independent consumers (FB1/FB3/FB5/FB6/FB7). */
/* slots 33/34 ADDED (pins 64/65) -- the
 * P68X64/P68X65 both fit cleanly on the real fitter, real bonded pads
 * excluded from the original discovery for the same auto-router-
 * grabbed-them-for-I1/I2 artifact as PC44/PC84 above. PC68 now bonds
 * 10 of its own 12-slot family (slots 32 and 53 remain genuinely
 * absent -- the directly-measured fixed VCC/VSS pins on this
 * specific package, not a decode gap). Directly resolves 19 of the
 * 200-design blind-spot corpus's 39 "OTHER" refusals (all 19 XC7372-
 * PC68 members, traced: every one hits slot33; zero hit slot32/34/53
 * -- slot34's own real pad, though now wired, happens to be unused by
 * any of this generator's own 200 designs). */
static const struct { int slot, pin; } xc7372_inpin_pc68[] = {
    { 33, 64 }, { 34, 65 },
    { 35, 66 }, { 36, 67 }, { 37, 68 },
    { 48,  2 }, { 49,  3 }, { 50,  4 }, { 51,  5 }, { 52,  6 },
};

/* XC7372's full device-wide dedicated-input slot
 * space, from PC84 (bonds all 12). Same rationale as
 * `xc7354_dedin_slots` above -- PC68 covers only 8 of these 12
 * (missing 32, 33, 34, 53); the P68X64/P68X65 (slots 33/34)
 * measured this fallback directly (33 "no package pin" by accident,
 * 34 silently substitutes pin 60 instead of the design's real pin 65).
 * The remaining two, 32 and 53, are a DIFFERENT case item
 * 1): their own address formula computes to pins 63 and 7, and this
 * package's own real `.PIN` report shows BOTH are fixed VCC/VSS,
 * confirmed by direct probe -- a genuine package-bonding shortfall
 * (PC68 has only 10 physical "I"-type pins for 12 dedicated-input
 * slots), not an unmeasured real pin like PC44/PC84's case. Refusing
 * them is correct for the right reason on this package specifically. */
static const int xc7372_dedin_slots[] = {
    32, 33, 34, 35, 36, 37, 48, 49, 50, 51, 52, 53
};

static int slot_in_set(int slot, const int *set, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) if (set[i] == slot) return 1;
    return 0;
}

/* Name an XC7354 UIM SOURCE slot. Returns 0 when the slot is not measured --
 * the MACROCELL-OUTPUT band is entirely unmeasured on this device, so a
 * decode is still impossible and `decode` still refuses.
 *
 * takes the DEVICE STRING now, not just an `is7372`
 * flag, so it can pick the right PACKAGE's dedicated-input table --
 * `xc7354_inpin`/`xc7372_inpin` are PC68/PC84-only; a
 * PC44 or PC84(XC7354)/PC68(XC7372) slot must use its OWN table
 * (`xc7354_inpin_pc44`, `xc7354_inpin_pc84`, `xc7372_inpin_pc68`, all
 *-item-3) or it silently prints the WRONG package's pin
 * number for a slot the two packages both use for different physical pads. */
static int slot_name_7354(int i, char *out, size_t cap, const char *device) {
    size_t k;
    int fb, base;
    int is7372 = strstr(device, "XC7372") != NULL;
    char fam[16], pkg[16];
    split_part(device, fam, sizeof fam, pkg, sizeof pkg);
    if (i < 0) return 0;
    for (k = 0; k < sizeof xc7354_fb6 / sizeof xc7354_fb6[0]; k++)
        if (xc7354_fb6[k].slot == i) {
            snprintf(out, cap, "FB%d_MC%d", xc7354_fb6[k].fb, xc7354_fb6[k].mc);
            return 1;
        }
    if (is7372) {
        int f2, m2;
        if (xc7372_slot_cell(i, &f2, &m2)) {
            snprintf(out, cap, "FB%d_MC%d", f2, m2);
            return 1;
        }
        if (strcasecmp(pkg, "PC68") == 0) {
            for (k = 0; k < sizeof xc7372_inpin_pc68 / sizeof xc7372_inpin_pc68[0]; k++)
                if (xc7372_inpin_pc68[k].slot == i) {
                    snprintf(out, cap, "PIN%02d (dedicated input)",
                             xc7372_inpin_pc68[k].pin);
                    return 1;
                }
        } else
        for (k = 0; k < sizeof xc7372_inpin / sizeof xc7372_inpin[0]; k++)
            if (xc7372_inpin[k].slot == i) {
                snprintf(out, cap, "PIN%02d (dedicated input)",
                         xc7372_inpin[k].pin);
                return 1;
            }
        if (xc7372_pad_cell(i, &f2, &m2)) {
            snprintf(out, cap, "FB%d_MC%d", f2, m2);
            return 1;
        }
    } else if (strcasecmp(pkg, "PC44") == 0) {
        for (k = 0; k < sizeof xc7354_inpin_pc44 / sizeof xc7354_inpin_pc44[0]; k++)
            if (xc7354_inpin_pc44[k].slot == i) {
                snprintf(out, cap, "PIN%02d (dedicated input)", xc7354_inpin_pc44[k].pin);
                return 1;
            }
    } else if (strcasecmp(pkg, "PC84") == 0) {
        for (k = 0; k < sizeof xc7354_inpin_pc84 / sizeof xc7354_inpin_pc84[0]; k++)
            if (xc7354_inpin_pc84[k].slot == i) {
                snprintf(out, cap, "PIN%02d (dedicated input)", xc7354_inpin_pc84[k].pin);
                return 1;
            }
    } else
    for (k = 0; k < sizeof xc7354_inpin / sizeof xc7354_inpin[0]; k++)
        if (xc7354_inpin[k].slot == i) {
            snprintf(out, cap, "PIN%02d (dedicated input)", xc7354_inpin[k].pin);
            return 1;
        }
    for (fb = 1; fb <= 5; fb++) {
        base = xc7354_pin_slot_base(fb);
        if (base < 0) continue;
        if (i >= base && i <= base + 8) {
            snprintf(out, cap, "FB%d_MC%d", fb, i - base + 1);
            return 1;
        }
    }
    /* The MACROCELL-OUTPUT band, 54 of 54 measured, 0 misses. */
    for (fb = 1; fb <= 6; fb++) {
        int b = xc7354_band(fb), mc;
        if (b < 0) continue;
        base = 64 + 16 * b;
        for (mc = 1; mc <= 9; mc++)
            if (base + ((mc + 8) % 12) == i) {
                snprintf(out, cap, "FB%d_MC%d out", fb, mc);
                return 1;
            }
    }
    return 0;
}

static int slot_name(int i, char *out, size_t cap) {
    static const int fb_of_idx[4] = { 1, 3, 2, 4 };
    int band, mc;

    /* REJECT A NEGATIVE SLOT. The slot rule is
     * i = 2*(block-64) + ..., so any block below 64 gives i < 0. Band 1 then
     * computed band = i/16 = -2 and indexed fb_of_idx[-2] -- an
     * OUT-OF-BOUNDS READ, undefined behaviour, which printed the impossible
     * name "FB0_MC1" for a device that has no function block 0. Found by the
     * first REAL DEVICE READBACK, because a readback has data at offsets the
     * fitter never writes and so reached blocks the fitter's own bitmaps
     * never populated. A fabricated name is the silent lie this project
     * forbids, and this one was fabricated by reading off the front of an
     * array. */
    if (i < 0) return 0;

    /* BAND 2 -- a MACROCELL OUTPUT used as an internal source:
     *     slot = 64 + 16*fb_index + ((mc + 8) mod 12)
     * Measured on 35 of 36 macrocells by a buried-source sweep, one design
     * each. FB3_MC6 cannot drive an output on PC44 (its pin is MR,
     * and is INTERPOLATED between its measured neighbours MC5=81 and MC7=83. */
    if (i >= 64) {
        int k = i - 64;
        band = k / 16;
        if (band < 4) {
            int off = k % 16;
            /* invert ((mc + 8) mod 12) over mc = 1..9 */
            mc = (off >= 9) ? (off - 8) : (off + 4);
            if (mc >= 1 && mc <= 9 && off < 12) {
                snprintf(out, cap, "FB%d_MC%d out", fb_of_idx[band], mc);
                return 1;
            }
        }
        return 0;
    }

    /* BAND 1 -- an I/O PIN used as an input. */
    band = i / 16; mc = (i % 16) + 1;
    if (band < 4 && mc <= 9) {
        snprintf(out, cap, "FB%d_MC%d", fb_of_idx[band], mc);
        return 1;
    }
    return 0;
}

/* The MAKEJED -s signature, stored as plain ASCII.
 *     char c (0..3) = GROUP,  bit b (0..7) = BLOCK
 *     address = b*128 + 104 + c*8 + 1
 * Verified by reading back four bitmaps written with known signatures
 * (TEST, ZZZZ, TESU, AAAA), all four exact. and NOT
 * implemented until now -- the fuses varied and the tool ignored them. */
static int cmd_signature(const char *path) {
    JedFile jf;
    int c, b;
    char sig[8];
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    if (!device_supported(&jf)) { jed_free(&jf); return 2; }
    for (c = 0; c < 4; c++) {
        int v = 0;
        for (b = 0; b < 8; b++)
            v |= jed_owned(&jf, (size_t)b, 104, c, 1, 1) << b;
        sig[c] = (v >= 32 && v < 127) ? (char)v : '?';
    }
    sig[4] = 0;
    printf("%s\n\n  MAKEJED signature : \"%s\"\n", path, sig);
    printf("\n  Stored as ASCII: char = group, bit = block, at\n"
           "  address = block*128 + 104 + group*8 + 1.\n"
           "  A '?' means the byte is not printable ASCII -- reported, not guessed.\n");
    jed_free(&jf);
    return 0;
}

static int cmd_uim(const char *path) {
    JedFile jf;
    const DevProfile *p;
    int blk, oi, g, bit, n = 0, amb = 0, hd_bank2 = 0;
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    if (geometry_gate(&jf)) { jed_free(&jf); return 2; }
    if (target_gate(&jf, 1)) { jed_free(&jf); return 2; }
    /* `uim` accepts XC7354 now that its PIN source band is measured.
     * `decode` still REFUSES it: the MACROCELL-OUTPUT band is unmeasured, so
     * any literal fed by another macrocell cannot be named, and a decode that
     * names some literals and invents the rest is the silent lie this project
     * exists to prevent. */
    if (!device_rows_supported(&jf)) { jed_free(&jf); return 2; }
    p = profile_for(&jf);
    if (p->uim_blk_lo < 0) {
        fprintf(stderr, "the UIM block extent is not established for %s "
                        " -- refusing to scan an unmeasured range\n",
                jf.device);
        jed_free(&jf); return 2;
    }
    printf("%s -- UIM crosspoints\n\n", path);
    printf("  src(block,offset)  slot      line     dest   source\n");
    {

    const int P108_ = p->row_per_record;
    const int *OFFS_ = P108_ ? uim_offs12 : uim_offs;
    const int NOFF_ = P108_ ? 12 : 6;
    const int LPO_ = P108_ ? 4 : 8;      /* lines per bank  */
    const int GMU_ = P108_ ? 16 : 1;     /* group multiplier */
    const int NBIT_ = P108_ ? 12 : 8;    /* destination bits per group */
    const int LAST_LO_ = P108_ ? 52 : 48, LAST_HI_ = P108_ ? 116 : 112;
    for (blk = p->uim_blk_lo; blk <= p->uim_blk_hi; blk++)
        for (oi = 0; oi < NOFF_; oi++)
            for (g = 0; g < LPO_; g++)
                for (bit = 0; bit < NBIT_; bit++) {
                    int dfb = fb_of_rowbit(p, bit);
                    if (!dfb) continue;      /* no FB at this bit here */
                    /* IDENTIFIED. A HIGH-DENSITY block has only
                     * TWENTY-ONE UIM inputs -- the fitter says so itself,
                     * `.MAP` reporting `UIM inputs - n/21` against `n/24` for
                     * a FAST block, and 21 sources saturate it while 22 will
                     * not place. The line axis addresses 24 (3 banks x 8), so
                     * lines 21, 22 and 23 DO NOT EXIST on an HD block, and
                     * their crosspoints -- bank 2, groups 5, 6 and 7 -- are
                     * held at a constant 1. That is the ~162 bits per block,
                     * invariant across all 265 XC7354 bitmaps.
                     * The block's other three inputs are the 3 FAST INPUTS,
                     * which bypass the UIM entirely. Groups 0..4 of bank 2
                     * are real lines 16..20 and ARE reported. */
                    /* was `p->nfb == 6`. XC7372 has EIGHT blocks and
                     * the same 21-of-24 High-Density line limit -- the fitter
                     * states it, `Fast inputs - 0/3` and `Shared pterms -
                     * 0/12` for FB3..FB8 against `0/12` and `0/0` for the two
                     * FAST blocks. With the guard keyed to six, bank 2's
                     * constant-1 filler decoded as real connections and one
                     * source appeared to drive five destination blocks. */
                    if (p->nfb >= 6 && dfb > p->fast_upto && g >= 5 &&
                        (OFFS_[oi] == LAST_LO_ || OFFS_[oi] == LAST_HI_)) {
                        hd_bank2++;
                        continue;
                    }
                    if (jed_owned(&jf, (size_t)blk, OFFS_[oi], g * GMU_, bit, 0)) {
                        char nm[32];
                        /* The offset selects the BANK and the half. */
                        static const int bank_of[6] = { 0, 1, 2, 0, 1, 2 };
                        static const int half_of[6] = { 0, 0, 0, 1, 1, 1 };
                        static const int bank12[12] = { 0,1,2,3,4,5, 0,1,2,3,4,5 };
                        /* FIFTH reader of the slot base. */
                        int i = 2 * (blk - p->uim_blk_lo) + (P108_ ? (oi >= 6) : half_of[oi]);
                        int ln = LPO_ * (P108_ ? bank12[oi] : bank_of[oi]) + g;
                        int named = (p->nfb >= 6) ? slot_name_7354(i, nm, sizeof nm,
                                       jf.device)
                                                  : slot_name(i, nm, sizeof nm);
                        if (named) {
                            printf("  (%3d,%3d)  slot %-3d  line %-2d   FB%d   "
                                   "%s (by rule)\n",
                                   blk, OFFS_[oi], i, ln, dfb, nm);
                            amb = 1;
                        } else {
                            printf("  (%3d,%3d)  slot %-3d  line %-2d   FB%d   "
                                   "UNNAMED -- slot outside the mapped ranges\n",
                                   blk, uim_offs[oi], i, ln, dfb);
                        }
                        n++;
                    }
                }
    }
    printf("\n%d crosspoint(s).\n", n);
    if (hd_bank2)
        printf("\n%d crosspoint(s) of UIM lines 21, 22 and 23 into HIGH-DENSITY\n"
               "blocks were NOT reported: THOSE LINES DO NOT EXIST. An\n"
               "HD block has 21 UIM inputs, not 24 -- `.MAP` reports\n"
               "`UIM inputs - n/21` against `n/24` for a FAST block, 21 sources\n"
               "saturate it and 22 will not place. Their crosspoints sit at a\n"
               "constant 1 in every one of 265 XC7354 bitmaps. The block's other\n"
               "three inputs are the 3 FAST INPUTS, which bypass the UIM.\n",
               hd_bank2);
    if (amb)
        printf("\nCAVEAT: names are BY RULE, i = 16*fb + (mc-1),\n"
               "verified on 17 macrocells with three advance predictions. A\n"
               "DEDICATED INPUT PIN measures to slot 32, which the rule also\n"
               "gives to FB2_MC1 -- the bitmap cannot tell them apart. Treat a\n"
               "named source as \"this slot\", not as a proven signal.\n");
    jed_free(&jf);
    return 0;
}

/* FORWARD-ENUMERATE every writable address each real
 * structure family can address, calling the SAME arithmetic the actual
 * READERS use (`row_fuse`, `cfg_bit_x`, `cmd_uim`'s own crosspoint loop,
 * `x108_line_block`/`x108_claims_line`) -- not a re-derivation from the
 * flat address backward, and not `name_bit`'s own (separately-measured,
 * NAMING-only) tables, which the history shows can
 * legitimately diverge from what a given family's REAL reader computes.
 * One line per (address, structure, detail); the SAME address can and does
 * get multiple lines when two families' records legitimately alias
 * (the "compressed by aliasing" finding) -- reported, not merged. */
static int cmd_structuremap(const char *devname) {
    JedFile jf;
    const DevProfile *p;
    int fb, mc, row, blk;
    memset(&jf, 0, sizeof jf);
    snprintf(jf.device, sizeof jf.device, "%s", devname);
    p = profile_for(&jf);

    /* A) AND-ARRAY ROWS -- row_fuse's own addressing, blk 0..47 (the EXACT
     * native loop bound every real caller of row_fuse uses, verified by
     * grep: `for (blk = 0; blk < 48; blk++)`, src/main.c). */
    for (fb = 1; fb <= p->nfb; fb++) {
        int bit = p->fbbit[fb - 1];
        if (bit < 0) continue;
        for (mc = 1; mc <= 9; mc++) {
            int base = mc_group_base(mc);
            for (row = 0; row < 5; row++) {
                for (blk = 0; blk < 48; blk++) {
                    long addr; int off, grp;
                    if (p->split_mc1 && mc == 1) {
                        if (row == 0) { off = 88; grp = 0; }
                        else          { off = 90; grp = row - 1; }
                    } else if (p->row_per_record) {
                        int r = p->row_g_hd + row; /* g0 is 0 on every
                                                     * measured profile */
                        if (r >= 4) { off = base + 4; grp = 0; }
                        else        { off = base; grp = 2 * r; }
                    } else {
                        off = base; grp = p->row_g_hd + row;
                    }
                    addr = (long)blk * 128 + off + grp * 8 + bit;
                    printf("%ld AND_ARRAY_ROW FB%d_MC%d row%d blk%d\n",
                           addr, fb, mc, row, blk);
                }
            }
            /* (sec.NNN): XC7372's MC1 own SIXTH
             * private row (group5, mc_priv_rows_x()'s own return of 6
             * for a buried export-donor MC1, --
             * this forward enumeration only ever swept row<5 (matching
             * the ORIGINAL five-row model), so every address in this
             * family read UNASSIGNED despite being genuinely read and
             * consumed by decode. The physical ADDRESS exists on this
             * device regardless of whether a SPECIFIC bitmap's own MC1
             * happens to be an exporter -- enumerated unconditionally,
             * matching how row0..4 are already enumerated for every
             * macrocell whether or not it is actually in use. */
            if (mc == 1 && strstr(devname, "XC7372"))
                for (blk = 0; blk < 48; blk++) {
                    long addr = (long)blk * 128 + base + 5 * 8 + bit;
                    printf("%ld AND_ARRAY_ROW FB%d_MC1 row5(export) blk%d\n",
                           addr, fb, blk);
                }
        }
    }

    /* B) SHARED PRODUCT-TERM POOL -- shared_pt_fuse()'s own addressing,
     * A device-wide (off,grp) family entirely OUTSIDE (A)'s
     * per-macrocell row-base table, read for any HD macrocell needing
     * more than 5 private product terms. Never previously enumerated
     * here at all (the "read but no structural tag" finding,
     * -- copied locally, not re-derived (rule 4),
     * the SAME 12-entry table `shared_pt[]`/`name_bit()` already use. */
    if (p->nfb >= 6) {
        static const struct { int off, grp; } spt[12] = {
            {0,0},{0,1},{0,2},{0,3},{0,4},{8,0},{8,1},{8,2},
            {16,0},{16,1},{8,3},{8,4}
        };
        int si;
        for (fb = p->fast_upto + 1; fb <= p->nfb; fb++) {
            int bit = p->fbbit[fb - 1];
            if (bit < 0) continue;
            for (si = 0; si < 12; si++)
                for (blk = 0; blk < 48; blk++) {
                    long addr = (long)blk * 128 + spt[si].off +
                                spt[si].grp * 8 + bit;
                    printf("%ld SHARED_PT FB%d row%d blk%d\n",
                           addr, fb, si, blk);
                }
        }
    }

    /* C) CONFIG/ATTRIBUTE WORD -- cfg_bit_x's own addressing, swept over
     * every named attribute block in the profile (cfg_fast[8], cfg_hd[8],
     * plus the four scalar attributes) x every (fb,mc) the profile can
     * name a bit for. The FAST/HD label distinguishes WHICH range a block
     * came from; a block value of -1 (not located on this device) is
     * skipped, not guessed. */
    {
        static const char *const attrlab[8] =
            { "OEused","OEsel","reg","fclk","toggle","pwr","set","reset" };
        int which, k;
        for (which = 0; which < 2; which++) {
            const int *blks = which ? p->cfg_hd : p->cfg_fast;
            const char *range = which ? "HD" : "FAST";
            for (k = 0; k < 8; k++) {
                int ablk = blks[k];
                if (ablk < 0) continue;
                for (fb = 1; fb <= p->nfb; fb++) {
                    int is_fast_fb = (fb <= p->fast_upto);
                    int bit = is_fast_fb ? p->cfg_fbbit_fast[fb - 1]
                                          : p->cfg_fbbit[fb - 1];
                    if (bit < 0) continue;
                    for (mc = 1; mc <= 9; mc++) {
                        long addr; int off, idx;
                        if (p->row_per_record) {
                            int grp = (mc == 5) ? 0 : cfg_group(mc);
                            off = (mc == 5) ? 100 : cfg_offset(mc);
                            idx = 16 * grp + bit;
                            if (idx > 63) continue;
                            addr = (long)ablk * 128 + off + idx;
                        } else {
                            off = cfg_offset(mc);
                            addr = (long)ablk * 128 + off +
                                   cfg_group(mc) * 8 + bit;
                        }
                        printf("%ld CONFIG_ATTR %s[%d]=%s blk%d FB%d_MC%d\n",
                               addr, range, k, attrlab[k], ablk, fb, mc);
                    }
                }
            }
        }
        {
            struct { int blk; const char *label; } scalars[4] = {
                { p->cfg_fbinuse, "FBinuse" }, { p->cfg_localfb, "LocalFB" },
                { p->cfg_alloc, "Alloc" }, { p->cfg_polarity, "Polarity" }
            };
            int s;
            for (s = 0; s < 4; s++) {
                int ablk = scalars[s].blk;
                if (ablk < 0) continue;
                for (fb = 1; fb <= p->nfb; fb++) {
                    int is_fast_fb = (fb <= p->fast_upto);
                    int bit = is_fast_fb ? p->cfg_fbbit_fast[fb - 1]
                                          : p->cfg_fbbit[fb - 1];
                    if (bit < 0) continue;
                    for (mc = 1; mc <= 9; mc++) {
                        long addr; int off, idx;
                        if (p->row_per_record) {
                            int grp = (mc == 5) ? 0 : cfg_group(mc);
                            off = (mc == 5) ? 100 : cfg_offset(mc);
                            idx = 16 * grp + bit;
                            if (idx > 63) continue;
                            addr = (long)ablk * 128 + off + idx;
                        } else {
                            off = cfg_offset(mc);
                            addr = (long)ablk * 128 + off +
                                   cfg_group(mc) * 8 + bit;
                        }
                        printf("%ld CONFIG_ATTR SCALAR=%s blk%d FB%d_MC%d\n",
                               addr, scalars[s].label, ablk, fb, mc);
                    }
                }
            }
        }
    }

    /* D) UIM CROSSPOINT -- the EXACT loop `cmd_uim` runs (copied, not
     * re-derived: same OFFS_/NOFF_/LPO_/GMU_/NBIT_/LAST_LO_/LAST_HI_
     * selection, same skip conditions). */
    if (p->uim_blk_lo >= 0) {
        int oi, g, bit;
        const int P108_ = p->row_per_record;
        const int *OFFS_ = P108_ ? uim_offs12 : uim_offs;
        const int NOFF_ = P108_ ? 12 : 6;
        const int LPO_ = P108_ ? 4 : 8;
        const int GMU_ = P108_ ? 16 : 1;
        const int NBIT_ = P108_ ? 12 : 8;
        const int LAST_LO_ = P108_ ? 52 : 48, LAST_HI_ = P108_ ? 116 : 112;
        for (blk = p->uim_blk_lo; blk <= p->uim_blk_hi; blk++)
            for (oi = 0; oi < NOFF_; oi++)
                for (g = 0; g < LPO_; g++)
                    for (bit = 0; bit < NBIT_; bit++) {
                        long addr = (long)blk * 128 + OFFS_[oi] +
                                    (long)(g * GMU_) * 8 + bit;
                        int dfb = fb_of_rowbit(p, bit);
                        if (!dfb) {
                            printf("%ld UIM_CROSSPOINT PADDING(no_fb_at_bit) "
                                   "blk%d off%d\n", addr, blk, OFFS_[oi]);
                            continue;
                        }
                        if (p->nfb >= 6 && dfb > p->fast_upto && g >= 5 &&
                            (OFFS_[oi] == LAST_LO_ || OFFS_[oi] == LAST_HI_)) {
                            printf("%ld UIM_CROSSPOINT "
                                   "PADDING(hd_bank2_const1) FB%d\n",
                                   addr, dfb);
                            continue;
                        }
                        printf("%ld UIM_CROSSPOINT dest_FB%d blk%d off%d\n",
                               addr, dfb, blk, OFFS_[oi]);
                    }
    }

    /* E) UIM LINE-CLAIM FAMILY (PAD SOURCES) -- XC73108 (row_per_record)
     * only. `uim_sources_for_line`'s own "input pads claim slots" branch,
     * src/main.c ~5380-5396) -- the DEDICATED-INPUT-PAD
     * twin of the MC-source UIM_LINE_CLAIM family below. This family was
     * NEVER enumerated here at all before -- traced as one of
     * the two dominant read-but-unnamed clusters on XC73108,
     * found by empirically tracing a real
     * `XC73_FUSE_READ_LOG` tuple (block=80, rec_off=32, group=4, bit=0)
     * back through source: `cmd_uim`'s own crosspoint loop and
     * `x108_line_of_rank`'s generic (bank,half) model BOTH provably
     * cannot reach it (neither's own address formula has a solution for
     * this tuple, checked directly, all 12 offsets x g=0..3 x bit=0..11),
     * but `x108_pad_addr`'s own dedicated-pad table (PC84 pin 79 ->
     * blk80/off32, does. The 12-slot (block,offset) table below
     * is COPIED from `x108_pad_addr`, not re-derived (Rule 4) --
     * PACKAGE-INVARIANT by direct comparison (PC84's own 12 measured
     * points, and PQ160/PG144/BG225's own, resolve
     * to the IDENTICAL {80,81,82,88,89,90}x{32,96} address set; only the
     * PIN IDENTITY differs by package, which this address-only
     * enumeration does not need). `band`/`slot`/`cfg_fbbit` mirror the
     * real function's own `bit = 16*(line%4) + cfg_fbbit[fb-1]`,
     * `band = line/4` exactly. */
    if (p->row_per_record) {
        static const int PADBLK[12] =
            { 88, 88, 89, 89, 90, 90, 80, 80, 81, 81, 82, 82 };
        static const int PADOFF[12] =
            { 32, 96, 32, 96, 32, 96, 32, 96, 32, 96, 32, 96 };
        static const int PBAND[6] = { 96, 100, 104, 108, 112, 116 };
        int pi, band, slot, dfb2;
        for (pi = 0; pi < 12; pi++) {
            int blk2 = PADBLK[pi], off2 = PADOFF[pi];
            for (band = 0; band < 6; band++) {
                int rec_off2 = (off2 == 32) ? 32 + 4 * band : PBAND[band];
                for (slot = 0; slot < 4; slot++) {
                    for (dfb2 = 1; dfb2 <= p->nfb; dfb2++) {
                        int cbit2 = p->cfg_fbbit[dfb2 - 1];
                        long addr2;
                        int bit2;
                        if (cbit2 < 0) continue;
                        bit2 = 16 * slot + cbit2;
                        if (bit2 > 63) continue;
                        addr2 = (long)blk2 * 128 + rec_off2 + bit2;
                        printf("%ld UIM_LINE_CLAIM src_PAD%d dest_FB%d "
                               "line%d\n", addr2, pi, dfb2,
                               band * 4 + slot);
                    }
                }
            }
        }
    }

    /* E2) UIM LINE-CLAIM FAMILY (ORDINARY MACROCELL PAD SOURCES) --
     * XC73108 (row_per_record) only,. The SAME
     * `uim_sources_for_line` "input pads claim slots" loop that section
     * E above enumerates (src/main.c, `uim_sources_for_line`, the
     * `for (pin = 1; pin <= 84 ...)` loop) ALSO calls `x108_pad_addr`
     * for physical pins OUTSIDE the 12 dedicated-input ranges section E
     * already covers. Those pins fall through to `x108_pad_addr`'s own
     * GENERIC `device_pin_macrocell`+`pb[]` branch -- the ORDINARY
     * macrocell-pad lookup, live for the four function blocks with a
     * nonzero `pb[]` base (FB7/FB8/FB9/FB10 only) -- plus the FB10_MC9/
     * PIN11 special case, PC84-only, block 92). Section E's
     * own 12-slot PADBLK/PADOFF table never covered this branch: found
     * by re-running(e)'s own instruction (resume the empirical
     * tuple-finding pass for the 1,184 UNRESOLVED RESIDUAL addresses) --
     * a SINGLE real board's decode (`gt_bm_H10.jed`, PC84, independently
     * re-run twice, once via a scan and once standalone)
     * produced a `XC73_FUSE_READ_LOG` reader tuple for EVERY ONE of the
     * 1,184, not a sample. Traced to this formula and verified
     * PROGRAMMATICALLY, not by inspection: the candidate address set
     * this enumeration produces is a STRICT SUPERSET of all 1,184
     * residual addresses (0 missed; the extra candidates are
     * never-exercised (fb,mc,band,slot,dest_fb) combinations in the
     * same family, the same "aliasing/over-enumeration" shape section
     * E's own comment and(d)'s XC7372 "+193" already document,
     * not an error). `pb[]`/`(mc-1)/2`/`(mc-1)&1` are COPIED from
     * `x108_pad_addr`, not called (Rule 4). Block 92 is emitted
     * unconditionally here even though `x108_pad_addr` gates it to
     * PC84 -- this command takes no package argument, matching section
     * E's own precedent of merging package-specific pad families into
     * one device-wide table (PC84/PQ160/PG144/BG225's dedicated-input
     * band is a single PACKAGE-INVARIANT address set by direct
     * comparison). NOT independently re-verified against a
     * non-PC84 real board; PC84 is real (the
     * corpus has 22 real PC84 XC73108 boards) so this is not a purely
     * hypothetical branch either way. */
    if (p->row_per_record) {
        static const int PADFB2[4] = { 7, 8, 9, 10 };
        static const int PADBASE2[4] = { 208, 336, 344, 216 };
        static const int PBAND2[6] = { 96, 100, 104, 108, 112, 116 };
        int fi, mc2, band, slot, dfb2;
        for (band = 0; band < 6; band++) {
            int rec_off2 = 32 + 4 * band;
            for (slot = 0; slot < 4; slot++) {
                for (dfb2 = 1; dfb2 <= p->nfb; dfb2++) {
                    int cbit2 = p->cfg_fbbit[dfb2 - 1];
                    long addr2; int bit2;
                    if (cbit2 < 0) continue;
                    bit2 = 16 * slot + cbit2;
                    if (bit2 > 63) continue;
                    addr2 = (long)92 * 128 + rec_off2 + bit2;
                    printf("%ld UIM_LINE_CLAIM src_PADMC_FB10_9 dest_FB%d "
                           "line%d\n", addr2, dfb2, band * 4 + slot);
                }
            }
        }
        for (fi = 0; fi < 4; fi++) {
            int sfb2 = PADFB2[fi], base2 = PADBASE2[fi];
            for (mc2 = 1; mc2 <= 9; mc2++) {
                int blk2 = base2 + (mc2 - 1) / 2;
                int off2 = ((mc2 - 1) & 1) ? 96 : 32;
                for (band = 0; band < 6; band++) {
                    int rec_off2 = (off2 == 32) ? 32 + 4 * band
                                                 : PBAND2[band];
                    for (slot = 0; slot < 4; slot++) {
                        for (dfb2 = 1; dfb2 <= p->nfb; dfb2++) {
                            int cbit2 = p->cfg_fbbit[dfb2 - 1];
                            long addr2; int bit2;
                            if (cbit2 < 0) continue;
                            bit2 = 16 * slot + cbit2;
                            if (bit2 > 63) continue;
                            addr2 = (long)blk2 * 128 + rec_off2 + bit2;
                            printf("%ld UIM_LINE_CLAIM src_PADMC_FB%d_%d "
                                   "dest_FB%d line%d\n", addr2, sfb2, mc2,
                                   dfb2, band * 4 + slot);
                        }
                    }
                }
            }
        }
    }

    /* D) UIM LINE-CLAIM FAMILY -- XC73108 (row_per_record) only.
     * `x108_line_block`/`x108_claims_line`'s own addressing, forward. */
    if (p->row_per_record) {
        static const int BOFF[6] = { 96, 100, 104, 108, 112, 116 };
        static const int BLOW[6] = { 32,  36,  40,  44,  48,  52 };
        /* x108_line_base's own table, copied (not called): that function is
         * defined LATER in this file and this command sits before it, same
         * shape as name_bit's own already-documented LBASE copy --
         * "the table is copied, not the address". */
        static const int LBASE12[12] =
            { 96,104,352,224,112,240,368,376,248,120,232,360 };
        int sfb, smc, dfb, line;
        for (sfb = 1; sfb <= 12; sfb++)
            for (smc = 1; smc <= 9; smc++) {
                static const int d[9] = { 4,5,5,0,0,1,1,2,2 };
                int lblk = LBASE12[sfb - 1];
                if (lblk < 0) continue;
                lblk += d[smc - 1];
                for (dfb = 1; dfb <= p->nfb; dfb++) {
                    int cbit = p->cfg_fbbit[dfb - 1];
                    if (cbit < 0) continue;
                    for (line = 0; line < 24; line++) {
                        int band = line / 4, slot = line % 4;
                        int bit = 16 * slot + cbit;
                        long addr; int off;
                        if (bit > 63) continue;
                        off = ((smc & 1) != 0) ? BOFF[band] : BLOW[band];
                        addr = (long)lblk * 128 + off + bit;
                        printf("%ld UIM_LINE_CLAIM src_FB%d_MC%d dest_FB%d "
                               "line%d\n", addr, sfb, smc, dfb, line);
                    }
                }
            }
    }
    return 0;
}

/* THE `.VMH` `bits`/`routes` RECORD
 * PARSER, at the framing confirmed on
 * 12 matched-pair `.VMH` files:
 * a `bits\0` or `routes\0` tag is followed by 12 bytes of record header,
 * then an int32 LE payload beginning at tag+17, fenced by a run of
 * 0xFFFFFFFF.
 *
 * WHAT THIS ITEM ESTABLISHED, DISCLOSED AT THE LEVEL IT HOLDS (COUNT/
 * BYTE level, NOT per-slot source identity -- that remains open, exactly
 * as itself said this would need its own dedicated campaign):
 *   - the framing reproduces cleanly across all 12 designs;
 *   - on THIS ITEM'S OWN 12 designs the number of `routes\0` tags equals
 *     the number of macrocells whose equation reads a non-local
 *     (cross-macrocell) signal (4 of 4, N_1_4..N_1_7) -- BUT a corpus-
 * scale validation, `validate_population.py`, 2,966
 *     real+generated designs across all 6 device profiles) REFUTES this
 *     as a general rule: `n_routes` is ~4 (occasionally 5, rarely more)
 *     on the overwhelming majority of designs REGARDLESS of how many
 *     macrocells actually reference another macrocell's signal in the
 *     `.EQN` (consistency 0.0%-31.2% by device -- worse than chance),
 *     and REGARDLESS of the device's own function-block count (XC7318
 *     has 2 FBs, XC73108 has 12, both still show `n_routes`~4). This
 *     item's own 12-design match was A COINCIDENCE OF SAMPLE
 *     CONSTRUCTION (every one of those 12 designs happened to have
 *     exactly 4 UIM-consuming macrocells BY DESIGN), not a property of
 *     the format -- exactly the "a pattern that fits every design in
 *     your own campaign is not a model" trap this project's own rules
 *     name. What `n_routes`~4 actually counts is UNESTABLISHED.
 *   - a MOVE (the same source SET, reordered in the written equation
 *     text) leaves the macrocell's own bits/routes BINARY records
 *     byte-IDENTICAL -- only surrounding ASCII text-table entries
 *     elsewhere in the file change (pair1: V36P1A vs V36P1B, 0 of 302
 *     words differ in either record; verified again on pair4);
 *   - an ADD (2 sources -> 3 sources on the same macrocell) does NOT
 *     change the record's OWN WORD COUNT (302 words either way, measured at the true fence) --
 *     ruling out "one entry per source" as the record's basic shape.
 *     The specific field that names WHICH source was added is NOT
 *     isolated: most differing words are explained by two distinct
 *     constant byte-offset shifts (+76, +96) landing at different byte
 *     lanes of packed 32-bit words, consistent with downstream string-
 *     table pointers shifting when a name is added elsewhere in the
 *     file, not with a clean per-source index field.
 * This is a real advance on (framing now has controls; two new
 * measured correlations) but does NOT reach per-slot source-macrocell
 * decoding -- `vmhroutes` is a diagnostic dump/count-consistency tool,
 * not a route decoder. */

static unsigned char *vmhr_read_whole(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n > 0 ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = n;
    return buf;
}

/* Every offset in `buf[0..len)` where `tag` (including its own trailing
 * NUL, `taglen` bytes) occurs. Returns a malloc'd array via *out, count
 * via return value; caller frees *out. */
static int vmhr_find_tag(const unsigned char *buf, long len,
                          const char *tag, int taglen, long **out) {
    long cap = 64, n = 0, i;
    long *arr = (long *)malloc(sizeof(long) * (size_t)cap);
    for (i = 0; i + taglen <= len; i++) {
        if (memcmp(buf + i, tag, (size_t)taglen) == 0) {
            if (n == cap) { cap *= 2; arr = (long *)realloc(arr, sizeof(long) * (size_t)cap); }
            arr[n++] = i;
        }
    }
    *out = arr;
    return (int)n;
}

/* Payload at tag+17: int32 LE words until 0xFFFFFFFF (or EOF, or maxwords). */
static int vmhr_parse_record(const unsigned char *buf, long len, long tag_off,
                              unsigned long *out, int maxwords) {
    long off = tag_off + 17;
    int n = 0;
    while (off + 4 <= len && n < maxwords) {
        unsigned long w = (unsigned long)buf[off] | ((unsigned long)buf[off+1] << 8) |
                           ((unsigned long)buf[off+2] << 16) | ((unsigned long)buf[off+3] << 24);
        if (w == 0xFFFFFFFFUL) break;
        out[n++] = w;
        off += 4;
    }
    return n;
}

static int cmd_vmhroutes(const char *path) {
    long len; unsigned char *buf = vmhr_read_whole(path, &len);
    long *bits_off, *routes_off; int n_bits, n_routes, i;
    if (!buf) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    n_bits = vmhr_find_tag(buf, len, "bits\0", 5, &bits_off);
    n_routes = vmhr_find_tag(buf, len, "routes\0", 7, &routes_off);
    printf("%s: %ld bytes, %d 'bits' tags, %d 'routes' tags\n",
           path, len, n_bits, n_routes);
    for (i = 0; i < n_routes; i++) {
        unsigned long words[320];
        int n = vmhr_parse_record(buf, len, routes_off[i], words, 320);
        int k, show = n < 8 ? n : 8;
        printf("  routes[%d] @%ld: %d words, first %d:", i, routes_off[i], n, show);
        for (k = 0; k < show; k++) printf(" %lu", words[k]);
        printf("%s\n", n > show ? " ..." : "");
    }
    free(bits_off); free(routes_off); free(buf);
    return 0;
}

static int vmhr_words_equal(const unsigned long *a, int na,
                             const unsigned long *b, int nb) {
    int i;
    if (na != nb) return 0;
    for (i = 0; i < na; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int cmd_vmhroutes_selftest(void) {
    const char *A1 = "testdata/vmhroutes/V36P1A.VMH";
    const char *B1 = "testdata/vmhroutes/V36P1B.VMH";
    const char *A2 = "testdata/vmhroutes/V36P2A.VMH";
    const char *B2 = "testdata/vmhroutes/V36P2B.VMH";
    int fails = 0;
    long lenA1, lenB1, lenA2, lenB2;
    unsigned char *bufA1 = vmhr_read_whole(A1, &lenA1);
    unsigned char *bufB1 = vmhr_read_whole(B1, &lenB1);
    unsigned char *bufA2 = vmhr_read_whole(A2, &lenA2);
    unsigned char *bufB2 = vmhr_read_whole(B2, &lenB2);
    if (!bufA1 && !bufB1 && !bufA2 && !bufB2) {
        printf("vmhroutes --selftest: SKIPPED -- the .VMH fixtures "
               "(testdata/vmhroutes/) are not included in the release build\n");
        return 0;
    }
    if (!bufA1 || !bufB1 || !bufA2 || !bufB2) {
        fprintf(stderr, "vmhroutes --selftest: fixture set incomplete "
                "under testdata/vmhroutes/ (need V36P1A/B and V36P2A/B .VMH)\n");
        free(bufA1); free(bufB1); free(bufA2); free(bufB2);
        return 1;
    }

    /* CHECK 1: a FIXTURE REGRESSION only -- routes-tag count == 4 on
     * these four committed files, by construction of THIS item's own
     * 12-design sample (each has exactly 4 UIM-consuming macrocells,
     * N_1_4..N_1_7). NOT a general device rule: the
     * corpus-scale run refutes "n_routes == count of UIM-consuming
     * macrocells" (0.0%-31.2% consistency across 2,966 real+generated
     * designs, all 6 device profiles) -- see the comment above
     * cmd_vmhroutes. This check exists only to catch a future edit
     * silently breaking the parser on these known files. */
    {
        long *o; int n;
        int i;
        const unsigned char *bufs[4] = { bufA1, bufB1, bufA2, bufB2 };
        long lens[4] = { lenA1, lenB1, lenA2, lenB2 };
        const char *names[4] = { "V36P1A", "V36P1B", "V36P2A", "V36P2B" };
        for (i = 0; i < 4; i++) {
            n = vmhr_find_tag(bufs[i], lens[i], "routes\0", 7, &o);
            free(o);
            if (n != 4) {
                printf("FAIL check1 (%s): routes tag count %d != 4\n", names[i], n);
                fails++;
            } else {
                printf("PASS check1 (%s): routes tag count == 4 (fixture regression only, not a general rule --)\n", names[i]);
            }
        }
    }

    /* CHECK 2 (MOVE-invariance, POSITIVE): the FIRST routes record
     * (N_1_4) is byte-identical between V36P1A (source order
     * N_2_1,N_3_1,N_4_1) and V36P1B (order N_4_1,N_2_1,N_3_1) -- the
     * measured "written order does not move the binary record" finding. */
    {
        long *oA, *oB; int nA, nB;
        unsigned long wA[320], wB[320]; int na, nb;
        nA = vmhr_find_tag(bufA1, lenA1, "routes\0", 7, &oA);
        nB = vmhr_find_tag(bufB1, lenB1, "routes\0", 7, &oB);
        if (nA >= 1 && nB >= 1) {
            na = vmhr_parse_record(bufA1, lenA1, oA[0], wA, 320);
            nb = vmhr_parse_record(bufB1, lenB1, oB[0], wB, 320);
            if (vmhr_words_equal(wA, na, wB, nb)) {
                printf("PASS check2: MOVE pair's N_1_4 routes record is byte-identical (%d words)\n", na);
            } else {
                printf("FAIL check2: MOVE pair's N_1_4 routes record DIFFERS (expected identical)\n");
                fails++;
            }
        } else {
            printf("FAIL check2: could not locate routes[0] in one of the MOVE pair files\n");
            fails++;
        }
        free(oA); free(oB);
    }

    /* CHECK 3 (ADD, record length invariance): V36P2A's N_1_4 (2 sources)
     * and V36P2B's N_1_4 (3 sources) routes records both parse to
     * EXACTLY 302 words (the true fence, not a truncation artefact). */
    {
        long *oA, *oB; int nA, nB;
        unsigned long wA[320], wB[320]; int na, nb;
        nA = vmhr_find_tag(bufA2, lenA2, "routes\0", 7, &oA);
        nB = vmhr_find_tag(bufB2, lenB2, "routes\0", 7, &oB);
        if (nA >= 1 && nB >= 1) {
            na = vmhr_parse_record(bufA2, lenA2, oA[0], wA, 320);
            nb = vmhr_parse_record(bufB2, lenB2, oB[0], wB, 320);
            if (na == 302 && nb == 302) {
                printf("PASS check3: ADD pair's N_1_4 routes record is 302 words on both sides (measured at the TRUE 0xFFFFFFFF fence, not an arbitrary cap)\n");
            } else {
                printf("FAIL check3: ADD pair's N_1_4 routes record length %d/%d != 302/302\n", na, nb);
                fails++;
            }
        } else {
            printf("FAIL check3: could not locate routes[0] in one of the ADD pair files\n");
            fails++;
        }
        free(oA); free(oB);
    }

    /* CHECK 4, NEGATIVE CONTROL: corrupt one byte inside V36P1A's own
     * N_1_4 routes record (before the fence) and confirm the parsed
     * record then DIFFERS from the uncorrupted reference -- proves
     * check 2's comparison can actually fail, not just always pass. */
    {
        long *oA; int nA;
        unsigned long wRef[320], wBad[320]; int nRef, nBad;
        nA = vmhr_find_tag(bufA1, lenA1, "routes\0", 7, &oA);
        if (nA >= 1) {
            long rec_start = oA[0] + 17;
            unsigned char save;
            nRef = vmhr_parse_record(bufA1, lenA1, oA[0], wRef, 320);
            save = bufA1[rec_start];
            bufA1[rec_start] = (unsigned char)(save ^ 0xFF);
            nBad = vmhr_parse_record(bufA1, lenA1, oA[0], wBad, 320);
            bufA1[rec_start] = save; /* restore */
            if (!vmhr_words_equal(wRef, nRef, wBad, nBad)) {
                printf("PASS check4 (negative control): a corrupted byte IS detected as a difference\n");
            } else {
                printf("FAIL check4 (negative control): corruption was NOT detected -- the comparison cannot fail\n");
                fails++;
            }
        } else {
            printf("FAIL check4: could not locate routes[0] for the negative control\n");
            fails++;
        }
        free(oA);
    }

    free(bufA1); free(bufB1); free(bufA2); free(bufB2);
    printf("\nvmhroutes --selftest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}

/* EVERY ATTRIBUTE BLOCK COMES FROM THE PROFILE.
 *
 * This function used to hardcode 11..23 and the literals 12,13,14,15,17,18,
 * 19,20,21, while `DevProfile` carried the same numbers in `cfg_fast[]`.
 * Nothing read the profile, so its XC7354 half was never exercised and its
 * FB bit map was wrong for both FAST blocks with no code path able to show
 * it. One address, one place (methodology rule 4).
 *
 * An attribute the profile marks -1 is NOT LOCATED on this device and is
 * printed as such. A field that is unknown must never print as a 0. */
static void cfg_line(const JedFile *jf, int fb, int mc, int blk,
                     const char *label, const char *one, const char *zero) {
    int v;
    if (blk < 0) {
        printf("  %-30s : NOT LOCATED on this device\n", label);
        return;
    }
    v = cfg_bit(jf, fb, mc, blk);
    if (v < 0) {
        printf("  blk%-2d %-24s : NOT MEASURED for FB%d on this device\n",
               blk, label, fb);
        return;
    }
    printf("  blk%-2d %-24s : %s\n", blk, label, v ? one : zero);
}

static int cmd_config(const char *path, int fb, int mc) {
    JedFile jf;
    const DevProfile *p;
    const int *cfg;
    int blk, lo, hi, oe_used, oe_sel, alloc;
    if (mc < 1 || mc > 9) {
        fprintf(stderr, "MC must be 1..9\n");
        return 1;
    }
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    if (geometry_gate(&jf)) { jed_free(&jf); return 2; }
    if (target_gate(&jf, 1)) { jed_free(&jf); return 2; }
    /* `config` ACCEPTS XC7354. Its attribute table is now measured for
     * The output enable, registered/clock as a two-bit field,
     * FastCLK, toggle, power, the two-bit async field,
     * polarity and the function-block flag. Attributes NOT located on
     * this device print as NOT LOCATED, and `decode` still REFUSES it -- an
     * inspection command may show what is measured; an emitter may not guess
     * what is not. */
    if (!device_rows_supported(&jf)) { jed_free(&jf); return 2; }
    p = profile_for(&jf);
    if (fb < 1 || fb > p->nfb) {
        fprintf(stderr, "%s has only %d function blocks\n", jf.device, p->nfb);
        jed_free(&jf); return 1;
    }
    cfg = (fb <= p->fast_upto) ? p->cfg_fast : p->cfg_hd;

    printf("%s  FB%d_MC%d -- macrocell configuration\n\n",
           path, fb, mc);

    /* The raw window: the span the named attributes actually occupy on THIS
     * device, not a hardcoded 11..23. */
    lo = p->cfg_raw_lo; hi = p->cfg_raw_hi;
    if (lo < 0) {                 /* extent not established -- show what IS */
        for (blk = 0; blk < 8; blk++)
            if (cfg[blk] >= 0) {
                if (lo < 0 || cfg[blk] < lo) lo = cfg[blk];
                if (cfg[blk] > hi) hi = cfg[blk];
            }
    }
    if (lo >= 0) {
        printf("  raw blocks %d..%d : ", lo, hi);
        for (blk = lo; blk <= hi; blk++) {
            int v = cfg_bit(&jf, fb, mc, blk);
            printf("%c", v < 0 ? '?' : (char)('0' + v));
        }
        if (p->cfg_raw_lo < 0)
            printf("   (span of the LOCATED attributes only -- the extent of\n"
                   "                        this range is NOT established)");
        printf("\n\n");
    }

    /* cfg[0] and cfg[1] are ONE TWO-BIT OUTPUT-ENABLE FIELD, matching
     * the datasheet exactly: "each output buffer can be individually
     * controlled by one of two dedicated Fast Output Enable inputs, or
     * permanently enabled or disabled" -- four states, two bits. */
    oe_used = cfg_bit(&jf, fb, mc, cfg[0]);
    oe_sel  = cfg_bit(&jf, fb, mc, cfg[1]);
    if (cfg[0] < 0 || cfg[1] < 0 || oe_used < 0 || oe_sel < 0)
        printf("  blk%d/%d %-19s : NOT MEASURED on this device\n",
               cfg[0], cfg[1], "output enable");
    else
        printf("  blk%d/%d %-19s : %s\n", cfg[0], cfg[1], "output enable",
               oe_used ? (oe_sel ? "permanently DISABLED -- drives no pin (buried)"
                                 : "permanently ENABLED -- drives its pin")
                       : (oe_sel ? "controlled by FOE1" : "controlled by FOE0"));

    /* on XC7354 HIGH-DENSITY, blk45 and blk46 are ONE TWO-BIT
     * CLOCK-SOURCE field, not the separate "registered" and "FastCLK select"
     * bits measurement showed:
     *     (0,0) combinatorial          34 designs
     *     (1,0) registered, FastCLK    48 designs
     *     (0,1) registered, CLOCK FROM A PRODUCT TERM   4 designs
     * The third state is what `OPTIONS OFF CLOCK_OPT` produces, and
     * reading blk45 ALONE reports such a macrocell as COMBINATORIAL -- a
     * silent wrong answer on a registered design. The (0,1) sample is FOUR,
     * all from matched pairs differing only in CLOCK_OPT. */
    /* was `p->nfb == 6` (XC7354 only). decode.c's
     * OWN correctness-critical reading of this EXACT field already
     * generalized to `NFB >= 6` -- "the encoding is XC7354's",
     * stated for XC7372 with no device-specific caveat, unlike the
     * toggle/polarity fields below which DO have one and are left alone).
     * This diagnostic printer was never brought into line with decode.c's
     * own already-fixed model -- `xc7300dump config` on an XC7372 or
     * XC73108 High-Density macrocell fell through to the GENERIC single-bit
     * `cfg_line` print below instead of this field's real three-state
     * reading. Diagnostic-only: no `.PLD`/`.JED` output is affected. */
    if (p->nfb >= 6 && fb > p->fast_upto) {
        int r45 = cfg_bit(&jf, fb, mc, 45), r46 = cfg_bit(&jf, fb, mc, 46);
        if (r45 >= 0 && r46 >= 0) {
            printf("  blk45/46 %-19s : %s\n", "flip-flop / clock",
                   (!r45 && !r46) ? "combinatorial"
                 : ( r45 && !r46) ? "REGISTERED, clocked from FastCLK"
                 : (!r45 &&  r46) ? "REGISTERED, clock from a PRODUCT TERM"
                                  : "REGISTERED, clocked from FastCLK1"
                                    /* (1,1) is FCLK1, measured on
                                     * FB3, FB5 and FB6 with the clock frozen
                                     * to PIN09; was "NOT MEASURED". */);
        }
    } else
    cfg_line(&jf, fb, mc, cfg[2], "flip-flop", "REGISTERED", "combinatorial");
    if (!(p->nfb >= 6 && fb > p->fast_upto)) {
    if (cfg[2] >= 0 && cfg_bit(&jf, fb, mc, cfg[2]) == 1 && cfg[3] >= 0) {
        int v = cfg_bit(&jf, fb, mc, cfg[3]);
        if (v < 0) printf("  blk%-2d %-24s : NOT MEASURED for FB%d\n",
                          cfg[3], "FastCLK select", fb);
        else       printf("  blk%-2d %-24s : FCLK%d\n", cfg[3],
                          "FastCLK select", v);
    } else if (cfg[3] >= 0) {
        printf("  blk%-2d %-24s : (combinatorial, no clock)\n",
               cfg[3], "FastCLK select");
    }
    }
    cfg_line(&jf, fb, mc, cfg[5], "power mode", "LOW power (PWR LOW)",
             "standard");
    cfg_line(&jf, fb, mc, p->cfg_alloc, "product-term allocation",
             "rows 1..4 (two or more terms)", "row0 (single term)");
    /* named on a SMALL positive sample -- six macrocells in the whole
     * fitter corpus have it clear -- but the rule is exact over 475: 6 hits,
     * 0 misses, 0 false positives, and it predicted both real chips exactly. */
    /* only meaningful at MC8, where it is a per-FUNCTION-BLOCK flag. */
    if (mc == 8 && p->cfg_fbinuse >= 0) {
        int v = cfg_bit(&jf, fb, mc, p->cfg_fbinuse);
        if (v >= 0)
            printf("  blk%-2d %-24s : FB%d %s\n", p->cfg_fbinuse,
                   "function block in use", fb,
                   v ? "has NO logic placed in it" : "HAS logic placed in it");
    }
    cfg_line(&jf, fb, mc, p->cfg_localfb, "local feedback",
             "no (feeds only via the UIM, if at all)",
             "YES -- feeds another macrocell in THIS function block");
    /* cfg[4] is blocks 21..24 on XC7354 HIGH-DENSITY, which
     * showed is the POLARITY field. Printing it as "flip-flop type" too made
     * the tool report TOGGLE and "not inverted" about the SAME FOUR BITS on
     * one screen -- the defect, reintroduced by me. */
    /* `toggle_aliased` -- true exactly when
     * this range's own "toggle" slot IS the polarity slot (no dedicated
     * toggle bit was ever measured for this device/range; currently only
     * XC73108's HIGH-DENSITY range, inherited from XC7354's cfg_hd[4]=21
     * unmodified). MEASURED, not guessed: a D-type/TOGGLE matched pair on
     * XC73108 FB6_MC5 (`.MAP`-confirmed identical placement, real fitter's
     * own `.EQN` confirming `Q := A*B` vs `Q := Q.D1 XOR Q.D2`) -- BOTH
     * printed "blk21 flip-flop type: TOGGLE", because blk21 is reading
     * the (identical, un-inverted) POLARITY of both probes, not toggle
     * status at all. A field that reports the SAME answer for a D-type
     * and a genuine toggle macrocell is not measuring toggle -- it must
     * say so, not guess. */
    { int toggle_aliased = (cfg[4] >= 0 && cfg[4] == p->cfg_polarity);
    if (!(p->nfb == 6 && fb > p->fast_upto)) {
        if (toggle_aliased) {
            printf("  blk%-2d %-24s : NOT MEASURED on this device -- this\n",
                   cfg[4], "flip-flop type");
            printf("  %-31s   slot is the SAME block as polarity here; no\n", "");
            printf("  %-31s   dedicated toggle bit is known\n", "");
        } else {
        /* on XC7372's HIGH-DENSITY blocks the toggle bit is blk24,
         * a field genuinely distinct from polarity (blk21..24 co-move for
         * polarity but toggle touches blk24 ALONE, -- and its SENSE
         * IS INVERTED relative to XC7354's. Measured: the D-type design sets
         * it, the T-type clears it. Swapping the labels is the whole fix; the
         * block was already right. */
        int inv = (strstr(jf.device, "XC7372") != NULL && fb > p->fast_upto);
        cfg_line(&jf, fb, mc, cfg[4], "flip-flop type",
                 inv ? "TOGGLE" : "D-type", inv ? "D-type" : "TOGGLE");
        }
    }
    /* blk19's label is validated on D-TYPE and COMBINATORIAL
     * macrocells. On a TOGGLE macrocell it reads 1 while the fitter's own
     * `.EQN` shows a NON-inverted `Q0.T = A` and the row holds a TRUE literal
     * (`slot0`, not `/slot0`) -- measured on three designs. The bit is not
     * falsified; the LABEL is unverified there, so it is not applied. */
    if (p->nfb == 6 && fb > p->fast_upto) {
        /* 21..24 is ONE polarity field here; the
         * separate "toggle" reading of the same blocks is superseded, and
         * printing both contradicted itself on one screen. */
        int v = cfg_bit(&jf, fb, mc, p->cfg_polarity);
        if (v >= 0)
            printf("  blk21-24 %-19s : %s\n", "equation polarity",
                   v ? "INVERTED" : "not inverted");
    } else if (toggle_aliased) {
        /* no dedicated toggle bit exists here to gate on -- the
         * "NOT INTERPRETED on a TOGGLE" check needs to know
         * toggle status independently of polarity, and here it cannot
         * (they are the SAME bit). Printing the polarity value plainly,
         * with the ambiguity stated, is more honest than reusing the
         * polarity bit itself as if it answered "is this a toggle" --
         * measured to be circular for this device (see just above). */
        int v = cfg_bit(&jf, fb, mc, p->cfg_polarity);
        if (v >= 0) {
            printf("  blk%-2d %-24s : %s\n", p->cfg_polarity,
                   "equation polarity", v ? "INVERTED (/Q :=)" : "not inverted");
            printf("  %-31s   (toggle status unknown on this device, so a\n", "");
            printf("  %-31s   TOGGLE macrocell's reading here is UNVERIFIED,\n", "");
            printf("  %-31s   no dedicated toggle bit exists)\n", "");
        }
    } else
    { /* `inv` matches the flip-flop-type branch's own sense flip
       * exactly -- the SAME raw bit, read the SAME way, so "is this
       * macrocell a toggle" agrees between the two prints instead of
       * contradicting on XC7372 (MEASURED: the unfixed check fired the
       * "NOT INTERPRETED" warning on the confirmed D-type half of a
       * matched pair and stayed silent on the confirmed TOGGLE half --
       * exactly backwards). */
      int inv = (strstr(jf.device, "XC7372") != NULL && fb > p->fast_upto);
      int tg_raw = (cfg[4] >= 0) ? cfg_bit(&jf, fb, mc, cfg[4]) : -1;
      int is_toggle = (tg_raw < 0) ? -1 : (inv ? (tg_raw == 1) : (tg_raw == 0));
      /* (an exhaustive sweep over
       * 920 designs found a REAL instance): this printed the RAW
       * bit at `p->cfg_polarity` (blk21 on XC7354) directly, at a FAST-
       * block address -- exactly the read `cfg_polarity_bit` (this
       * file, exists to avoid: "blk21 is an HD-only field
       * ... reading it at a FAST address returns 0 often enough to emit
       * a NOT-INVERTED equation for a macrocell the fitter always
       * inverts". `decode.c`'s own emission already calls
       * `cfg_polarity_bit`, which special-cases every FAST-block
       * macrocell on an nfb>=6 device to ALWAYS report inverted (152
       * designs, 0 exceptions) -- this diagnostic did not, and so it
       * disagreed with decode's own output on two real corpus designs
       * (`gt_port4_xc7354`, `gt_s33_port_4`, FB1_MC2/MC3 -- confirmed
       * against the REAL FITTER's own `.eqn`: `READY` at FB1_MC3 is
       * `/READY := ...`, genuinely inverted, matching decode and NOT
       * this diagnostic). Same fix shape as: route through
       * the function decode.c itself trusts, not a second, independent
       * raw read of the same bit. */
      if (is_toggle == 1 && p->cfg_polarity >= 0) {
          int v = cfg_polarity_bit(&jf, fb, mc);
          printf("  blk%-2d %-24s : %d -- NOT INTERPRETED on a TOGGLE\n",
                 p->cfg_polarity, "equation polarity", v);
          printf("  %-31s   macrocell: the label is measured on D-type and\n", "");
          printf("  %-31s   combinatorial only\n", "");
      } else if (p->cfg_polarity < 0) {
          printf("  %-30s : NOT LOCATED on this device\n", "equation polarity");
      } else {
          printf("  blk%-2d %-24s : %s\n", p->cfg_polarity, "equation polarity",
                 cfg_polarity_bit(&jf, fb, mc) ? "INVERTED (/Q :=)" : "not inverted");
      } } }
    alloc = (p->cfg_alloc >= 0) ? cfg_bit(&jf, fb, mc, p->cfg_alloc) : -1;
    /* on XC7354 High-Density the async term is a TWO-BIT field.
     * was `p->nfb == 6` -- decode.c's own
     * reading of this SAME field: "the pair is {41,42} here and
     * the encoding is XC7354's") already generalized to `NFB >= 6` for
     * XC7372/XC73108's correctness; this diagnostic printer had not been.
     * Diagnostic-only, no `.PLD`/`.JED` output affected. */
    if (p->nfb >= 6 && fb > p->fast_upto) {
        int a1 = cfg_bit(&jf, fb, mc, 41), a2 = cfg_bit(&jf, fb, mc, 42);
        /* (0,0) printed as "both clear -- NOT
         * MEASURED" -- decode.c's own reading of this SAME field (
         * resolved this exact state earlier: it is a FOURTH state, not an unmeasured one, meaning a
         * DEDICATED SET (row0) AND a DEDICATED RESET (row1) both present
         * on the same macrocell simultaneously -- confirmed against the
         * real fitter's own .EQN, which prints both .SETF and .RSTF. This
         * diagnostic kept telling users "not measured" for a fact decode.c
         * has fully resolved and uses to emit correct .PLD output. */
        if (a1 >= 0 && a2 >= 0)
            printf("  blk41/42 %-19s : %s\n", "async term",
                   (a1 && a2) ? "NONE" : (a1 ? "RESET (.RSTF)"
                                             : (a2 ? "SET (.SETF)"
                                                   : "BOTH SET (.SETF) and RESET (.RSTF)")));
    }
    cfg_line(&jf, fb, mc, cfg[6], "async row0 sense",
             alloc == 0 ? "SET, or row0 is the logic term" : "SET or unused",
             "RESET");

    printf("\n  block 11 is NOT configuration: it is a fixed\n"
           "  function of the MACROCELL NUMBER, and does not vary across 246\n"
           "  bitmaps. Block 23 does NOT vary either, EXCEPT at MC8 in all\n"
           "  four function blocks, which is not explained.\n"
           "  Block 22 is constant 1 over 246 bitmaps and both real chips, and\n"
           "  is NOT named.\n"
           "  Raw values are in the line above.\n");
    jed_free(&jf);
    return 0;
}

/* XC73108's UIM crosspoint, MEASURED 11/11 exact.
 *
 *     offset = cfg_offset(src_mc)      104 for MC1..MC4, 96 for MC5..MC9
 *     bit    = 16 * cfg_group(src_mc) + cfg_bit(src_fb)
 *     blocks = 10, 21, 22              a PROGRAMMED crosspoint reads 0
 *
 * Found by taking a baseline design with the source ABSENT and SUBTRACTING
 * every structure already measured: 251 differing bits reduce to three, one
 * of which is the fuse (method 7z). Four earlier readings were withdrawn
 * because `(mc_group_base(mc), cfg_bit(fb))` -- the source macrocell's OWN
 * AND-array row -- moves in any experiment that adds or relocates a source
 * and is a far larger signal than this single fuse. */
/* MC5 is special: 9-5 = 4 would need a fifth group, which a 64-bit record
 * (four groups of sixteen) does not have -- so MC5 gets its own offset, 100,
 * at group 0, where FB4_MC5 was
 * the ONE source whose fuse did not appear at blocks 10/21/22. */
/* A self-describing slot for XC73108 -- (fb, mc) packed, with a
 * high bit so it can never collide with a smaller device's integer slot. */
#define X108_SLOT(fb,mc)   (0x40000000 | ((fb) << 8) | (mc))
#define X108_PADSLOT(pin)  (0x20000000 | (pin))
#define X108_IS_PAD(s)     (((s) & 0x20000000) != 0)
#define X108_PAD_PIN(s)    ((s) & 0xFF)
#define X108_IS_SLOT(s)    (((s) & 0x40000000) != 0)
#define X108_SLOT_FB(s)    (((s) >> 8) & 0xFF)
#define X108_SLOT_MC(s)    ((s) & 0xFF)

static int x108_cfg_group(int mc) { return mc == 5 ? 0 : (mc < 5 ? 4 - mc : 9 - mc); }
static int x108_cfg_off(int mc)   { return mc == 5 ? 100 : (mc < 5 ? 104 : 96); }

static int x108_uim_connected(const JedFile *jf, const DevProfile *p,
                              int sfb, int smc) {
    static const int blk[3] = { 10, 21, 22 };
    /* `XC73_FAST_GATE=off` treats a FAST-block source as connected.
     * removed this permanently and reverted after K63 -- 25 registers,
     * NINE wired-AND nodes -- failed 14 of 19 pins. A later measurement showed that
     * FAST sources do NOT over-claim (0 false positives, both directions), so
     * the stated reason was withdrawn and the cause is unknown.
     * NOBODY EVER TRIED IT ON A MINIMAL DESIGN. */
    {   /* `getenv()` scans the whole environment array
         * and this function is called ~65M times on a dense design
         * (gprof) -- cache both debug-env reads once per process
         * instead of re-scanning `environ` on every call. Behaviour is
         * IDENTICAL: these variables are read-only diagnostic toggles
         * this project's own probes set once, before running `decode`,
         * never changed mid-run. */
        static int checked = 0, off_flag = 0;
        if (!checked) {
            const char *g = getenv("XC73_FAST_GATE");
            off_flag = (g && g[0]=='o' && g[1]=='f');
            checked = 1;
        }
        if (off_flag && p && sfb <= p->fast_upto) return 1;
    }
    int bit, off, i, n = 0;
    /* THE FAST BLOCKS ARE NOT IN THIS RECORD AT ALL, so asking it
     * about them returns 0 and VETOES a source that is really there.
     *
     * Measured on two designs whose sources are pinned to known macrocells,
     * scanning all 64 bits of blocks 10, 21 and 22:
     *
     *   BN2  sources in FB4 and FB5, consumers FB3, output FB7
     *        bits programmed: 2, 4, 8, 10  = cfg_fbbit of FB5, FB4, FB3, FB7
     *        -- every block the design uses, the HD sources included
     *   BF1  sources in FB1 and FB2, consumers FB3, output FB7
     *        bits programmed: 8, 10        = FB3 and FB7 ONLY
     *        -- **FB1 (bit 0) and FB2 (bit 1) never appear**
     *
     * So this crosspoint record describes HIGH-DENSITY sources. A FAST-block
     * macrocell has no fuse here to find, and `x108_line_of_rank` was
     * dropping every line fed by FB1 or FB2: the block's claimed-line set
     * came up short and every rank past its length was unresolvable. That is
     * `K63`'s ten, and it is why checks kept finding the
     * ADDRESSING correct -- the addressing was never wrong.
     *
     * BUT REMOVING THE GATE IS WRONG AND WAS REVERTED. With
     * `if (p->nfb >= 6 && sfb <= p->fast_upto) return 1;` here, `K63` goes
     * from 10 unresolved terms to ZERO -- and then FAILS THE ROUND CYCLE,
     * 14 of 19 output pins differing. The decoder names FAST-block sources
     * that do not drive the line, and a wrong design is worse than a refusal.
     *
     * What `BF1` measured was that every real FAST source IS found: 18 of 18.
     * It never checked FALSE POSITIVES -- whether the OTHER FAST macrocells
     * also appear to claim. That control was missing, and this is what it
     * would have caught.
     *
     * with `XC73_DUMP_LINES` the two readings were finally compared
     * against the FITTER'S OWN counts instead of against each other, and
     * **this gate is wrong**. Ungated, every block's line set is contiguous
     * from 0, every saturated block sits at capacity (21 High-Density, 24
     * FAST) and the two blocks that are not saturated match their signal
     * count exactly -- FB7 16 of 16, FB11 20 of 20. Gated, all ten are short.
     *
     * It stays anyway. Removing it still fails the SAME 14 of 19 pins on
     * `K63`, so it converts a refusal into a wrong design and nothing more.
     * The remaining fault is bounded to what `K63` has and `BN2` has not --
     * registers, wired-AND, export, or density -- because `BN2` decodes nine
     * consumers of nine correctly with the chain the gate feeds. */
    {   static int checked2 = 0, nogate = 0;
        if (!checked2) { nogate = (getenv("XC73_NOGATE") != NULL); checked2 = 1; }
        if (nogate && p->nfb >= 6 && sfb <= p->fast_upto) return 1;
    }
    if (smc == 5) {                    /* block 20 ONLY */
        bit = 48 + p->cfg_fbbit[sfb - 1];
        if (bit < 0 || bit > 63) return 0;
        /* this required "programmed at 20 AND NOT at 10".
         * measured that on SPARSE designs, where block 10 is mostly
         * unprogrammed and the second half of the test is free. On a PACKED
         * device block 10 is programmed too, so the test rejected EVERY MC5 --
         * including members the fitter names in its own wired-AND table.
         *
         *     packed  FB3_MC5 blk20=0 blk10=0  rejected
         *             FB4_MC5 blk20=0 blk10=0  rejected
         *     sparse  FB4_MC5 blk20=0 blk10=1  accepted
         *
         * The disambiguating fact is the block-20 fuse itself: MC5
         * is the only macrocell addressed there). Block 10 carries other
         * traffic and cannot be used as a negative test. */
        if (!jed_owned(jf, (size_t)20, 104, bit / 8, bit % 8, 1))
            return 3;                  /* programmed at block 20 => MC5 */
        return 0;
    }
    bit = 16 * x108_cfg_group(smc) + p->cfg_fbbit[sfb - 1];
    off = x108_cfg_off(smc);
    if (bit < 0 || bit > 63) return 0;
    for (i = 0; i < 3; i++)
        if (!jed_owned(jf, (size_t)blk[i], off, bit / 8, bit % 8, 1)) n++;  /* programmed reads 0 */
    if (getenv("XC73_TRACE_CONNECTED"))
        fprintf(stderr, "CONNECTED sfb=%d smc=%d bit=%d(grp%d/bit%d) off=%d "
                "blk10=%d blk21=%d blk22=%d n=%d\n",
                sfb, smc, bit, bit/8, bit%8, off,
                !jed_owned(jf, 10, off, bit/8, bit%8, 1),
                !jed_owned(jf, 21, off, bit/8, bit%8, 1),
                !jed_owned(jf, 22, off, bit/8, bit%8, 1), n);
    return n;              /* how many of the three carry the fuse */
}

static int cmd_rows(const char *path, int fb, int mc) {
    JedFile jf;
    int row;
    /* was a literal `fb > 6` -- XC7354's block count, hard-coded in
     * the CLI, which refused every XC7372 and XC73108 block above 6 with
     * "FB must be 1..6". A DEVICE-DRIVEN check already existed thirteen
     * lines further down and had never been reachable, because the literal
     * rejected the call first. Deleting the literal is the whole fix.
     * (method 7n: a device shape is never a literal.) */
    if (mc < 1 || mc > 9) {
        fprintf(stderr, "MC must be 1..9\n");
        return 1;
    }
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    if (geometry_gate(&jf)) { jed_free(&jf); return 2; }
    if (target_gate(&jf, 1)) { jed_free(&jf); return 2; }
    if (!device_rows_supported(&jf)) { jed_free(&jf); return 2; }
    if (fb > profile_for(&jf)->nfb) {
        fprintf(stderr, "%s has only %d function blocks\n",
                jf.device, profile_for(&jf)->nfb);
        jed_free(&jf); return 1;
    }
    printf("%s  FB%d_MC%d -- product-term rows\n\n", path, fb, mc);
    /* was hardcoded `row < 5` -- silently never showed a 6th
     * private row on an exporting MC1 on XC7372, the
     * SAME row-count model `mc_in_use`/`rowN_state` share via
     * `mc_priv_rows_x`. Empirically confirmed harmless on the entire
     * real 1497-file corpus (18 XC7372 designs, every FB1-8/MC1: the
     * notional 6th row is never active) -- fixed anyway, matching this
     * class's own precedent: a row-count model
     * that changes in one place and not its readers is a standing risk,
     * not a proven-safe one, until every reader shares it). */
    for (row = 0; row < mc_priv_rows_x(&jf, fb, mc); row++) {
        char lits[512];
        int r = row_decode(&jf, fb, mc, row, lits, sizeof(lits));
        if (r < 0)      printf("  row%d  DISABLED\n", row);
        else if (r == 0) printf("  row%d  unused\n", row);
        else             printf("  row%d  %s\n", row, lits);
    }
    /* A macrocell can hold MORE than its 5 private rows by
     * CLAIMING a slot in the block's 12-entry SHARED product-term pool
     * -- and, measured on two real test designs
     * (`W5004` FB5_MC2, `W5012` FB4_MC9), the fitter can reach for a shared
     * slot even when the equation has FIVE terms or fewer, not only past
     * private capacity. `decode.c`'s own emission (`rowN_state`/`rowN_slot`,
     * r >= `mc_priv_rows_x`) already reads these; this command never showed
     * them at all, so any claimed shared term looked like a term the
     * decoder had silently dropped, when it was never in the private window
     * to begin with. Same functions decode.c calls (`shared_pt_claimed`
     * etc.) -- not a new address (rule 4: one address, one place). */
    {
        int i, any = 0, n = shared_pt_count(&jf);
        for (i = 0; i < n; i++) {
            RowState st;
            char lits[512];
            size_t used;
            int k;
            if (!shared_pt_claimed(&jf, fb, mc, i)) continue;
            if (!any) {
                printf("\n  shared product terms claimed from this block's "
                       "12-slot pool:\n");
                any = 1;
            }
            st = shared_pt_state(&jf, fb, i);
            if (st == ROW_DISABLED) { printf("  shared%d  DISABLED\n", i); continue; }
            if (st == ROW_UNUSED)   { printf("  shared%d  unused\n", i); continue; }
            lits[0] = 0;
            used = 0;
            for (k = 0; k < 24; k++) {
                int neg;
                if (!shared_pt_slot_used(&jf, fb, i, k, &neg)) continue;
                used += (size_t)snprintf(lits + used, sizeof(lits) - used,
                         "%s%sslot%d", used ? " * " : "", neg ? "/" : "", k);
                if (used >= sizeof(lits) - 32) break;
            }
            printf("  shared%d  %s\n", i, lits);
        }
    }
    printf("\nSlots are indices, not signal names: the slot index is a\n"
           "per-design allocation over the signals actually used.\n"
           "Slot k corresponds to UIM line k -- run `uim` on the same file to\n"
           "resolve it to a source. BOTH source bands are mapped on\n"
           "XC7336/XC7318; XC7354 sources are NOT.\n");
    jed_free(&jf);
    return 0;
}

static int cmd_colat(const char *path, int offset, long nblocks) {
    JedFile jf;
    long blk;
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    printf("%-28s offset %3d : ", path, offset);
    for (blk = 0; blk < nblocks; blk++) {
        size_t a = (size_t)(blk * 128 + offset);
        if (blk && blk % 8 == 0) printf(" ");
        printf("%d", a < jf.count ? jf.fuses[a] : 0);
    }
    printf("\n");
    jed_free(&jf);
    return 0;
}

static int cmd_col(const char *path, int fb, int mc, long nblocks) {
    JedFile jf;
    long blk;
    int gb = group_base(mc), fbit = fb_bit(fb);

    if (gb < 0 || fbit < 0) {
        fprintf(stderr, "FB must be 1..4 and MC must be 1..9\n");
        return 1;
    }
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    printf("%s  FB%d_MC%d\n", path, fb, mc);
    printf("address = block*128 + %d + %d\n\n", gb, fbit);
    if (mc == 1)
        printf("NOTE: MC1 addressing is used here; the block-0 IN-USE decode\n"
               "at group 88 remains unsolved.\n\n");

    for (blk = 0; blk < nblocks; blk++) {
        size_t a = (size_t)(blk * 128 + gb + fbit);
        if (blk % 64 == 0) printf("\nblk %4ld  ", blk);
        else if (blk % 8 == 0) printf(" ");
        if (a >= jf.count) { printf("."); continue; }
        printf("%d", jf.fuses[a]);
    }
    printf("\n");
    jed_free(&jf);
    return 0;
}

static int cmd_used(const char *path) {
    JedFile jf;
    int fb, mc, n = 0;
    if (jed_load(path, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    if (!device_rows_supported(&jf)) { jed_free(&jf); return 2; }
    printf("macrocells reported IN USE\n\n");
    for (mc = 1; mc <= 9; mc++)
        for (fb = 1; fb <= profile_for(&jf)->nfb; fb++) {
            int row, any = 0;
            /* was hardcoded `row < 5` -- see cmd_rows's own
             * comment above; same fix, same reason, same shared
             * `mc_priv_rows_x`. */
            for (row = 0; row < mc_priv_rows_x(&jf, fb, mc) && !any; row++) {
                char lits[512];
                if (row_decode(&jf, fb, mc, row, lits, sizeof(lits)) == 1) any = 1;
            }
            if (any) { printf("   FB%d_MC%d\n", fb, mc); n++; }
        }
    printf("\n%d macrocell(s), MC1..MC9, all four function blocks.\n", n);

    /* AN INDEPENDENT WITNESS. blk23 read at MC8 is a per-FUNCTION-
     * BLOCK "has logic placed in it" flag -- 724 of 724 against the fitter's
     * own .EQN, no error in either direction. It is decoded from the
     * CONFIGURATION word and owes nothing to the row addressing, so it can
     * contradict the row decode, and when it does the row decode is the thing
     * to doubt.
     *
     * This is the check that would have caught the row-base defect in one command: with
     * the XC7354 row base wrong, single-term macrocells decoded as unused
     * while this flag still said their block held logic. */
    {
        const DevProfile *p = profile_for(&jf);
        int disagree = 0;
        if (p->cfg_fbinuse >= 0) {
            printf("\nCross-check against blk%d at MC8, the per-function-block\n"
                   "flag. It is decoded from the CONFIGURATION word and\n"
                   "shares no addressing with the rows:\n", p->cfg_fbinuse);
            for (fb = 1; fb <= p->nfb; fb++) {
                int rows_say = 0, flag;
                for (mc = 1; mc <= 9 && !rows_say; mc++) {
                    int row;
                    /* was hardcoded `row < 5`, same fix as above. */
                    for (row = 0; row < mc_priv_rows_x(&jf, fb, mc) && !rows_say; row++) {
                        char lits[512];
                        if (row_decode(&jf, fb, mc, row, lits, sizeof lits) == 1)
                            rows_say = 1;
                    }
                }
                flag = cfg_bit(&jf, fb, 8, p->cfg_fbinuse);
                if (flag < 0) continue;
                if (rows_say == (flag == 0))
                    printf("   FB%d  rows say %-6s   flag says %-6s   agree\n",
                           fb, rows_say ? "USED" : "unused",
                           flag ? "unused" : "USED");
                else {
                    printf("   FB%d  rows say %-6s   flag says %-6s   *** DISAGREE ***\n",
                           fb, rows_say ? "USED" : "unused",
                           flag ? "unused" : "USED");
                    disagree++;
                }
            }
            if (disagree)
                printf("\n%d function block(s) DISAGREE. The row decode is the\n"
                       "thing to doubt: the flag is one bit and the rows are a\n"
                       "whole addressing model.\n", disagree);
        }
    }

    printf("Rule: a macrocell is IN USE iff at least one of its five\n"
           "product-term rows is ACTIVE -- neither all-ones (DISABLED) nor\n"
           "all-zero. Read through the OWNING RECORD. Validated\n"
           "against the fitter's own .RES on 36 designs, 0 mismatch,\n"
           "including MC1, which no earlier rule could decode.\n");
    jed_free(&jf);
    return 0;
}

static int cmd_devices(void) {
    int i;
    printf("%-10s %4s %5s %8s   %s\n",
           "device", "FBs", "MCs", "QF", "source of QF");
    for (i = 0; i < device_count(); i++) {
        const device_t *d = device_nth(i);
        if (d->qf)
            printf("%-10s %4d %5d %8ld   %s\n", d->name, d->n_fbs, d->n_mcs,
                   d->qf, d->qf_measured ? "MEASURED from a real bitmap"
                                         : "unverified");
        else
            printf("%-10s %4d %5d %8s   NOT KNOWN -- no accepted -p spelling\n",
                   d->name, d->n_fbs, d->n_mcs, "-");
    }
    return 0;
}


/* pin count by package name. */
static int npins_of(const char *pkg) {
    /* this only ever recognised 44/68/84, so
     * `pin_for_macrocell`'s search loop (`for p = 1..npins_of(pkg)`)
     * silently capped at 44 for EVERY OTHER PACKAGE THIS TOOL CLAIMS TO
     * SUPPORT -- PQ100 (101 pins), PQ160 (161 pins) -- meaning almost
     * every macrocell's real pin (mostly numbered above 44) was NEVER
     * CHECKED and looked unbonded. On XC73108-PQ160 this buried EVERY
     * signal outside the two FastCLK pads as an internal
     * node instead of an OUTPUTPIN -- confirmed directly: a fully
     * packed, `.MAP`-checked PQ160 design decoded with zero OUTPUTPINs
     * at all. Package pin counts now come from the package's OWN
     * numeric suffix, which every spelling used in this codebase
     * already carries (PC44, PC68, PC84, PQ100, PQ160, VQ44, PG144) --
     * one address, one place, instead of a second hand-copied table
     * that can disagree with the vendor tables in `device.c`. */
    const char *p;
    int n = 0;
    if (!pkg) return 44;
    for (p = pkg; *p; p++)
        if (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); }
        else if (n) break;              /* stop after the first run of digits */
    return n ? n : 44;
}

static int cmd_pins(const char *devname, const char *pkg) {
    const device_t *d = device_find(devname);
    int pin, any = 0;
    if (!d) { fprintf(stderr, "unknown device %s\n", devname); return 1; }
    printf("%s %s\n\n", d->name, pkg);
    printf("%4s  %-12s %s\n", "pin", "macrocell", "function");
    /* this said `pin <= 44`. PC68 has SIXTY-EIGHT pins and PC84 has
     * eighty-four, so every pad above 44 was invisible -- which is the whole
     * of the "17 of the 49 known pins do not resolve". The bound
     * comes from the PACKAGE now. */
    for (pin = 1; pin <= npins_of(pkg); pin++) {
        int fb = 0, mc = 0;
        const char *fn = device_pin_function(d, pkg, pin);
        if (device_pin_macrocell(d, pkg, pin, &fb, &mc)) {
            printf("%4d  FB%d_MC%-8d %s\n", pin, fb, mc, fn ? fn : "");
            any = 1;
        } else if (fn) {
            printf("%4d  %-12s %s\n", pin, "-", fn);
            any = 1;
        }
    }
    if (!any)
        printf("no pin table for this device/package yet\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * `selftest` -- the POSITIVE CONTROL the C never had.
 *
 * Every Python checker in this project carries a --selftest that corrupts an
 * input and REQUIRES the check to fail, because a checker that finds nothing
 * looks exactly like a clean result. The C tool had no such thing, and two
 * defects lived in it for a whole revision each precisely because no code
 * path exercised them:
 *
 * prof_7354.row_g_fast = 1 put row4 at group 5 of a 40-BIT
 *             record -- five groups -- so it read the default 1 forever.
 * cfg_fbbit carried the ROW map into the CONFIG word and was
 *             wrong on both XC7354 FAST blocks, with NOTHING reading it.
 *
 * Both are STRUCTURAL and neither needed a bitmap to catch. This checks the
 * invariants directly, and `--fail` corrupts a profile so the control can be
 * seen to fire.
 * ------------------------------------------------------------------------ */
static int st_fail;
static void st(int ok, const char *what) {
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) st_fail++;
}

static int cmd_selftest(int force_fail) {
    const DevProfile *profs[2];
    char buf[160];
    int d, fb, mc, row, g, bit, seen[9];
    DevProfile broken;

    profs[0] = &prof_7336;
    profs[1] = &prof_7354;
    if (force_fail) {
        /* POSITIVE CONTROL: reintroduce the defect and require the
         * structural check to catch it. */
        broken = prof_7354;
        broken.row_g_fast = 1;
        profs[1] = &broken;
        printf("--fail: prof_7354.row_g_fast forced back to 1 (a known "
               "wrong value)\n\n");
    }
    printf("xc7300dump selftest -- structural invariants of the device "
           "profiles\n\n");

    for (d = 0; d < 2; d++) {
        const DevProfile *p = profs[d];
        printf("%s\n", p->name);

        /* 1. FIVE product-term rows must fit inside a 40-bit record, which is
         * exactly five 8-fuse groups. This is the invariant. */
        for (fb = 1; fb <= p->nfb; fb++) {
            int g0 = (fb <= p->fast_upto) ? p->row_g_fast : p->row_g_hd;
            snprintf(buf, sizeof buf,
                     "FB%d: five rows from group %d fit a 40-bit record "
                     "(needs g0+4 <= 4)", fb, g0);
            st(g0 >= 0 && g0 + 4 <= 4, buf);
        }

        /* 2. Every macrocell has a group base, and they are DISTINCT. Nine
         *    macrocells sharing a base would silently alias two columns. */
        for (mc = 0; mc < 9; mc++) seen[mc] = 0;
        for (mc = 1; mc <= 9; mc++) {
            int gb = mc_group_base(mc), k, dup = 0;
            for (k = 1; k < mc; k++) if (mc_group_base(k) == gb) dup = 1;
            seen[mc - 1] = (gb >= 0 && gb % 8 == 0 && !dup);
        }
        { int all = 1;
          for (mc = 0; mc < 9; mc++) if (!seen[mc]) all = 0;
          st(all, "MC1..MC9 group bases are distinct and 8-fuse aligned"); }

        /* 3. The configuration word must land inside its record. offset 96
         *    holds groups 0..4 (MC9..MC5) and offset 104 groups 0..3
         *    (MC4..MC1); a group outside that is off the end. */
        { int all = 1;
          for (mc = 1; mc <= 9; mc++) {
              g = cfg_group(mc);
              if (cfg_offset(mc) == 96) { if (g < 0 || g > 4) all = 0; }
              else                      { if (g < 0 || g > 3) all = 0; }
          }
          st(all, "config group is inside its record for MC1..MC9"); }

        /* 4. THE INVARIANT. The config FB bit map must be a map, not a
         *    copy of the row map, and every entry must be a real bit 0..7.
         *    Two function blocks in the SAME range sharing a bit would make
         *    the address ambiguous -- which is what "a bit index is not an
         *    address" means in code. */
        { int used[8], ok = 1;
          for (bit = 0; bit < 8; bit++) used[bit] = 0;
          for (fb = 1; fb <= p->nfb && fb <= p->fast_upto; fb++) {
              int b = p->cfg_fbbit_fast[fb - 1];
              if (b < 0) continue;
              if (b > 7 || used[b]) ok = 0;
              used[b] = 1;
          }
          st(ok, "config FAST-range FB bits are distinct and in 0..7");
          for (bit = 0; bit < 8; bit++) used[bit] = 0;
          ok = 1;
          for (fb = p->fast_upto + 1; fb <= p->nfb; fb++) {
              int b = p->cfg_fbbit[fb - 1];
              if (b < 0) continue;
              if (b > 7 || used[b]) ok = 0;
              used[b] = 1;
          }
          st(ok, "config HD-range FB bits are distinct and in 0..7"); }

        /* 5. Row bits likewise, across ALL function blocks: the row map is
         *    one range covering every FB on the device. */
        { int used[8], ok = 1;
          for (bit = 0; bit < 8; bit++) used[bit] = 0;
          for (fb = 1; fb <= p->nfb; fb++) {
              int b = p->fbbit[fb - 1];
              if (b < 0) { ok = 0; continue; }
              if (b > 7 || used[b]) ok = 0;
              used[b] = 1;
          }
          st(ok, "row FB bits are distinct, in 0..7, and defined for every FB"); }

        /* 5b. The row bit map must INVERT. fb_of_rowbit() turns a bit
         *     back into a function block and every consumer now uses it; if
         *     the round trip fb -> bit -> fb ever failed, `uim` would print
         *     the wrong destination block with no other symptom. */
        { int ok = 1;
          for (fb = 1; fb <= p->nfb; fb++) {
              int b = p->fbbit[fb - 1];
              if (b < 0 || fb_of_rowbit(p, b) != fb) ok = 0;
          }
          st(ok, "the row FB bit map inverts: fb -> bit -> fb for every FB"); }

        /* 5c. The UIM block extent, where stated, must be a sane range the
         *     slot rule can address. i = 2*(blk-64)+... goes NEGATIVE below
         *     block 64, and a negative slot is not a slot. */
        if (p->uim_blk_lo >= 0) {
            st(p->uim_blk_hi >= p->uim_blk_lo,
               "the UIM block extent is a non-empty range");
            st(p->uim_blk_lo >= 64,
               "the UIM scan starts at or above block 64, so no slot is "
               "negative");
        }

        /* 6. A located attribute must be a real block number. -1 means NOT
         *    MEASURED and is fine; anything else must be addressable. */
        { int ok = 1, k;
          const int *tab = p->cfg_fast;
          for (k = 0; k < 8; k++) if (tab[k] < -1) ok = 0;
          tab = p->cfg_hd;
          for (k = 0; k < 8; k++) if (tab[k] < -1) ok = 0;
          st(ok, "attribute block numbers are >= -1"); }

        /* 7. The raw config window, when stated, must CONTAIN every named
         *    attribute -- otherwise the tool prints an epilogue about blocks
         *    it did not show. */
        if (p->cfg_raw_lo >= 0) {
            int ok = 1, k;
            for (k = 0; k < 8; k++) {
                int b = p->cfg_fast[k];
                if (b >= 0 && (b < p->cfg_raw_lo || b > p->cfg_raw_hi)) ok = 0;
            }
            if (p->cfg_alloc >= 0 &&
                (p->cfg_alloc < p->cfg_raw_lo || p->cfg_alloc > p->cfg_raw_hi))
                ok = 0;
            if (p->cfg_polarity >= 0 &&
                (p->cfg_polarity < p->cfg_raw_lo ||
                 p->cfg_polarity > p->cfg_raw_hi)) ok = 0;
            st(ok, "the raw config window contains every named attribute");
        }
        printf("\n");
    }

    /* 8. row_fuse must never address a group outside the five-group record.
     *    Checked over every (device, fb, mc, row) combination there is. */
    { int ok = 1;
      for (d = 0; d < 2; d++) {
          const DevProfile *p = profs[d];
          for (fb = 1; fb <= p->nfb; fb++) {
              int g0 = (fb <= p->fast_upto) ? p->row_g_fast : p->row_g_hd;
              for (mc = 1; mc <= 9; mc++)
                  for (row = 0; row < 5; row++) {
                      if (p->split_mc1 && mc == 1) continue;  /* 8+32 records */
                      if (g0 + row > 4) ok = 0;
                  }
          }
      }
      st(ok, "row_fuse never addresses a group past the end of its record"); }

    /* 9. `decode` emits over function blocks 1..4 unconditionally.
     *    It REFUSES every device but XC7336/XC7318, and both of those use
     *    prof_7336 -- so the assumption holds today and is checked here so
     *    that enabling a wider device cannot silently truncate the emitter. */
    st(prof_7336.nfb == 4,
       "decode's hardcoded 1..4 function-block loop matches the only profile "
       "it accepts");

    printf("\n%s\n", st_fail ? "*** SELFTEST FAILED ***"
                              : "all structural invariants hold");
    /* SCOPE, stated plainly. This catches STRUCTURAL errors -- an address
     * that cannot exist -- and nothing else. It would have caught the config-map defect in
     * one second without a bitmap, and it would NOT have caught,
     * because carrying the ROW map into the CONFIG word produces bits that
     * are perfectly well-formed and simply WRONG. No self-consistency check
     * can find a wrong value; only a measurement against the fitter's own
     * reports can. That is what the Python checkers are for. */
    printf("\nSCOPE: this checks only that an address CAN exist -- that five\n"
           "rows fit a five-group record, that bit maps are distinct and in\n"
           "range, that the raw window covers what the epilogue discusses.\n"
           "It CANNOT tell a well-formed WRONG value from a right one: the\n"
           "historical defect put the ROW bit map into the CONFIG word and every\n"
           "invariant here still holds. Values are checked against the\n"
           "FITTER'S OWN reports (.EQN/.MAP) and by round cycle, never here.\n");
    return st_fail ? 1 : 0;
}


/* ---------------------------------------------------------------------------
 * `readback` -- turn a PROGRAMMER DUMP of a real part into a JEDEC.
 *
 * A readback is an Intel HEX byte buffer, not a JEDEC file, and the
 * device layout is:
 *
 *     jed_owned(block, rec_off, group, bit) == chip byte
 *         block*128 + rec_off + group,  bit `bit`
 *
 * The emitted record layout is CANONICAL, not a copy of MAKEJED's: MAKEJED
 * emits a different set per design (seven distinct per-block layouts in one
 * bitmap), so there is nothing fixed to copy. What is fixed is where the
 * STRUCTURES live, and this writes exactly those:
 *
 *     blocks  0..47   24,32,40,48,56,64,72,80 (40b)  88 (8b)  90 (32b)
 *                     96 (40b)  104 (32b)
 *     blocks 64..125  32,40,48,96,104,112 (64b)      the UIM, 8 groups
 *
 * NOTHING is written for blocks 48..63, and MAKEJED writes nothing there
 * either. Emitting records there is what produced NEGATIVE slots and the
 * fabricated name "FB0_MC1", because the slot rule is
 * i = 2*(block-64).
 * ------------------------------------------------------------------------ */
static int ihex_load(const char *path, unsigned char **out, size_t *outn) {
    FILE *f = fopen(path, "rb");
    char line[600];
    unsigned char *buf = NULL;
    size_t cap = 0, hi = 0;
    long base = 0;
    int bad = 0, seen = 0;
    if (!f) { perror(path); return -1; }
    while (fgets(line, sizeof line, f)) {
        unsigned v[300];
        int n, i, sum = 0, len, type;
        long addr;
        char *p = line;
        while (*p && *p != ':') p++;
        if (*p != ':') continue;
        p++;
        for (n = 0; p[0] && p[1] && isxdigit((unsigned char)p[0])
                     && isxdigit((unsigned char)p[1]) && n < 300; n++, p += 2) {
            char h[3]; h[0] = p[0]; h[1] = p[1]; h[2] = 0;
            v[n] = (unsigned)strtol(h, NULL, 16);
        }
        if (n < 5) continue;
        len = (int)v[0]; type = (int)v[3];
        if (n != len + 5) { bad++; continue; }
        for (i = 0; i < len + 4; i++) sum += (int)v[i];
        if (((-sum) & 0xFF) != (int)v[len + 4]) { bad++; continue; }
        if (type == 0) {
            addr = base + (long)((v[1] << 8) | v[2]);
            for (i = 0; i < len; i++) {
                size_t a = (size_t)(addr + i);
                if (a + 1 > cap) {
                    size_t nc = (a + 1) * 2;
                    unsigned char *nb = realloc(buf, nc);
                    if (!nb) { free(buf); fclose(f); return -1; }
                    memset(nb + cap, 0, nc - cap);
                    buf = nb; cap = nc;
                }
                buf[a] = (unsigned char)v[4 + i];
                if (a + 1 > hi) hi = a + 1;
                seen++;
            }
        } else if (type == 4) base = (long)((v[4] << 8) | v[5]) << 16;
        else if (type == 2)  base = (long)((v[4] << 8) | v[5]) << 4;
    }
    fclose(f);
    if (bad) {
        fprintf(stderr, "REFUSING: %d Intel HEX record(s) failed their "
                        "checksum. A readback with a bad record is not a "
                        "readback, it is a bad file.\n", bad);
        free(buf); return -1;
    }
    if (!seen) {
        fprintf(stderr, "REFUSING: no Intel HEX data records in %s\n", path);
        free(buf); return -1;
    }
    *out = buf; *outn = hi;
    return 0;
}

/* THE TEMPLATE IS NOT A CONVENIENCE, IT IS THE ONLY HONEST OPTION.
 *
 * My first version invented a "canonical" record layout -- every structure at
 * every block where one could live. On the fitter's own bitmaps that is
 * harmless. On a real chip it is not:
 *
 *     UIM region, blocks 64..125, offsets 32/40/48/96/104/112, bits 0..3
 *       REAL CHIP           2445 of 11904 set   20.5%
 *       gt_p1_xc7336           4 of  6720 set    0.06%
 *       gt_s20_rich           26 of  6720 set    0.39%
 *       gt_s39_rt_rich        26 of  6720 set    0.39%
 *
 * Fifty to three hundred times denser. A crossbar with a fifth of its
 * crosspoints closed would tie every source together, so MOST OF THOSE BITS
 * ARE NOT CROSSPOINTS -- the UIM region holds structures this project has not
 * identified, interleaved with the ones it has.
 *
 * The fitter's record layout therefore carries knowledge we do not have
 * independently: WHICH addresses are which structure. Inventing a layout
 * throws that away and makes the tool read unknown fields as UIM connections.
 * So a template is REQUIRED, and any MAKEJED bitmap for the same device will
 * do. */
static int layout_from(const char *tmpl, long **addrs, int **nbits, int *n) {
    FILE *f = fopen(tmpl, "rb");
    char line[8192];
    long *A = NULL; int *B = NULL, cap = 0, k = 0;
    if (!f) { perror(tmpl); return -1; }
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        long a; int cnt = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != 'L') continue;
        a = strtol(p + 1, &p, 10);
        while (*p && *p != '*') { if (*p == '0' || *p == '1') cnt++; p++; }
        if (!cnt) continue;
        if (k == cap) {
            cap = cap ? cap * 2 : 256;
            A = realloc(A, (size_t)cap * sizeof *A);
            B = realloc(B, (size_t)cap * sizeof *B);
            if (!A || !B) { fclose(f); free(A); free(B); return -1; }
        }
        A[k] = a; B[k] = cnt; k++;
    }
    fclose(f);
    if (!k) { free(A); free(B); return -1; }
    *addrs = A; *nbits = B; *n = k;
    return 0;
}

static int cmd_readback(const char *prg, const char *outjed, const char *tmpl) {
    unsigned char *buf = NULL;
    size_t n = 0, i, covered = 0, nz = 0, inside = 0;
    unsigned char *mark;
    FILE *o;
    int k, distinct[256], nd = 0, nrec = 0;
    long *addrs = NULL; int *nbits = NULL;

    if (ihex_load(prg, &buf, &n) != 0) return 1;
    {   /* A byte count matching no known
         * device's QF is not a readback of anything this tool understands
         * -- refuse rather than let `layout_from`'s template silently
         * dictate a device the image itself never claimed to be. Same
         * bits==qf || bits==qf-8 rule the `info` banner already prints:
         * A programmer image is QF/8 bytes, plus one). */
        long bits = (long)n * 8, dn; int match = 0;
        const device_t *d;
        for (dn = 0; (d = device_nth((int)dn)) != NULL; dn++)
            if (d->qf && (d->qf == bits || d->qf == bits - 8)) { match = 1; break; }
        if (!match) {
            fprintf(stderr,
                "\nREFUSING: %lu bytes (%ld bits) matches NO known device's QF.\n"
                "This is not a readback of any XC7300 part this tool has a fuse\n"
                "count for -- decoding it against an unrelated template would be\n"
                "a guess.\n", (unsigned long)n, bits);
            free(buf); return 2;
        }
    }
    memset(distinct, 0, sizeof distinct);
    for (i = 0; i < n; i++) { if (!distinct[buf[i]]) { distinct[buf[i]] = 1; nd++; } }
    printf("%s\n  %lu bytes, 0..%lu, every checksum OK\n",
           prg, (unsigned long)n, (unsigned long)(n ? n - 1 : 0));
    printf("  %d distinct byte values\n", nd);
    if (nd <= 2) {
        fprintf(stderr,
            "\nREFUSING: %d distinct byte values. A BLANK or SECURITY-LOCKED\n"
            "part reads back uniform and will look exactly like a decoder\n"
            "failure -- so this is reported, not decoded.\n", nd);
        free(buf); return 2;
    }
    if (layout_from(tmpl, &addrs, &nbits, &nrec) != 0) {
        fprintf(stderr,
            "\nREFUSING: cannot read a record layout from '%s'.\n"
            "`readback` needs a TEMPLATE -- any MAKEJED bitmap for this\n"
            "device. The fitter's record set says WHICH addresses are which\n"
            "structure, and a real chip's UIM region is 50-300x denser than\n"
            "any fitter bitmap, so a made-up layout makes the tool\n"
            "read unidentified fields as UIM connections.\n", tmpl);
        free(buf); return 2;
    }
    printf("  layout   : %d records from %s\n", nrec, tmpl);
    mark = calloc(n ? n : 1, 1);
    if (!mark) { free(buf); free(addrs); free(nbits); return 1; }
    o = fopen(outjed, "wb");
    if (!o) { perror(outjed); free(buf); free(mark); return 1; }
    /* The header must NOT contain the string "XC7...". jed.c finds
     * the part name by scanning for the first token that looks like one
     * (jed.c:find_device), and putting VERSION here -- "xc7300dump v1.31 --
     * XC7300 EPLD bitmap inspector" -- made the tool read its OWN BANNER as
     * the device and then refuse the file it had just written. Caught only
     * because `decode` refused; `info` would have printed it happily. */
    fprintf(o, "Programmer Jedec Bit Map\r\n"
               "Converted from a real device readback\r\n\r\n");
    fprintf(o, "\x02\r\nXC7336-5PC44\r\nREADBACK*\r\nQF128960*\r\nF1*\r\n\r\n");
    for (k = 0; k < nrec; k++) {
        size_t a = (size_t)addrs[k];
        int b, nbit = nbits[k];
        if (a + (size_t)(nbit + 7) / 8 > n) continue;
        fprintf(o, "L%05lu ", (unsigned long)a);
        for (b = 0; b < nbit; b++) {
            size_t by = a + (size_t)(b / 8);
            fputc(((buf[by] >> (b % 8)) & 1) ? '1' : '0', o);
            if (!mark[by]) { mark[by] = 1; covered++; }
        }
        fprintf(o, "*\r\n");
    }
    fprintf(o, "\x03""0000\r\n");
    fclose(o);
    for (i = 0; i < n; i++) if (buf[i]) { nz++; if (mark[i]) inside++; }
    printf("  wrote %s\n\n", outjed);
    printf("  device bytes NON-ZERO             : %lu\n", (unsigned long)nz);
    printf("    inside the structures written   : %lu\n", (unsigned long)inside);
    printf("    OUTSIDE them                    : %lu\n",
           (unsigned long)(nz - inside));
    printf("\n  Those outside bytes are REAL DEVICE DATA at addresses no\n"
           "  structure is mapped to. They are NOT in the .jed, because every\n"
           "  reader in this tool was measured against the fitter's own record\n"
           "  set and nothing is known about the rest. Do NOT assume\n"
           "  they are unused.\n");
    printf("\n  A decode of a readback has NOT been round-cycled. Check\n"
           "  `xc7300dump uim %s` before trusting any equation.\n", outjed);
    free(buf); free(mark); free(addrs); free(nbits);
    return 0;
}

int main(int argc, char **argv) {
    /* The banner prints on EVERY invocation, not only the usage
     * screen. A decode that scrolls past without it gives no clue which
     * build produced the output, and the disclaimer belongs where the work
     * happens rather than where the help does. stderr, so redirected stdout
     * stays clean for piping. */
    fprintf(stderr, "%s\n", VERSION);
    {   /* `--package PKG`, accepted anywhere, removed before dispatch */
        int i, j;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--package") == 0 && i + 1 < argc) {
                g_package = argv[i+1];
                for (j = i; j + 2 <= argc - 1; j++) argv[j] = argv[j+2];
                argc -= 2; i--;
            }
        }
    }
    if (argc < 2) { usage(); return 1; }
    /* recognise the file's FORMAT before anything else, and say so
     * in one line. A programmer writes Intel hex (`.prg`); most commands
     * decode JEDEC. Silence, or a bare "cannot read", left the user
     * guessing.: this block used to run for
     * EVERY command whose argv[2] is Intel hex, including `readback`,
     * whose whole job is to READ an Intel-hex file -- the block always
     * printed its banner and returned 0 before `cmd_readback` was ever
     * reached, making `readback` permanently dead code (confirmed: no
     * output file was ever written, on any input, by any invocation).
     * `readback` now handles its own Intel-hex argument and is excluded
     * here. */
    if (argc >= 3 && strcmp(argv[1], "selftest") != 0 &&
        strcmp(argv[1], "version") != 0 &&
        strcmp(argv[1], "readback") != 0) {
        int fmt = file_format(argv[2]);
        if (fmt == FMT_IHEX) {
            IhexInfo ii;
            printf("Intel Hex recognized\n");
            if (ihex_probe(argv[2], &ii) == 0) {
                printf("  %lu data records, %lu bytes, 0x%04lX..0x%04lX",
                       (unsigned long)ii.records, (unsigned long)ii.bytes,
                       ii.lo, ii.hi);
                if (ii.bad_checksums)
                    printf("  (%lu BAD CHECKSUM%s)",
                           (unsigned long)ii.bad_checksums,
                           ii.bad_checksums == 1 ? "" : "S");
                printf("\n");
            }
            {   /* The image size IDENTIFIES the device. A
                 * programmer writes exactly QF/8 bytes, so the byte count is
                 * the fuse count divided by eight -- 16120 bytes is QF
                 * 128960, which is XC7336 and XC7318. */
                long bits = (long)ii.bytes * 8, n; int nm = 0;
                const device_t *d;
                printf("\n  image is %ld bytes = %ld fuses", (long)ii.bytes, bits);
                for (n = 0; (d = device_nth((int)n)) != NULL; n++)
                    if (d->qf == bits || d->qf == bits - 8) {
                        printf("%s%s", nm++ ? ", " : "  ->  ", d->name);
                    }
                printf("\n");
            }
            printf("\n  An earlier sweep of every linear byte->bit packing\n"
                   "  (lsb/msb, direct/inverted) over the WHOLE image against a\n"
                   "  real chip's JEDEC and found ~40%% or ~60%% agreement --\n"
                   "  called this noise and the image an unrecoverable permutation.\n"
                   "  That sweep compared the\n"
                   "  wrong denominator: ~75%% of a programmer image is offsets\n"
                   "  the FITTER never writes (no ground truth exists there at\n"
                   "  all), which drowns any real signal in the ~25%% that IS\n"
                   "  fitter-covered. RESTRICTED to that covered region, the\n"
                   "  direct mapping (byte address == JEDEC L-record address,\n"
                   "  bit b of byte N == JEDEC fuse 8*N+b, LSB-first, no\n"
                   "  inversion) round-cycled CLEAN on 2 of 2 real chips\n"
                   "  available in both formats (decode -> real fitter\n"
                   "  refit -> decode -> 400 traces x 6 cycles, 0 differing\n"
                   "  output pins, both). Use `readback` to convert -- it needs\n"
                   "  a TEMPLATE (any MAKEJED bitmap for the device) and writes\n"
                   "  ONLY the addresses the template's own record set covers;\n"
                   "  what the other ~75%% of this image holds is still UNKNOWN\n"
                   "  and `readback` reports how much it left out, every time.\n"
                   "  Your programmer can also save the same part as JEDEC\n"
                   "  directly -- that file decodes fully, no conversion needed.\n");
            return 0;
        }
        if (fmt == FMT_JEDEC) fprintf(stderr, "JEDEC recognized\n");
    }

    if (strcmp(argv[1], "info") == 0 && argc == 3)
        return cmd_info(argv[2]);

    if (strcmp(argv[1], "namebits") == 0 && argc == 4)
        return cmd_namebits(argv[2], argv[3]);

    if (strcmp(argv[1], "profiledump") == 0 && argc == 3)
        return cmd_profiledump(argv[2]);

    if (strcmp(argv[1], "structuremap") == 0 && argc == 3)
        return cmd_structuremap(argv[2]);

    if (strcmp(argv[1], "vmhroutes") == 0 && argc == 3) {
        if (strcmp(argv[2], "--selftest") == 0)
            return cmd_vmhroutes_selftest();
        return cmd_vmhroutes(argv[2]);
    }

    if (strcmp(argv[1], "rdiff") == 0 && argc >= 4) {
        long mx = 40;
        if (argc >= 6 && strcmp(argv[4], "--max") == 0)
            mx = strtol(argv[5], NULL, 10);
        return cmd_diff_recs(argv[2], argv[3], mx);
    }

    if (strcmp(argv[1], "diff") == 0 && argc >= 4) {
        long maxshow = 40;
        int i;
        for (i = 4; i < argc - 1; i++)
            if (strcmp(argv[i], "--max") == 0) maxshow = strtol(argv[i+1], NULL, 10);
        return cmd_diff(argv[2], argv[3], maxshow);
    }

    if (strcmp(argv[1], "dump") == 0 && argc == 5)
        return cmd_dump(argv[2], strtol(argv[3], NULL, 0),
                        strtol(argv[4], NULL, 0));

    /* canonical FULL decode -- everything the tool can say about a
     * bitmap, in one invocation, so a mutation test can ask "does flipping
     * this fuse change what the tool understands?" */
    if (strcmp(argv[1], "all") == 0 && argc == 3) {
        int fb, mc, nfb = 4;
        JedFile peek;
        /* this loop's own upper bound was
         * hardcoded to 4 -- a diagnostic-lag defect, the same class
         * as one found earlier in cmd_config's toggle field. `cmd_rows`
         * and `cmd_config` have each been generalized to any device's real
         * `p->nfb` (up to 12, `decode_nfb`'s own cap) for a long time; this
         * summary command silently never asked them past FB4, so `all` on
         * an XC7354/XC7372/XC73108 design printed nothing for the majority
         * of its function blocks while claiming to be the "canonical FULL
         * decode -- everything the tool can say about a bitmap". Peek the
         * device's own profile the same way `cmd_config` does
         * (`profile_for`), falling back to the historical 4 only if the
         * file cannot even be loaded (`cmd_rows`/`cmd_config` will then
         * each print their own refusal, unchanged from before this fix). */
        if (jed_load(argv[2], &peek) == 0) {
            const DevProfile *pp = profile_for(&peek);
            if (pp && pp->nfb > 0 && pp->nfb <= 12) nfb = pp->nfb;
            jed_free(&peek);
        }
        cmd_used(argv[2]);
        cmd_signature(argv[2]);
        cmd_uim(argv[2]);
        for (fb = 1; fb <= nfb; fb++)
            for (mc = 1; mc <= 9; mc++) {
                cmd_rows(argv[2], fb, mc);
                cmd_config(argv[2], fb, mc);
            }
        return 0;
    }

    if (strcmp(argv[1], "decode") == 0 && argc >= 3)
        return decode_emit(argv[2], argc >= 4 ? argv[3] : "RT_OUT");

    if (strcmp(argv[1], "signature") == 0 && argc == 3)
        return cmd_signature(argv[2]);

    if (strcmp(argv[1], "uim") == 0 && argc == 3)
        return cmd_uim(argv[2]);

    if (strcmp(argv[1], "config") == 0 && argc == 5)
        return cmd_config(argv[2], (int)strtol(argv[3], NULL, 10),
                          (int)strtol(argv[4], NULL, 10));

    if (strcmp(argv[1], "rows") == 0 && argc == 5)
        return cmd_rows(argv[2], (int)strtol(argv[3], NULL, 10),
                        (int)strtol(argv[4], NULL, 10));

    if (strcmp(argv[1], "colat") == 0 && (argc == 4 || argc == 5))
        return cmd_colat(argv[2], (int)strtol(argv[3], NULL, 10),
                         argc == 5 ? strtol(argv[4], NULL, 10) : 48);

    if (strcmp(argv[1], "col") == 0 && (argc == 5 || argc == 6))
        return cmd_col(argv[2], (int)strtol(argv[3], NULL, 10),
                       (int)strtol(argv[4], NULL, 10),
                       argc == 6 ? strtol(argv[5], NULL, 10) : 64);

    if (strcmp(argv[1], "used") == 0 && argc == 3)
        return cmd_used(argv[2]);

    if (strcmp(argv[1], "readback") == 0 && argc == 5)
        return cmd_readback(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "padinv") == 0 && argc == 4) {
        JedFile jf; int pin = atoi(argv[3]);
        if (jed_load(argv[2], &jf) != 0) return 1;
        printf("%d\n", x108_pad_inverted(&jf, pin));
        return 0;
    }
    if (strcmp(argv[1], "x108uim") == 0 && argc == 5) {
        JedFile jf; int sfb = atoi(argv[3]), smc = atoi(argv[4]);
        if (jed_load(argv[2], &jf) != 0) return 1;
        printf("%d\n", x108_uim_connected(&jf, profile_for(&jf), sfb, smc));
        jed_free(&jf); return 0;
    }
    /* `ball <device> <BG225 position>` */
    if (strcmp(argv[1], "ball") == 0 && argc == 4) {
        int fb, mc;
        const device_t *d = device_find(argv[2]);
        if (!d) { printf("unknown device %s\n", argv[2]); return 1; }
        if (device_ball_macrocell(d, "BG225", argv[3], &fb, &mc))
            printf("%s BG225 ball %s -> FB%d_MC%d\n", d->name, argv[3], fb, mc);
        else
            printf("%s BG225 ball %s -> not a macrocell ball\n", d->name, argv[3]);
        return 0;
    }
    if (strcmp(argv[1], "selftest") == 0)
        return cmd_selftest(argc > 2 && strcmp(argv[2], "--fail") == 0);
    if (strcmp(argv[1], "devices") == 0)
        return cmd_devices();

    if (strcmp(argv[1], "pins") == 0 && argc == 4)
        return cmd_pins(argv[2], argv[3]);

    usage();
    return 1;
}


/* ---- API for the decoder (decode.c). Each of these calls the SAME address
   helper the interactive commands use; nothing here re-derives a fuse
   address (methodology rule 4). ---- */


/* ----: SHARED PRODUCT TERMS (High-Density blocks only) ----
 *
 * A High-Density macrocell has FIVE PRIVATE ROWS. Every term beyond the fifth
 * takes one of the block's TWELVE SHARED product terms -- the pool
 * located and measurement showed from `.MAP`'s `Shared pterms - n/12`.
 *
 * Measured on a 17-point term sweep at FB3_5, one literal per term, placement
 * confirmed in `.MAP` before any fuse was read. The law is exact and linear:
 * shared = max(0, terms - 5), saturating at 12/12 for 17 terms, agreeing with
 * the fitter at all seventeen points.
 *
 * WHICH macrocell owns a shared term is recorded at THE MACROCELL'S OWN
 * CONFIG ADDRESS -- `cfg_offset(mc)`, `cfg_group(mc)`, and the config FB bit
 * -- across twelve attribute blocks. Each claim block corresponds
 * to one shared ROW, at an AND-array offset OUTSIDE the five-row group-base
 * set, which is why nothing had ever named these:
 *
 *     claim blk  25 26 27 28 29 | 30 31 32 | 37 38 | 39 40
 *     offset      0  0  0  0  0 |  8  8  8 | 16 16 |  8  8
 *     group       0  1  2  3  4 |  0  1  2 |  0  1 |  3  4
 *
 * The pairing was derived from INCREMENTAL DELTAS: each of the twelve steps
 * of the sweep adds exactly one claim block and exactly one shared row, so
 * the correspondence is read off directly rather than fitted.
 *
 * CONTROLS. A design of five terms or fewer puts NOTHING in the region
 * (1->4 terms and 1->5 terms both show zero bits there) -- the negative class,
 * without which these twelve positions would only be where bits happen to
 * appear. And on TWO-CLAIMANT designs, where two macrocells in one block
 * compete for the same pool, the claim sets are DISJOINT, their union equals
 * the fitter's `n/12` for the block, and the rows predicted by the mapping
 * above equal the rows observed -- checked on six splits from 6+6 up to 6+11.
 *
 * TERM ORDER IS NOT NEEDED AND WAS NOT MEASURED: the terms form a SUM, which
 * is commutative, so a decoder need only collect them.
 *
 * FAST blocks are excluded: `.MAP` reports `Shared pterms - 0/0` for FB1 and
 * FB2. Note that 0/0 is the fitter declining to have a pool, not a
 * Measurement that the silicon lacks one (the lesson). */

static const struct { int blk, off, grp; } shared_pt[12] = {
    { 25,  0, 0 }, { 26,  0, 1 }, { 27,  0, 2 }, { 28,  0, 3 },
    { 29,  0, 4 }, { 30,  8, 0 }, { 31,  8, 1 }, { 32,  8, 2 },
    { 37, 16, 0 }, { 38, 16, 1 }, { 39,  8, 3 }, { 40,  8, 4 },
};

int shared_pt_count(const JedFile *jf) { (void)jf; return 12; }

/* Has (fb,mc) claimed shared product term `i`? */
int shared_pt_claimed(const JedFile *jf, int fb, int mc, int i) {
    const DevProfile *p = profile_for(jf);
    /* FOURTEENTH equality guard. XC7372 never saw a shared product
     * term claimed, so every High-Density macrocell that overflows past its
     * private rows lost the excess -- `K0001`'s FB6_MC9 is five terms with a
     * product-term clock, and the fitter's own `.MAP` says `Shared pterms -
     * 1/12`. The address rule reads the PROFILE's config map, so it
     * transfers as written.
     *
     * that fix hand-wrote `jed_owned(jf, blk, cfg_offset
     * (mc), cfg_group(mc), bit, 1)` -- correct for XC7372, but it RE-
     * DERIVED an address `cfg_bit()` (main.c) already computes correctly
     * for EVERY device, `row_per_record` included (the `idx =
     * 16*group + bit` repacking, MC5's own offset/group exception). Rule
     * 4: never re-derive an address the tool already computes. On
     * XC73108 the hand-written version reads a bit that does not exist in
     * the SAME (group,bit) shape the 64-bit row_per_record config record
     * needs, so it always reads unclaimed and every HD macrocell needing
     * a shared term silently drops it -- confirmed on a single,
     * uncontested 7-term macrocell
     * (a test design, `.MAP` says
     * `Shared pterms - 2/12`, decode emitted only 5 of 7 terms), so this
     * was never a contention effect. Calling `cfg_bit()` -- the SAME
     * function `name_bit` already uses to correctly print "FB5_MC3
     * config attr blk25" for this exact fuse in `rdiff` output -- fixes
     * both devices with one call. */
    int bit;
    if (p->nfb < 6 || fb <= p->fast_upto) return 0;   /* HD blocks only */
    if (i < 0 || i >= 12) return 0;
    bit = cfg_bit(jf, fb, mc, shared_pt[i].blk);
    if (getenv("XC73_TRACE_SPTC"))
        fprintf(stderr, "SPTC fb=%d mc=%d i=%d bit=%d claimed=%d\n",
                fb, mc, i, bit, bit > 0);
    return bit > 0;
}

/* One fuse of shared row `i`. Mirrors `row_fuse`, at the shared address. */
static int shared_pt_fuse(const JedFile *jf, int fb, int i, int blk) {
    const DevProfile *p = profile_for(jf);
    int bit = p->fbbit[fb - 1];
    if (bit < 0) return 1;
    return jed_owned(jf, (size_t)blk, shared_pt[i].off, shared_pt[i].grp,
                     bit, 1);
}

RowState shared_pt_state(const JedFile *jf, int fb, int i) {
    int blk, ones = 0;
    if (i < 0 || i >= 12) return ROW_DISABLED;
    for (blk = 0; blk < 48; blk++) ones += shared_pt_fuse(jf, fb, i, blk);
    if (getenv("XC73_TRACE_SPTC"))
        fprintf(stderr, "SPTS fb=%d i=%d ones=%d\n", fb, i, ones);
    if (ones == 48) return ROW_DISABLED;
    if (ones == 0)  return ROW_UNUSED;
    return ROW_ACTIVE;
}

int shared_pt_slot_used(const JedFile *jf, int fb, int i, int slot, int *neg) {
    int e = shared_pt_fuse(jf, fb, i, 2 * slot);
    int o = shared_pt_fuse(jf, fb, i, 2 * slot + 1);
    if (!e && !o) return 0;
    *neg = e ? 1 : 0;           /* even = complement, odd = true */
    return 1;
}


/* ----: INPUT REGISTER ON A DEDICATED INPUT PAD ----
 *
 * The fitter writes `INPUTPIN ( RCLK = RC ) A B` with `FASTCLOCK RC`, and
 * reports those pads as `I-R` in `.pin` against plain `I`. Five `gt_ba_*`
 * designs use it; without it their clock pin vanishes from the rebuild and
 * The pin sets differ Class A).
 *
 * THE BIT LIVES AT THE CONFIG ADDRESS OF THE MACROCELL WHOSE BAND SLOT THE
 * PAD OCCUPIES -- it has no address of its own. Measured one pad at a time
 * with the pin frozen by `.VMF`, registered against plain, across ALL EIGHT
 * dedicated inputs of PC68. Every probe was verified in the fitter's own
 * `.pin` as `I-R` versus `I` BEFORE any fuse was read, and every pair moved
 * exactly two bits, in attribute blocks 13 and 14:
 *
 *     PIN43 slot 32 -> FB3_MC1     PIN03 slot 48 -> FB6_MC1
 *     PIN65 slot 33 -> FB3_MC2     PIN05 slot 49 -> FB6_MC2
 *     PIN67 slot 34 -> FB3_MC3     PIN11 slot 50 -> FB6_MC3
 *     PIN68 slot 35 -> FB3_MC4
 *     PIN02 slot 37 -> FB3_MC6
 *
 * That is EXACTLY the slot table read through the band rule
 * `16*B + (mc-1)`, arrived at independently from the config side. It is a
 * second, separate confirmation that the dedicated inputs really do occupy
 * those band positions -- the thing could only infer.
 *
 * THE CLOCK SELECT is a separate two-bit field, measured on three probes
 * differing only in which CLK pad `RC` is frozen to:
 *
 *     blk12 at the pad's own macrocell address : 1 for PIN08, else 0
 *     blk11 at offset 96 group 0               : 1 for PIN08 and PIN09,
 *                                                0 for PIN10
 *
 * so (1,1) = PIN08, (0,1) = PIN09, (0,0) = PIN10. THREE POINTS, ONE PAD --
 * this is the weakest evidence in this file. The blk11
 * bit sits at a DIFFERENT macrocell's address from the blk12 bit, which is
 * not understood; it may be a per-clock-line enable rather than part of a
 * per-pad field. Every design in the corpus that uses this feature uses
 * PIN08, so only that leg is exercised by a round cycle. */

static const struct { int pin, fb, mc; } xc7354_inpad_cfg[8] = {
    { 43, 3, 1 }, { 65, 3, 2 }, { 67, 3, 3 }, { 68, 3, 4 }, {  2, 3, 6 },
    {  3, 6, 1 }, {  5, 6, 2 }, { 11, 6, 3 },
};

/* Is the pad on this package pin a REGISTERED input? */
int input_pad_registered(const JedFile *jf, int pin) {
    const DevProfile *p = profile_for(jf);
    size_t k;
    if (strstr(jf->device, "XC7372")) {
        size_t k;
        for (k = 0; k < sizeof xc7372_inpad_cfg / sizeof xc7372_inpad_cfg[0]; k++) {
            int bit;
            if (xc7372_inpad_cfg[k].pin != pin) continue;
            bit = p->cfg_fbbit_fast[xc7372_inpad_cfg[k].fb - 1];
            if (bit < 0) return 0;
            return !jed_owned(jf, 14, cfg_offset(xc7372_inpad_cfg[k].mc),
                              cfg_group(xc7372_inpad_cfg[k].mc), bit, 1);
        }
        return 0;
    }
    if (p->nfb != 6) return 0;              /* measured on PC68 only */
    for (k = 0; k < 8; k++) {
        int fb, bit;
        if (xc7354_inpad_cfg[k].pin != pin) continue;
        fb  = xc7354_inpad_cfg[k].fb;
        bit = p->cfg_fbbit[fb - 1];
        if (bit < 0) return 0;
        /* this read block 13. Block 13 is the CLOCK ENABLE and
         * block 14 is the INPUT REGISTER. The probes had no CE, so 13
         * tracked 14 exactly and either would have passed:
         *     plain      13 = 1, 14 = 1
         *     RCLK       13 = 0, 14 = 0
         *     RCLK + CE  13 = 1, 14 = 0
         * A pad with RCLK AND CE reads 13 = 1 and was therefore decoded as
         * NOT REGISTERED, losing both the register and the enable. */
        return !jed_owned(jf, 14, cfg_offset(xc7354_inpad_cfg[k].mc),
                          cfg_group(xc7354_inpad_cfg[k].mc), bit, 1);
    }
    return 0;
}

/* does this registered input pad also carry a CLOCK ENABLE?
 * Block 13, at the same address as the register bit of `input_pad_registered`.
 * Measured on all eight PC68 dedicated inputs, RCLK against RCLK+CE, with the
 * enable frozen to PIN60; every pair moves exactly this one bit. The fitter
 * spells it `INPUTPIN ( RCLK = RC CE = EN )` with `CEPIN /EN` -- the enable is
 * ACTIVE LOW (`cl346`), the negation belongs on the CEPIN and NOT on the
 * option, and CE without RCLK is refused (`cl396`). */
int input_pad_ce(const JedFile *jf, int pin) {
    const DevProfile *p = profile_for(jf);
    size_t k;
    /* TWELFTH equality guard, and the same shape as
     * `input_pad_registered`: `nfb != 6` returned 0, so XC7372
     * never detected a CLOCK ENABLE and 14 designs lost PIN74 in the rebuild.
     * Block 13 at the pad's own config address, exactly as XC7354. */
    if (strstr(jf->device, "XC7372")) {
        if (!input_pad_registered(jf, pin)) return 0;
        for (k = 0; k < sizeof xc7372_inpad_cfg / sizeof xc7372_inpad_cfg[0]; k++) {
            int bit;
            if (xc7372_inpad_cfg[k].pin != pin) continue;
            bit = p->cfg_fbbit_fast[xc7372_inpad_cfg[k].fb - 1];
            if (bit < 0) return 0;
            return jed_owned(jf, 13, cfg_offset(xc7372_inpad_cfg[k].mc),
                             cfg_group(xc7372_inpad_cfg[k].mc), bit, 1);
        }
        return 0;
    }
    if (p->nfb != 6) return 0;
    if (!input_pad_registered(jf, pin)) return 0;
    for (k = 0; k < 8; k++) {
        int fb, bit;
        if (xc7354_inpad_cfg[k].pin != pin) continue;
        fb = xc7354_inpad_cfg[k].fb;
        bit = p->cfg_fbbit[fb - 1];
        if (bit < 0) return 0;
        return jed_owned(jf, 13, cfg_offset(xc7354_inpad_cfg[k].mc),
                         cfg_group(xc7354_inpad_cfg[k].mc), bit, 1);
    }
    return 0;
}

/* Which CLK pad clocks the input registers. 0 if not determinable. */
int input_pad_rclk_pin(const JedFile *jf, int pin) {
    const DevProfile *p = profile_for(jf);
    size_t k;
    /* THIRTEENTH equality guard. `nfb != 6` returned 0, so XC7372
     * fell back to a hard-coded `RC PIN12` and 8 designs whose registered
     * input pad is clocked from PIN10 rebuilt on the wrong pad.
     *
     * The ENCODING is XC7354's exactly -- blk12 at the PAD'S OWN
     * macrocell address, blk11 at a FIXED address (offset 96, group 0, bit 2)
     * -- and only the pad NUMBERS differ:
     *     XC7354  PIN08 PIN09 PIN10        XC7372  PIN09 PIN10 PIN12
     * Measured on four pads across both FAST blocks, with a second FastCLK
     * live so the fitter cannot collapse the choices. */
    if (strstr(jf->device, "XC7372")) {
        for (k = 0; k < sizeof xc7372_inpad_cfg / sizeof xc7372_inpad_cfg[0]; k++) {
            int bit, b12, b11, mc;
            if (xc7372_inpad_cfg[k].pin != pin) continue;
            bit = p->cfg_fbbit_fast[xc7372_inpad_cfg[k].fb - 1];
            if (bit < 0) return 0;
            mc  = xc7372_inpad_cfg[k].mc;
            b12 = !jed_owned(jf, 12, cfg_offset(mc), cfg_group(mc), bit, 1);
            b11 = !jed_owned(jf, 11, 96, 0, 2, 1);
            if (!b12 && !b11) return 9;
            if ( b12 && !b11) return 10;
            return 12;
        }
        return 0;
    }
    if (p->nfb != 6) return 0;
    for (k = 0; k < 8; k++) {
        int fb, bit, b12, b11;
        if (xc7354_inpad_cfg[k].pin != pin) continue;
        fb  = xc7354_inpad_cfg[k].fb;
        bit = p->cfg_fbbit[fb - 1];
        if (bit < 0) return 0;
        /* MEASURED PROPERLY -- 24 probes, all eight dedicated
         * input pads crossed with the clock frozen to PIN08, PIN09 and
         * PIN10, every one verified in the fitter's `.pin`.
         *
         *   blk12 at the PAD'S OWN macrocell address, at the pad's block
         *         config bit -- flips between PIN08 and PIN09.
         *   blk11 at a FIXED address, offset 96 group 0 BIT 2, THE SAME FOR
         *         EVERY PAD -- flips only for PIN10.
         *
         * had blk11 at the fixed address but used the PAD'S block
         * bit, which is 3 for the FB6-band pads and therefore the wrong bit;
         * then moved blk11 to the pad's address, which fixed those
         * three by accident and broke PIN02. Both were guesses. The bit is
         * literally 2 and the address is fixed, for all eight pads. */
        b12 = !jed_owned(jf, 12, cfg_offset(xc7354_inpad_cfg[k].mc),
                         cfg_group(xc7354_inpad_cfg[k].mc), bit, 1);
        b11 = !jed_owned(jf, 11, 96, 0, 2, 1);
        if (!b12 && !b11) return 8;
        if ( b12 && !b11) return 9;
        return 10;
    }
    return 0;
}


int device_row_per_record(const JedFile *jf) {
    return profile_for(jf)->row_per_record;
}

int mc_in_use(const JedFile *jf, int fb, int mc) {
    int row;
    /* XC7372's MC1 has SIX rows and this loop was fixed
     * at five, so a donor whose ONLY active row is row 5 was declared
     * UNUSED -- the macrocell was skipped entirely, its rows never read and
     * its exported term lost. `K0108` is exactly that: FB2_MC1 buried, one
     * term `/N7` in group 5, nothing anywhere else.
     *
     * this used to carry its OWN local copy of the "mc==1 &&
     * XC7372" test (`nrow = mc==1&&XC7372 ? 6 : 5`) -- now calls the
     * shared `mc_priv_rows_x`, the same function `decode.c`'s own
     * `rowN_state`/`rowN_slot` use, so the row-count model has exactly
     * ONE definition again. */
    for (row = 0; row < mc_priv_rows_x(jf, fb, mc); row++)
        if (row_state(jf, fb, mc, row) == ROW_ACTIVE) return 1;
    return 0;
}

RowState row_state(const JedFile *jf, int fb, int mc, int row) {
    int blk, ones = 0;
    /* trace what this scan actually sees. `0 dropped` says the term
     * never reaches resolution, and this is the gate immediately before it. */
    for (blk = 0; blk < 48; blk++) ones += row_fuse(jf, fb, mc, row, blk);
    if (getenv("XC73_TRACE_ROW"))
        fprintf(stderr, "  row_state FB%d_MC%d row%d : ones=%d of 48 -> %s\n",
                fb, mc, row, ones,
                ones==48 ? "DISABLED" : (ones==0 ? "UNUSED" : "ACTIVE"));
    if (ones == 48) return ROW_DISABLED;
    if (ones == 0)  return ROW_UNUSED;
    return ROW_ACTIVE;
}

int slot_used(const JedFile *jf, int fb, int mc, int row, int slot, int *neg) {
    int e = row_fuse(jf, fb, mc, row, 2 * slot);
    int o = row_fuse(jf, fb, mc, row, 2 * slot + 1);
    if (!e && !o) return 0;
    *neg = e ? 1 : 0;           /* even = complement, odd = true */
    /* Dump the decision rather than let an
     * analysis script re-derive the row address. Measurement showed the decoder
     * emitting NEGATED dedicated-input literals where the design has positive
     * ones, and the first attempt to read the two columns by hand guessed the
     * record offset -- the exact trap this avoids.
     *
     *     XC73_DUMP_ROWS=FBxx_MCy  xc7300dump decode f.jed out 2>&1 >/dev/null
     */
    {   const char *w = getenv("XC73_DUMP_ROWS");
        if (w && *w) {
            char tag[32];
            snprintf(tag, sizeof tag, "FB%d_MC%d", fb, mc);
            if (strcmp(w, "1") == 0 || strcmp(w, tag) == 0)
                fprintf(stderr, "ROWBIT %-9s row%d slot%-3d even(col%2d)=%d "
                        "odd(col%2d)=%d -> %s\n", tag, row, slot,
                        2 * slot, e, 2 * slot + 1, o, *neg ? "NEGATED" : "true");
        }
    }
    return 1;
}


/* EVERY named source on a UIM line, not just one.
 *
 * The UIM is a WIRED-AND crossbar (SMARTswitch): a line with two closed
 * crosspoints carries the AND of both sources, and `NODE (UIM) x` in PLUSASM
 * is how a design asks for exactly that. `uim_source_for_line` returns a
 * source only when there is exactly ONE -- correct for naming, useless for
 * emitting, and its predecessor took the FIRST of several, which SILENTLY
 * DROPPED the rest.
 *
 * Measured on A6, whose `.EQN` is `NB = A * B ; UIM node` and
 * `/Q1 := /C + /NB`: line 1 into FB3 has two crosspoints, slot 24 and slot 32.
 * v1.24 emitted `/I_42 + /I_7` -- one source of NB and none of the other --
 * so the recovered design was NOT equivalent to the chip. That is the silent
 * lie this project exists to avoid, and it sat in a shipped decompiler.
 *
 * Returns the count; fills up to `cap` slots. */
/* does this macrocell's OUTPUT appear as a UIM crosspoint source?
 *
 * `is_exporter` rests on the premise that a used,
 * combinatorial, buried macrocell "has nowhere for its terms to go EXCEPT the
 * export chain". The UIM is somewhere for them to go, and it is decodable, so
 * the premise is testable rather than assumed. A macrocell feeding a UIM
 * wired-AND node is an ordinary output that happens not to reach a pin -- it
 * is NOT donating its product terms to its neighbour.
 *
 * This scans the whole crosspoint array for the slot that names (fb,mc) as a
 * macrocell OUTPUT. It reuses `uim_slot_decode_dev` rather than re-deriving
 * the slot, per methodology rule 4. */
/* `decode_emit` hardcoded 16 for local feedback. That address is
 * the profile's `cfg_localfb`, which is -1 (NOT LOCATED) on XC7354 -- and
 * `cfg_bit` cannot report that, because it only returns -1 when the block
 * NUMBER is negative, never when the caller supplies one the device has not
 * had located. Methodology rule 4: one address, one place. */
int localfb_blk(const JedFile *jf) { return profile_for(jf)->cfg_localfb; }

int mc_drives_uim(const JedFile *jf, int fb, int mc) {
    const DevProfile *p = profile_for(jf);
    int tfb, line, slots[64], n, k;
    for (tfb = 1; tfb <= p->nfb; tfb++)
        for (line = 0; line < 24; line++) {
            /* lines 21, 22 and 23 DO NOT EXIST on a High-Density
             * block -- it has 21 UIM inputs, not 24 -- and their crosspoints
             * are held at a CONSTANT 1. `uim_sources_for_line` reports them
             * as sources anyway, so scanning them made EVERY macrocell look
             * like a UIM driver and this test answered yes universally.
             *
             * That is the instrument-error signature this project keeps
             * meeting: a sweep that agrees unanimously is measuring itself.
             * It cost 22 previously-passing designs before the round cycle
             * caught it, which is exactly what the round cycle is for. */
            /* ELEVENTH `nfb == 6` guard. On XC7372 the
             * non-existent High-Density lines 21..23 were scanned, their
             * constant-1 crosspoints read as sources, and EVERY macrocell
             * came back as a UIM driver -- so `is_exporter`'s
             * `!drives_uim` was false everywhere and the export merge never
             * fired on any design. The comment above says this exact failure
             * cost 22 designs on XC7354; the guard that fixed it was written
             * as an equality and excluded the next device. */
            if (p->nfb >= 6 && tfb > p->fast_upto && line >= 21) continue;
            n = uim_sources_for_line(jf, tfb, line, slots, 64);
            for (k = 0; k < n; k++) {
                int sfb, smc, isout;
                if (!uim_slot_decode_dev(jf, slots[k], &sfb, &smc, &isout))
                    continue;
                if (isout && sfb == fb && smc == mc) return 1;
            }
        }
    return 0;
}

/* The LINE ASSOCIATION, measured. `block = SOURCE`, offset 96,
 * `bit = 16*slot + cfg_bit(consumer_fb)`. 58 of 108 sources read directly
 * from a 214-design campaign -- every one giving EXACTLY two bits at ONE
 * block, zero non-conforming -- and the table is perfectly regular:
 *
 *     base   FB1 96 · FB2 104 · FB3 352 · FB4 224 · FB5 112 · FB6 240
 *            FB7 368 · FB8 376 · FB9 248 · FB10 120 · FB11 232 · FB12 360
 *     MC5,6 -> +0   MC7,8 -> +1   MC9 -> +2   MC1,2 -> +4   MC3,4 -> +5
 *
 * The even macrocells were not separately observable (the fitter placed them
 * identically whichever order they were declared), so they are PAIRED with
 * their odd neighbour -- which is sound because the CROSSPOINT already says
 * which macrocell of the pair is connected. The round
 * cycle is the test of that pairing. */
static int x108_line_base(int fb) {
    static const int b[12] = { 96,104,352,224,112,240,368,376,248,120,232,360 };
    return (fb >= 1 && fb <= 12) ? b[fb - 1] : -1;
}
/* MEASURED, 18 pinned probes, two function blocks, 9 of 9 each.
 *
 * Every macrocell of a block has its OWN slot: the block offset says WHICH
 * record and the BANK says which of the two macrocells sharing it. The
 * pairing was off by one -- it read (MC1,MC2)->+4, (MC3,MC4)->+5 ... when the
 * truth is MC1 ALONE at +4, then (MC2,MC3)->+5, (MC4,MC5)->+0, (MC6,MC7)->+1,
 * (MC8,MC9)->+2.
 *
 *     MC1 +4 HIGH   MC2 +5 LOW    MC3 +5 HIGH
 *     MC4 +0 LOW    MC5 +0 HIGH   MC6 +1 LOW
 *     MC7 +1 HIGH   MC8 +2 LOW    MC9 +2 HIGH
 *
 * ODD macrocells take the HIGH bank (96..116), EVEN take the LOW (32..52).
 * Nine macrocells over five records: four records carry two banks each and
 * `+4` carries one, which is exactly the measured asymmetry -- the `+4`
 * block has no low bank because MC1 is its only occupant, not because "MC2
 * has no crossbar". */
static int x108_line_block(int fb, int mc) {
    static const int d[9] = { 4,5,5,0,0,1,1,2,2 };   /* MC1..MC9 */
    int b = x108_line_base(fb);
    return (b < 0 || mc < 1 || mc > 9) ? -1 : b + d[mc - 1];
}
/* 1 = this macrocell's claims live in the HIGH bank, 0 = the LOW bank. */
static int x108_line_bank_hi(int mc) { return (mc & 1) != 0; }

/* INPUT PADS as UIM sources. Measured on nine designs, each with a
 * consumer reading ONE pad: the pad claims its slot at ITS OWN pad-slot
 * address -- (block, offset) with two pads per block at offsets
 * 32 and 96 -- using the same `bit = 16*slot + cfg_bit(consumer_fb)`.
 *
 *   PIN23 (216,32)  PIN24 (216,96)  PIN25 (217,32)
 *   PIN31 (344,32)  PIN32 (344,96)  PIN44 (336,32)
 *   PIN45 (336,96)  PIN56 (208,32)  PIN57 (208,96)
 *
 * nine of nine exact against the pad table already measured. */
/* THE PAD-INVERSION TABLE.
 *
 * A UIM wired-AND node that needs `/P7` cannot invert a PIN the way it inverts
 * A macrocell (the blk21..24 polarity field) -- a pad has no macrocell.
 * Instead the pad's LINE is inverted for the whole device, and every ordinary
 * row that wants plain `P7` selects the COMPLEMENT column. Reading the column
 * alone therefore reports the wrong polarity.
 *
 * MEASURED on XC73108-PC84, ALL TWELVE dedicated inputs, one design per pin:
 * the negated member's pin frozen with a `.VMF`, diffed against its positive
 * twin. Block 15 throughout, `bit = 8*group + idx`:
 *
 *     pin  offset  bit      pin  offset  bit     k  (offset, group)
 *       2    104    51       79    104    50     0     (104, 6)
 *       3    104    35       80    104    34     1     (104, 4)
 *       4    104    19       81    104    18     2     (104, 2)
 *       5    104     3       82    104     2     3     (104, 0)
 *       6    100     3       83    100     2     4     (100, 0)
 *       7     96    51       84     96    50     5     (96,  6)
 *
 * The two halves share (offset, group) at EVERY position; only the low index
 * differs -- 3 for pins 2..7, 2 for pins 79..84. The irregularity at k=4,5 is
 * measured independently in both halves, so it is structure, not noise.
 *
 * This is a COMPLETE ENUMERATION of the dedicated inputs, not a formula fitted
 * to a few of them. It does NOT cover an I/O pad used as an input,
 * and the caller must keep refusing those.
 *
 * Returns 1 inverted, 0 not inverted, -1 not a pin this table covers. */
int x108_pad_inverted(const JedFile *jf, int pin) {
    static const int OFF[6] = { 104, 104, 104, 104, 100, 96 };
    static const int GRP[6] = {   6,   4,   2,   0,   0,  6 };
    const DevProfile *p = profile_for(jf);
    int k, idx, bit;
    if (!p || p->nfb != 12) return -1;              /* XC73108 only          */
    if (!strstr(jf->device, "PC84")) return -1;     /* measured on PC84 only */
    if (pin >= 2 && pin <= 7)        { k = pin - 2;  idx = 3; }
    else if (pin >= 79 && pin <= 84) { k = pin - 79; idx = 2; }
    else if (pin == 11) {
        /* PIN11 (FB10_MC9) is the one input-capable I/O PAD on
         * PC84. Its inversion sits in the SAME block-15 structure as the
         * twelve dedicated inputs -- measured at offset 96, bit 3, the
         * `idx = 3` family. One measurement, verified the same way
         * as the pad ADDRESS: by round cycle, not by a second
         * address point. */
        bit = 3;
        return jed_owned(jf, 15, 96, bit / 8, bit % 8, 0) ? 0 : 1;
    }
    else return -1;
    bit = 8 * GRP[k] + idx;
    /* The fuse reads 1 when the line is NOT inverted and 0 when it is --
     * measured the same way in all twelve pairs. */
    return jed_owned(jf, 15, OFF[k], bit / 8, bit % 8, 0) ? 0 : 1;
}

static int x108_pad_addr(const JedFile *jf, int pin, int *blk, int *off) {
    int fb, mc, base;
    static const int pb[13] = { 0,0,0,0,0,0,208,336,344,216,0,0,0 };
    /* this whole function was PC84-ONLY (the
     * dedicated-input band and the FB10 pad address were both measured
     * on PC84, but had NO package check at all -- called
     * unconditionally for every package, including a hardcoded literal
     * "PC84" passed to `device_pin_macrocell` a few lines down
     * regardless of what `jf` actually targets. On XC73108-PQ160 this
     * named dedicated inputs at PC84's pin NUMBERS (`I_2`, `I_80`, ...)
     * on a package where those numbers mean something else entirely --
     * confirmed directly: PLUSASM's own refit rejects them (`PIN02` is
     * type CE, `PIN80` is VCC/VSS/MR on PQ160, not an input at all).
     * `x108_pad_inverted` (this function's sibling, right above) ALREADY
     * gates on PC84 and returns -1 rather than guess; this one now does
     * the same rather than name a pin that is wrong on any other
     * package. PQ160's real dedicated-input band is UNMEASURED -- same
     * scope note as `x108_pad_inverted`'s "OTHER PACKAGES ARE NOT
     * MEASURED". */
    /* The DEDICATED-INPUT band below (pins 2-7,
     * 79-84) and the FB10/MC9 special case are PC84 PIN NUMBERS -- those
     * stay gated to PC84 exactly as before, unmeasured on any other
     * package. But the ORDINARY macrocell-pad lookup a few lines down
     * hardcoded the STRING "PC84" into `device_pin_macrocell` regardless
     * of what `jf` actually targets -- the same class of bug the
     * `decode_pkg` fix (this file, `split_part`) already fixed for
     * `pin_for_macrocell`/`npins_of`'s callers, just not applied HERE.
     * `pb[]`'s formula is a function of (fb, mc) alone, resolved from the
     * PACKAGE-SPECIFIC pin table `device_pin_macrocell` already carries
     * for PQ160 (`vt[]`, device.c) -- getting the real package via
     * `split_part` (already used throughout this file for the same job)
     * lets that table resolve PQ160 pins too, with NO new address guessed. */
    {   /* `split_part` re-parses the SAME `jf->device`
         * string on every one of this function's ~39.5M calls (gprof,
         * a dense design) -- `jf` is the one file being decoded for the
         * whole lifetime of a `decode` invocation, so its `device`
         * string cannot change between calls. Cache by POINTER IDENTITY
         * of `jf` (safe: `decode()`'s own `jf` is loaded once and never
         * reassigned to a different file within one process run) rather
         * than re-splitting the string every time. */
        static const JedFile *cached_jf = NULL;
        static char fam[16], pkg[16];
        if (jf != cached_jf) {
            split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
            cached_jf = jf;
        }
        if (strcasecmp(pkg, "PC84") == 0) {
            /* The DEDICATED INPUT pins are their own band, two
             * pins per block at offsets 32 and 96, measured 12 of 12:
             *     PIN2 (88,32) PIN3 (88,96) PIN4 (89,32) PIN5 (89,96)
             *     PIN6 (90,32) PIN7 (90,96)
             *     PIN79 (80,32) PIN80 (80,96) PIN81 (81,32) PIN82 (81,96)
             *     PIN83 (82,32) PIN84 (82,96)                            */
            if (pin >= 2 && pin <= 7) {
                *blk = 88 + (pin - 2) / 2;
                *off = (pin & 1) ? 96 : 32;
                return 1;
            }
            if (pin >= 79 && pin <= 84) {
                *blk = 80 + (pin - 79) / 2;
                *off = (pin & 1) ? 32 : 96;
                return 1;
            }
        }
        /* PQ160's dedicated-input band,
         * measured (12 of 12 pins) and resolved as PAD-INTRINSIC --
         * (block, offset-family) depends ONLY on which pad, never on
         * which consumer reads it. Confirmed by four independent
         * consumer function blocks (FB3, FB6, FB10, FB12), each with the
         * SAME source pad held fixed: (block, offset-family) never
         * moved; only the caller's own `bit = 16*(line%4) +
         * cfg_fbbit[fb-1]` (this file, `x108_claims_line`'s "try the
         * INPUT PADS" loop, already written, untouched here) selects the
         * consumer -- and every measured bit matched that EXISTING
         * formula exactly (FB3->8, FB6->6, FB10->3, FB12->9, all
         * `cfg_fbbit[fb-1]` at line%4==0). This is the SAME shape as
         * PC84's own dedicated-input band above -- one lookup by PIN,
         * offset-family only, no consumer dependence in this function --
         * so wiring it costs no new caller logic at all.
         *
         * `.PIN` report confirmed (two independent designs) PQ160 has
         * EXACTLY 12 pure "I"-type pins, not the ~50 once assumed:
         *     rank  pin  block off   rank  pin  block off
         *      1    11    80   32     7    22    88   32
         *      2    13    80   96     8    23    88   96
         *      3    15    81   32     9    24    89   32
         *      4    17    81   96    10    26    89   96
         *      5    18    82   32    11    28    90   32
         *      6    19    82   96    12    30    90   96
         *
         * VALIDATED at band 0 only (line 0-3, one or a few
         * dedicated-pad-sourced UIM claims per consumer) -- every probe
         * the validation used a single product term. Band 1+ (a consumer
         * needing five or more UIM lines where several come from
         * dedicated pads) is UNMEASURED for this package; round-cycle
         * testing must exercise it directly before this is
         * trusted at scale. */
        if (strcasecmp(pkg, "PQ160") == 0) {
            static const int pq160_pin[12] =
                { 11,13,15,17,18,19,22,23,24,26,28,30 };
            int i;
            for (i = 0; i < 12; i++)
                if (pq160_pin[i] == pin) {
                    int rank = i + 1;
                    *off = (rank & 1) ? 32 : 96;
                    *blk = (rank <= 6) ? 80 + (rank - 1) / 2
                                       : 88 + (rank - 7) / 2;
                    return 1;
                }
        }
        /* PG144/BG225's dedicated-input band. Measured:
         * IDENTICAL (block,offset) family on both packages, and
         * the SAME rank<->(block,offset) formula PQ160 already uses above
         * (verified algebraically against the 12 measured
         * points before being wired -- no new formula, only a new PIN
         * IDENTITY problem: these two packages have no numeric pin
         * numbering at all, only alphanumeric ball labels ("E1", "G144"
         * is not one -- real examples are "F3", "K1", ...). `pin` here is
         * therefore NOT a physical pin -- it is an arbitrary RANK 1-12,
         * this function's caller (`x108_line_of_rank`'s pad-scan loop,
         * `uim_sources_for_line`'s twin, both already iterating pin=1..84
         * for every package unconditionally) simply never matches
         * anything above rank 12 here. The rank<->ball correspondence
         * (needed ONLY to print the real ball name in the `.VMF`, never
         * to compute an address) is `pg144_bg225_pad_ball[]` below,
         * shared by both `decode.c` VMF-writer sites this same rank
         * feeds. Inversion: UNMEASURED on these packages (same scope
         * limit `x108_pad_inverted` already documents for every package
         * but PC84) -- `x108_pad_inverted` returns -1 here unchanged and
         * The existing negated-literal guard refuses rather than
         * guesses, exactly as it already does for PQ160. */
        if (strcasecmp(pkg, "PG144") == 0 || strcasecmp(pkg, "BG225") == 0) {
            if (pin >= 1 && pin <= 12) {
                int rank = pin;
                *off = (rank & 1) ? 32 : 96;
                *blk = (rank <= 6) ? 80 + (rank - 1) / 2
                                   : 88 + (rank - 7) / 2;
                return 1;
            }
        }
        /* `device_find("XC73108")` re-resolves the
         * SAME compile-time-constant string on every single call --
         * this function alone accounts for ~39.5M calls on one dense
         * design (gprof), each re-running `device_find`'s own
         * strncasecmp scan over the device table for an answer that
         * cannot change within one process's lifetime (the table is
         * `static const`). Cached once; safe because the argument is
         * a literal, never a caller-supplied name. */
        static const device_t *dev73108 = NULL;
        if (!dev73108) dev73108 = device_find("XC73108");
        if (!device_pin_macrocell(dev73108, pkg, pin, &fb, &mc))
            return 0;
    }
    if (fb < 1 || fb > 12) return 0;
    /* FB10 has no `pb[]` base and PIN11 (FB10_MC9) is the ONE
     * input-capable I/O pad on PC84 -- FB10's other pins are FastClock,
     * FB1/FB2 are Fast Outputs and FB5's are Clock Enable.
     * Without an address the pad is never NAMED, so the input vanishes from
     * the emitted design entirely: `INPUTPIN I_79 I_80 I_81 I_82 I_83` where
     * the fitter's `.PIN` says `11 I/O I IOP`, and the round cycle fails
     * `PIN SETS DIFFER [11]`. Silently -- the guard cannot fire on
     * a pad that produces no literal.
     *
     * Measurement showed PIN11's claim at BLOCK 92, LOW family, from a
     * two-fuse matched pair, and declined to write it because one data point
     * cannot fix `pb[]`'s formula (`pb[10] + 4 = 92` would need `pb[10] = 88`,
     * which collides with the dedicated-input block).
     *
     * So it is NOT written as a `pb[]` base. It is written as the one
     * measurement it is, and VERIFIED FUNCTIONALLY: `IOP`/`ION` must
     * round-cycle. A single address point checked by round cycle is stronger
     * evidence than a second address point would have been. */
    /* PC84-only: PIN11/(FB10,MC9)'s address was a single measured point,
     * not a formula -- stays gated exactly as before. */
    if (fb == 10 && mc == 9 && strstr(jf->device, "PC84")) {
        *blk = 92; *off = 32; return 1;
    }
    base = pb[fb];
    if (!base) return 0;
    *blk = base + (mc - 1) / 2;
    *off = ((mc - 1) & 1) ? 96 : 32;
    return 1;
}

/* PG144/BG225 have no numeric pin, so the RANK
 * `x108_pad_addr` (above) uses in place of one is meaningless outside this
 * file -- `decode.c`'s `.VMF` writer must print the REAL ball name for a
 * human/PLUSASM-readable freeze line (`PIN<ballname>`, confirmed working
 * syntax,. This is the one place that translation happens; every
 * OTHER call site (literal naming, `seen_in[]`, `x108_pad_inverted`) keeps
 * using the rank exactly as if it were a physical pin, unchanged. Returns
 * 1 and fills `out` if `rank` (1-12) is one of the twelve dedicated pads
 * on this package, 0 otherwise (any other package, or an out-of-range
 * rank). Table order matches `x108_pad_addr`'s own rank<->(block,offset)
 * assignment exactly (verified: F3->(80,32), E1->(80,96), G3->(81,32),
 * G1->(81,96), H2->(82,32), H1->(82,96), J1->(88,32), K1->(88,96),
 * J2->(89,32), K3->(89,96), L2->(90,32), N1->(90,96), the
 * measured table). */
int x108_pad_ball(const JedFile *jf, int rank, char *out, size_t outsz) {
    static const char *ball[12] =
        { "F3","E1","G3","G1","H2","H1","J1","K1","J2","K3","L2","N1" };
    char fam[16], pkg[16];
    if (!jf || !out || outsz == 0 || rank < 1 || rank > 12) return 0;
    split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
    if (strcasecmp(pkg, "PG144") != 0 && strcasecmp(pkg, "BG225") != 0)
        return 0;
    {   size_t n = strlen(ball[rank - 1]);
        if (n >= outsz) return 0;
        memcpy(out, ball[rank - 1], n + 1);
    }
    return 1;
}

/* XC73108-PQ100's dedicated-input crosspoint
 * ADDRESS, measured -- a COMPLETELY DIFFERENT SHAPE from PC84's
 * (`x108_pad_addr`, above). **NOT YET WIRED INTO `uim_sources_for_line`**
 * -- see the comment at that call site for why: the field carries no
 * per-UIM-LINE information, and this project's rank/line resolution
 * machinery needs one. The functions below are correct and validated
 * (12 of 12 pin-to-select-bit points, 12 of 12 consumer-to-block points)
 * as a WHICH-PAD-FEEDS-WHICH-FB table; using them to actually resolve a
 * literal needs that separate, unsolved question answered first. PC84's
 * pad claim lives in a small, PAD-specific block, with the
 * CONSUMER identified by an (offset,bit) pair computed from the LINE.
 * PQ100's lives in a CONSUMER-FUNCTION-BLOCK-specific block/offset pair
 * (MC-independent -- confirmed on four different consuming macrocells,
 * FB1_MC1/FB3_MC9/FB4_MC7/FB5_MC3, all following the SAME rule), with the
 * PAD identified by a 12-bit ONE-HOT SELECT field reusing this device's
 * OWN row-bit permutation (`rb[]` -- the same twelve values
 * `prof_73108.fbbit[]`/`cfg_fbbit[]` already carry,.
 *
 * MEASURED: PQ100 has 12 dedicated ("I"-type) pins, pins 6,8,9,10,11,12,
 * 14,15,16,17,18,20 -- PLUSASM assigns them to `INPUTPIN` declarations in
 * ASCENDING PIN ORDER (confirmed directly from a fitted `.PIN` report,
 * a test design). The first attempt to
 * measure this was noisy (65 differing records instead of ~12) because it
 * compared a 2-literal AND against a 1-literal buffer -- changing TERM
 * SHAPE, not just pad identity (rule 0be). This measurement keeps shape
 * constant: ten matched pairs, each swapping WHICH of two already-used
 * dedicated pins feeds one already-PARTITIONed consumer, placement and
 * pin assignment verified identical before trusting the diff
 * (a test design). Each pair differs at exactly two
 * small record families (the two consumers' own blocks), never the noisy
 * 65-record shape.
 *
 * Per pin RANK (1-based, ascending pin number):
 *
 *     rank   1  2  3  4  5  6   7   8  9  10 11 12
 *     pin    6  8  9 10 11 12  14  15 16  17 18 20
 *     idx    0  1  8  4  2  6  10  11  7   3  5  9
 *
 * `idx` is EXACTLY `rb[rank-1]`. `group = idx/8, bit = idx%8` (group0 all
 * eight bits, group1 only bits 0-3 -- twelve slots for twelve pins,
 * nothing left over).
 *
 * The consumer BLOCK/OFFSET (stride 64 within pairs of FBs, offset
 * alternating 32/96 exactly like the PC84 dedicated-input pairing):
 *
 *     FB1..FB6:   block 80 + (fb-1)/2,  offset (fb odd) 32 else 96
 *     FB7..FB12:  block 88 + (fb-7)/2,  offset (fb odd) 32 else 96
 *
 * NOT MEASURED: two dedicated pads feeding the SAME consumer function
 * block via two different macrocells (every probe used exactly one
 * macrocell per block) -- if the field is genuinely per-FB, as measured,
 * this shape cannot arise (one one-hot field per FB), but that is
 * inferred, not independently confirmed. Pad POLARITY/inversion on
 * PQ100 is ALSO not measured (`x108_pad_inverted` stays PC84-gated) --
 * every probe used a positive pad literal; a negated dedicated-input
 * literal on PQ100 remains unhandled. */
static int x108_pad_block_pq100(int dfb, int *off) {
    if (dfb < 1 || dfb > 12) return -1;
    *off = (dfb & 1) ? 32 : 96;
    return (dfb <= 6) ? 80 + (dfb - 1) / 2 : 88 + (dfb - 7) / 2;
}
static int x108_pad_select_pq100(int pin, int *group, int *bit) {
    static const int rank_pin[12] = { 6,8,9,10,11,12,14,15,16,17,18,20 };
    static const int rb[12] = { 0,1,8,4,2,6,10,11,7,3,5,9 };
    int i;
    for (i = 0; i < 12; i++)
        if (rank_pin[i] == pin) { *group = rb[i] / 8; *bit = rb[i] % 8; return 1; }
    return 0;
}

/* the slot a source drives into `dfb`, or -1 */
/* line = 4*band + slot, bit = 16*slot + cfg_fbbit(consumer). */
static int x108_claims_line(const JedFile *jf, const DevProfile *p,
                            int sfb, int smc, int dfb, int line) {
    /* there are TWO banks. The high bank (96..116) was the only one
     * tested; the LOW bank (32..52) carries claims as well -- measured on a
     * packed design, FB4 has 2 slot-bits set at each of offsets 36, 40 and 44
     * while the high bank yields only its four self-feeds.
     *
     * An earlier version of this comment said "Offset 52 is
     * excluded". **THE CODE HAS NEVER EXCLUDED IT** and must not:
     * measured offsets 52 and 116 slot by slot over five designs --
     *
     *     slots 1,2,3   set in EVERY design including a near-empty one
     *                   -- the constant fill marking lines 21, 22, 23
     *     slot 0        0.000 when the design is empty, non-zero when it is
     *                   not -- LINE 20 IS REAL DATA
     *
     * Excluding the offset would throw line 20 away with the padding. What
     * keeps the constant out is the CALLER's guard, which skips lines 21..23
     * for a High-Density consumer only -- and measurement showed that the fill
     * is set for FB3..FB12 and CLEAR for FB1 and FB2 (0 of 108 records), so
     * for a FAST consumer those three lines are real and carry real claims.
     *
     * A comment that says the opposite of the code is worse than none: this
     * one cost a session's worth of misreading. */
    static const int BOFF[6] = { 96, 100, 104, 108, 112, 116 };
    static const int BLOW[6] = { 32,  36,  40,  44,  48,  52 };
    int blk = x108_line_block(sfb, smc);
    int band = line / 4, slot = line % 4, bit, off;
    if (blk < 0 || band < 0 || band >= 6) return 0;
    bit = 16 * slot + p->cfg_fbbit[dfb - 1];
    if (bit < 0 || bit > 63) return 0;
    /* The bank is NOT a search: it names the macrocell. Reading both
     * over-claimed by returning the other member of the record as well, which
     * is the whole symptom -- the COUNT was right and the
     * IDENTITY was wrong. */
    off = x108_line_bank_hi(smc) ? BOFF[band] : BLOW[band];
    {   int r = jed_owned(jf, (size_t)blk, off, bit / 8, bit % 8, 0);
        /* up to 3 `getenv()` + 2 `atoi()` calls per
         * invocation, ~64.7M invocations on a dense design (gprof) --
         * cache the trace toggle once. These are debug env vars a probe
         * sets before running `decode`, never mid-run. */
        static int checked = 0, has_dfb = 0, want_dfb = 0, has_l = 0, want_line = 0;
        if (!checked) {
            const char *e1 = getenv("XC73_TRACE_CLAIMLINE");
            has_dfb = (e1 != NULL);
            want_dfb = e1 ? atoi(e1) : 0;
            {   const char *e2 = getenv("XC73_TRACE_CLAIMLINE_L");
                has_l = (e2 != NULL);
                want_line = e2 ? atoi(e2) : 0;
            }
            checked = 1;
        }
        if (has_dfb && dfb == want_dfb && (!has_l || line == want_line))
            fprintf(stderr, "CLAIMLINE sfb=%d smc=%d dfb=%d line=%d blk=%d "
                    "band=%d slot=%d off=%d bit=%d(g%d/b%d) -> %d\n",
                    sfb, smc, dfb, line, blk, band, slot, off, bit,
                    bit/8, bit%8, r);
        return r;
    }
}


/* The ROW SLOT IS A RANK, NOT A LINE NUMBER.
 *
 * Measured on FB10 of a packed design, tool row slots against the signals the
 * fitter says each macrocell reads:
 *
 *     signals 0 1 2 3 4 5 33 34 35 60 61 62 63
 *     slots   0 1 2 3 4 5  6  7  8  9 10 11 12
 *
 * The slot is the RANK of the signal among the DISTINCT signals that block
 * uses, sorted ascending. Signals 0..5 happen to map to slots 0..5, which is
 * why sparse designs -- where a block uses only low signals -- worked by
 * accident for years.
 *
 * So `uim_sources_for_line(fb, k)` must resolve k as a RANK: collect every
 * line any source claims for `fb`, sort, and take the k-th. Treating k as the
 * line directly is the whole defect vs. */
static int x108_line_of_rank(const JedFile *jf, const DevProfile *p,
                             int fb, int rank) {
    int seen[24], safe[24], n = 0, line, sfb, smc, i, j, t;
    /* A FAST block has TWENTY-FOUR UIM inputs and a High-Density
     * block twenty-one, from `.MAP` reporting `UIM inputs - n/24`
     * against `n/21`). `x108_claims_line` already honours that -- it excludes
     * lines 21..23 for HD blocks only -- but THIS loop stopped at 21 for
     * every block, so the three lines a FAST block really has were never
     * collected and every rank above the block's 21st was unresolvable.
     *
     * That is the `K63` failure at `FB1_MC8 row2 slot22`: slot 22 needs the
     * 23rd line, which exists on FB1 and was never scanned.
     *
     * The band offsets themselves are NOT at fault. Measured with 18 pinned
     * sources feeding one consumer block and nothing else: the claims land on
     * lines 0..17 in exact source order, across bands 0 to 4, at the offsets
     * already in the table. */
    /* XC7336 has fast_upto = 4 -- EVERY block is FAST -- and all four
     * decode. It escapes this branch only via `nfb >= 6`. So the FAST path
     * itself is what fails, not FAST blocks as such.
     * `XC73_FAST_NLINE=21` forces a FAST block down the High-Density line
     * count, to test exactly that. */
    int nline = (p->nfb >= 6 && fb <= p->fast_upto) ? 24 : 21;
    {   const char *e = getenv("XC73_FAST_NLINE");
        if (e && nline == 24) nline = atoi(e);
    }
    for (line = 0; line < nline && n < 24; line++) {
        int got = 0;
        /* `x108_uim_connected`'s blocks-{10,21,22} formula never
         * carries a bit for a FAST-block source, measured on BF1/
         * BN2) -- so a FAST source vetoes itself out of this loop before
         * `x108_claims_line` is ever asked, and a HD consumer reading a
         * FAST-block product-term clock (or any FAST source) refuses with
         * "the UIM does not resolve this line to a source" on a design
         * that is otherwise completely ordinary. Minimal reproducer: one
         * FAST-block node feeding a single HD macrocell's CLKF, nothing
         * else (a test design).
         *
         * `x108_claims_line` uses a DIFFERENT address (`x108_line_block`,
         * that already carries a real, per-source,
         * per-consumer, per-line bit -- proven on a matched pair
         * (FB1_MC1 vs FB2_MC1 as the only variable, HD consumer pinned
         * identically in both): the two designs differ at EXACTLY one
         * record, and it decodes as "FB1_MC1 claims UIM line 2 of FB5" /
         * "FB2_MC1 claims..." respectively. So the connectivity answer a
         * FAST source needs is already present -- `x108_uim_connected`
         * just never has a reason to ask, because for HD sources its own
         * blocks-10/21/22 answer is normally enough.
         *
         * Naively removing the veto (treat every FAST sfb as connected,
         * same as the historical `XC73_NOGATE` experiment) was tried
         * first and FAILS: `random_packed/K63.JED` (25 registers, 9
         * wired-AND lines, 60/108 macrocells) then has several lines
         * with TWO simultaneous claimants for the SAME (consumer, line) --
         * always at the top of a block's line range (line 19 or 20, the
         * boundary band already flagged as delicate) -- e.g. FB6
         * line 20 claimed by BOTH FB2_MC5 (FAST) and FB10_MC9 (HD).  That
         * is not evidence the FAST claim is wrong, or that the HD claim
         * is wrong -- it is evidence this exact (fb, line) pair is
         * ambiguous with today's addressing, on TOP of the previously
         * unreachable FAST branch. A crosspoint selects exactly one
         * source in real silicon; two simultaneous claims can only be
         * decoded correctly if resolved, and guessing which one is real
         * is exactly the "wrong design is worse than a refusal" mistake
         * this project reverted once already (the round-cycle
         * failure, 14 of 19 pins).
         *
         * So: enable the FAST path, but require the claim to be UNIQUE.
         * Two designs with zero ambiguity (S73108's real failure, and
         * FASTHD2) are unaffected by this guard -- neither has a second
         * claimant at any line.
         *
         * An earlier version shipped this
         * gate lifted for MC1 and MC6 only, because a BLANKET lift (all
         * nine MC, no uniqueness guard) broke K63. With the uniqueness
         * guard in place, all NINE macrocells were re-tested, one at a
         * time and combined, three ways:
         *   1. a matched-pair differential per macrocell (FB1_MCn vs
         *      FB2_MCn, HD consumer pinned identically) -- ALL NINE give
         *      a clean, single, correctly-named claim (`rdiff` decodes
         *      it as "FBn_MCn claims UIM line 2 of FB5"), same shape as
         *      MC1/MC6's original evidence
         *   2. each of the seven previously-gated macrocells (MC2,3,4,5,
         *      7,8,9), individually and in combination, against K63 via
         *      an equation-level check against the fitter's .EQN (rows vs `.EQN`) -- ZERO new
         *      mismatches beyond the three pre-existing ones (FB4_MC3,
         *      FB6_MC3, FB6_MC6 -- unrelated: none reads a FAST source,
         *      and they mismatch identically whether the FAST gate is
         *      touched or not)
         *   3. a full sequential round cycle (a sequential round-cycle checker, 200 traces x
         *      12 cycles) on each of the seven minimal matched pairs
         *      (a test design) --
         *      ALL SEVEN pass, all output pins agree
         *
         * The "ALL NINE UNLOCKED" IS
         * PARTIALLY WITHDRAWN. Its evidence (points 1-3 above) is all
         * PER-MACROCELL or PER-PAIR -- no design in that campaign used
         * MORE THAN ONE unlocked FAST source at once. Re-testing K63
         * itself (the one design that DOES combine them, 9 wired-AND
         * lines), the "es54 capacity limit" below
         * does NOT reproduce on the actual committed binary -- instead
         * the refit FITS and the round cycle is WRONG, 14 of 19 output
         * pins differing (a sequential round-cycle checker, both frozen and unfrozen refit,
         * twice each for determinism). That is exactly the ambiguous-
         * claim failure mode the revert and this gate's own
         * uniqueness guard exist to prevent, and it means the
         * uniqueness guard is NOT sufficient once several FAST sources
         * are unlocked TOGETHER, only when tested individually.
         *
         * REVERTED to v2.34's original, narrower gate: only MC1 and
         * MC6 stay unlocked (`smc == 1 || smc == 6` below). The other
         * seven macrocells' PER-MACROCELL evidence (points 1 and 3
         * above) is not in question -- each one, ALONE, is clean and
         * round-cycles. What is withdrawn is the COMBINATION claim.
         * A future unlock needs evidence that tests multiple unlocked
         * FAST sources IN ONE DESIGN, round-cycled -- not another
         * one-at-a-time sweep. K63 decodes with 8 unresolved terms
         * again at this narrower gate (matching v2.34's own count) --
         * a refusal, not a wrong answer.
         *
         * K63 ITSELF, WITH ALL NINE (kept for the record, not shipped):
         * cannot be round-cycled -- refitting its decode hits `es54:
         * Could not fit equation 'N_6_7' into its specified partition,
         * too many UIM inputs` in SOME runs and produces a WRONG design
         * in others (see above). Either way it is
         * not safe. This is NOT purely a wrong-value defect in `N_6_7`
         * itself: `N_6_7`'s own decoded equation is BYTE-IDENTICAL
         * whether nine macrocells or two are unlocked (verified
         * directly). It is PARTITION-forced refit exhausting FB6's
         * shared/UIM budget once K63's other, sibling macrocells in
         * the same block also gain the extra terms they were always
         * entitled to -- a fitter capacity limit on this
         * already near-maximal adversarial design -- OR, as observed,
         * it is a wrong design instead, non-deterministically.
         * NOW GATED BACK to MC1/MC6 only. Full corpus
         * (1497 files) and all 5 saturation designs are unaffected by
         * this narrowing either way -- no archived design exercises
         * this pattern at all, unlocked or not. */
        int claimed_unsafe = 0;
        {   int cnt = 0;
            for (sfb = 1; sfb <= p->nfb; sfb++)
                for (smc = 1; smc <= 9; smc++) {
                    int conn = x108_uim_connected(jf, p, sfb, smc);
                    /* was `sfb <= p->fast_upto` (all nine FAST
                     * macrocells) -- withdrawn, see the comment above.
                     * Back to v2.34's original two. */
                    if (conn < 1 && p->nfb >= 6 && sfb <= p->fast_upto &&
                        (smc == 1 || smc == 6))
                        conn = 1;         /* FAST source, no veto */
                    /* (silent-drop audit,: a source can
                     * genuinely claim `line` (x108_claims_line, the
                     * address measurement showed directly) while
                     * the SAFETY gate above refuses to confirm it -- a
                     * FAST source at a macrocell other than MC1/MC6, or
                     * A second, ambiguous claimant the uniqueness
                     * guard exists to reject. Either way the LINE IS
                     * REAL. Silently leaving it out of `seen[]` (the
                     * v2.43 behaviour) shrinks `n` and shifts every
                     * HIGHER rank's line number down by one -- the exact
                     * mechanism measurement showed: FB9_MC2's real line 18
                     * was never queried at all because rank 18 landed on
                     * line 20 instead. Track it separately so it still
                     * occupies a rank slot; a caller that lands exactly
                     * on it gets a REFUSAL (below), never a guess. */
                    if (x108_claims_line(jf, p, sfb, smc, fb, line)) {
                        claimed_unsafe = 1;
                        if (conn >= 1 && cnt < 2) {
                            cnt++;
                            if (getenv("XC73_TRACE_CLAIMS"))
                                fprintf(stderr, "CLAIM FB%d line %d <- FB%d_MC%d\n",
                                        fb, line, sfb, smc);
                        }
                    }
                }
            if (cnt == 1) got = 1;
        }
        if (!got) {                      /* try the INPUT PADS */
            static const int POFF_[6] = { 96, 100, 104, 108, 112, 116 };
            int pin, bit = 16 * (line % 4) + p->cfg_fbbit[fb - 1];
            int band = line / 4;
            if (bit >= 0 && bit <= 63 && band < 6)
                for (pin = 1; pin <= 84 && !got; pin++) {
                    int b2, o2;
                    if (!x108_pad_addr(jf, pin, &b2, &o2)) continue;
                    if (o2 != 32 && o2 != 96) continue;
                    if (jed_owned(jf, (size_t)b2,
                                  (o2 == 32) ? 32 + 4 * band : POFF_[band],
                                  bit / 8, bit % 8, 0))
                        got = 1;
                }
        }
        if (got) {
            for (i = 0; i < n; i++) if (seen[i] == line) break;
            if (i == n) { seen[n] = line; safe[n] = 1; n++; }
        } else if (claimed_unsafe) {
            for (i = 0; i < n; i++) if (seen[i] == line) break;
            if (i == n) { seen[n] = line; safe[n] = 0; n++; }
        }
    }
    for (i = 0; i < n; i++)          /* ascending, safe[] follows seen[] */
        for (j = i + 1; j < n; j++)
            if (seen[j] < seen[i]) {
                t = seen[i]; seen[i] = seen[j]; seen[j] = t;
                t = safe[i]; safe[i] = safe[j]; safe[j] = t;
            }
    /* The flagship wired-AND
     * proof designs (TA/TB) hit this exact "unsafe" path -- 2 to 4
     * genuine, simultaneous claimants forming a real wired-AND, not a
     * competing-candidate ambiguity -- and used to refuse identically
     * to a genuinely wrong case (T73108.JED,. Measured
     * directly: 37 ground-truthed designs from the
     * generated-corpus refusal population, plus TA/TB): an unsafe
     * line is safe to promote to "confirmed wired-AND, decode every
     * member" if and only if BOTH:
     *
     *   (a) TRAILING, measured against the ORIGINAL safe[] this loop
     *       already built -- no SAFE line exists at a HIGHER rank in
     *       this same FB's own sequence. v2.43's pre-666 silent-skip
     *       bug shifted every higher rank's line number down by one
     *       when a line was skipped; if nothing safe follows, no
     *       later resolution is corrupted by treating this one as
     *       resolved too. Computed from the ORIGINAL array, not
     *       re-evaluated as promotions happen, so a whole maximal run
     *       of otherwise-eligible trailing lines (TA's FB6: all 9 of
     *       its lines are simultaneously unsafe, none originally
     *       safe, hence all 9 trivially "trailing") promotes as a
     *       block, not one at a time.
     *   (b) NO INVISIBLE CLAIMANT -- none of the line's real
     *       claimants (`x108_claims_line` hits) is a FAST macrocell
     *       (sfb<=fast_upto) at other than MC1/MC6. Such a claimant
     *       can NEVER be confirmed connected (`x108_uim_connected`'s
     *       own blocks-10/21/22 record carries no fuse for it,
     * and the override above is deliberately narrow,
     * -- `uim_sources_for_line`'s own member-collection
     *       loop, below, uses the IDENTICAL conn filter, so it would
     *       silently omit that member from the decoded equation even
     *       if this line were declared resolved. This is RANKT3.JED's
     *       own defect (2 of its 5 true members, FB1_MC2/FB2_MC2, are
     * exactly this shape, -- the SAME architectural
     * limit already measured unsafe to lift (K63,
     *       14 of 19 pins wrong when MC2-5/7-9 were unlocked wholesale).
     *
     * Verified against all 37 ground-truthed designs/
     * (b) plus TA/TB (the named controls): every one of the 20
     * designs whose v2.43 decode was independently confirmed CORRECT
     * satisfies both (a) and (b) on its own unsafe line (0 exceptions,
     * TA/TB included: all 9 of TA's own claimants are High-Density
     * blocks, condition (b) never triggers). Every one of the 17
     * confirmed NOT EQUIVALENT designs (T73108/S73108/R17T2 and 14
     * others) retains at least one line failing (a) or (b) -- for
     * RANKT3.JED specifically, its OWN first term (line 3, condition
     * (b) fails: FB1_MC2/FB2_MC2) stays refused even though its
     * sibling terms (lines 4/5/6, condition (b) holds: no FAST
     * claimant at all) now correctly promote -- so the WHOLE design
     * still refuses, per its own real defect, not by accident. */
    {   int trailing[24], any_safe_after = 0;
        for (i = n - 1; i >= 0; i--) {
            trailing[i] = !any_safe_after;
            if (safe[i]) any_safe_after = 1;
        }
        for (i = 0; i < n; i++) {
            int L, tsfb, tsmc, invisible;
            if (safe[i] || !trailing[i]) continue;
            L = seen[i];
            invisible = 0;
            for (tsfb = 1; tsfb <= p->nfb && !invisible; tsfb++) {
                if (!(p->nfb >= 6 && tsfb <= p->fast_upto)) continue;
                for (tsmc = 1; tsmc <= 9; tsmc++) {
                    if (tsmc == 1 || tsmc == 6) continue;
                    if (x108_claims_line(jf, p, tsfb, tsmc, fb, L)) {
                        invisible = 1;
                        break;
                    }
                }
            }
            if (!invisible) safe[i] = 1;
        }
    }
    /* ("DUMP THE TOOL'S OWN
     * ADDRESSES, DO NOT RE-DERIVE THEM"). Every discrepancy chased in
     * turned out to be between a Python REPRODUCTION of this
     * function and the function itself -- never between the function and the
     * device. Re-deriving the rule in an analysis script is how
     * invented a missing source and scanned 21 lines where the code
     * scans 24.
     *
     *     XC73_DUMP_LINES=1 xc7300dump decode f.jed out 2>&1 >/dev/null
     *
     * prints, once per consumer block, the line set this function actually
     * collected and the rank -> line mapping every caller will use. */
    {   static int shown[16];
        const char *dbg = getenv("XC73_DUMP_LINES");
        if (dbg && *dbg && fb >= 1 && fb < 16 && !shown[fb]) {
            shown[fb] = 1;
            fprintf(stderr, "LINEMAP FB%-2d nline=%d collected=%d :", fb, nline, n);
            for (i = 0; i < n; i++)
                fprintf(stderr, " %d%s", seen[i], safe[i] ? "" : "(unsafe)");
            fprintf(stderr, "\n");
            for (i = 0; i < n; i++)
                fprintf(stderr, "        rank %-2d -> line %d%s\n", i, seen[i],
                        safe[i] ? "" : "  -- REFUSES");
        }
    }
    if (rank < 0 || rank >= n) return -1;
    /* A real, reserved-but-unsafe line landed exactly on the
     * requested rank -- -2 tells the caller to refuse, distinct from -1
     * ("no such rank at all"), which the caller may still treat as
     * "nothing to resolve" for a slot the fitter itself never used. */
    return safe[rank] ? seen[rank] : -2;
}

/* expose the FAST/High-Density boundary so the decoder's refusal can
 * name the real cause. spent a full investigation rediscovering
 * because the message blamed the UIM, which decodes correctly. */
int fast_upto_of(const JedFile *jf) {
    const DevProfile *p = profile_for(jf);
    return p ? p->fast_upto : 0;
}

int uim_sources_for_line(const JedFile *jf, int fb, int line,
                         int *slots, int cap) {
    /* MUTATION CONTROL for the set-vs-row question (xc2c §106).
     * `XC73_SWAP_UIM=a,b` exchanges the sources of UIM lines a and b -- a
     * PERMUTATION WITHIN THE SET, invisible to any set-based check. If the
     * ROUND CYCLE cannot see it either, then 656 passing designs say nothing
     * about UIM line assignment.
     *
     * The first attempt put this in `uim_source_for_line`, which has ZERO
     * callers -- the mutation changed nothing and would have "proved" the
     * decode insensitive to a permutation it never received. */
    {   const char *sw = getenv("XC73_SWAP_UIM");
        if (sw) { int a=-1,b=-1; if (sscanf(sw,"%d,%d",&a,&b)==2) {
            if (line==a) line=b; else if (line==b) line=a; } }
    }

    static const int uim_bank_lo[3] = { 32, 40,  48 };
    static const int uim_bank_hi[3] = { 96, 104, 112 };
    /* XC73108 has SIX banks where every smaller device has three.
     * The Application Guide (p92) gives the line count -- 24 signals to each
     * FAST block, 21 plus 3 fast inputs to each High-Density block -- and at
     * FOUR lines per offset that is exactly six. The record scan
     * found six low offsets and six high, at stride 4. */
    static const int uim6_lo[6] = { 32, 36, 40,  44,  48,  52 };
    static const int uim6_hi[6] = { 96, 100, 104, 108, 112, 116 };
    const DevProfile *P_ = profile_for(jf);
    /* XC73108's crosspoint is NOT the (bank, half) form the smaller
     * devices use. A source is named by (offset, bit) at blocks 10/21/22, and
     * The LINE is the consumer's allocation order, not part of the address.
     * So enumerate every macrocell that IS connected, and hand
     * them back in a stable order; the caller assigns them to slots.
     *
     * The slot returned encodes (fb, mc) directly, so `uim_slot_decode_dev`
     * needs no table for this device. */
    if (P_->row_per_record) {
        int sfb, smc, n = 0;
        /* An HD block has 21 UIM inputs, not 24. Lines 21,
         * 22 and 23 do not exist and their bits sit at a constant 1, so EVERY
         * source appeared to claim them and the caller saw a 12-source
         * wired-AND on three phantom lines. */
        if (fb > P_->fast_upto && line >= 21) return 0;
        /* `line` arrives as a ROW SLOT -- a RANK over the block's
         * distinct signals, not a line number. Resolve it. */
        {   int real_ = x108_line_of_rank(jf, P_, fb, line);
            /* -2 means this rank IS a real line, but the only
             * claimant(s) are a gated FAST source or an ambiguous
             * multi-claim -- refuse rather than fall through
             * with `line` unchanged, which is a GUESS (the raw row slot
             * treated as if it were already a real line number) that
             * happened to look right on 2 of 3 affected ranks in
             * The reproducer and silently wrong on the third. */
            if (real_ == -2) return 0;
            if (real_ >= 0) line = real_;
        }
        for (sfb = 1; sfb <= P_->nfb; sfb++)
            for (smc = 1; smc <= 9; smc++) {
                if (n >= cap) return n;
                /* The gate used to require the crosspoint to
                 * appear in ALL THREE replica blocks. Measurement showed that a
                 * single logical crosspoint is REPLICATED and that reading ANY
                 * ONE of the replicas suffices. On a SPARSE design all three
                 * agree so  was harmless; on a 100%-FULL XC73108 the
                 * sources feeding lines 6, 7 and 8 report ONE replica and the
                 * gate discarded every one of them. */
                /* `x108_uim_connected`'s blocks-10/21/22 record
                 * never carries a bit for a FAST-block source --
                 * this gate silently drops every real FAST source, and a
                 * HD consumer reading one (e.g. a product-term clock fed
                 * by a FAST-block node) gets zero members for a line that
                 * genuinely has one. Minimal reproducer:
                 * a test design.
                 * unlocked all NINE macrocells here on
                 * per-macrocell evidence (matched-pair differentials,
                 * a an equation-level check against the fitter's .EQN sweep, seven individual
                 * round cycles, all clean in isolation).
                 * PARTIALLY WITHDRAWS that: K63, the one
                 * design that combines several unlocked FAST sources
                 * at once, round-cycles WRONG (14 of 19 pins) with all
                 * nine live -- see the full account in the matching
                 * comment in `x108_line_of_rank`. Back to MC1/MC6 only,
                 * v2.34's original, until combination evidence exists
                 * for the rest. */
                {   int conn = x108_uim_connected(jf, P_, sfb, smc);
                    if (conn < 1 && P_->nfb >= 6 && sfb <= P_->fast_upto &&
                        (smc == 1 || smc == 6))
                        conn = 1;
                    if (conn < 1) continue;
                }
                /* was `x108_slot_of(...) != line` -- a query of the
                 * shape "which ONE line does this source claim?". A source
                 * claims SEVERAL lines (bit 16*slot+fbbit is programmed in
                 * more than one band), so that helper returned the FIRST and
                 * every other line resolved to nothing. Ask the right
                 * question instead: does this source claim THIS line? */
                if (!x108_claims_line(jf, P_, sfb, smc, fb, line)) continue;
                /* (the positional pair rule was REMOVED here.) It existed
                 * only because the old block map put TWO macrocells in one
                 * record with no way to tell them apart, so the decoder had to
                 * guess the member from the order of the lines a block claims.
                 * With the corrected map the BANK names the macrocell and the
                 * claim is already exact -- keeping the guess would discard
                 * correct members. Rule 0ae: a corrected addressing rule
                 * orphans its workarounds. */
                slots[n++] = X108_SLOT(sfb, smc);
            }
        /* PQ100's dedicated-pad ADDRESS is now
         * measured -- `x108_pad_block_pq100`/`x108_pad_select_pq100`,
         * above, correctly name WHICH pad feeds WHICH consumer function
         * block (12 of 12 points confirmed). NOT WIRED IN HERE: a first
         * attempt matched unconditionally for every `line` this loop is
         * asked about (the measured field carries no per-LINE information
         * the way an ordinary macrocell crosspoint claim does), so it
         * claimed the SAME pad for every literal slot in the consumer's
         * block -- corrupting resolution of that block's OTHER inputs
         * (a design with `Z = I1*C1` decoded to `I1*I1`, C1's own literal
         * silently replaced). A follow-up probe (`PQMIX.PLD`, a pad
         * ALONGSIDE a genuine ordinary source in the same block) did not
         * cleanly resolve which rank/line the pad's field corresponds to
         * within one attempt -- needs its own dedicated investigation.
         * Reverted rather than ship a decoder that corrupts a literal it
         * used to at least correctly refuse (this project's own lesson,
         * applied to itself). */
        {   /* input pads claim slots at their own pad-slot address */
            static const int POFF[6] = { 96, 100, 104, 108, 112, 116 };
            int pin, bit = 16 * (line % 4) + P_->cfg_fbbit[fb - 1];
            int band = line / 4;
            if (bit >= 0 && bit <= 63 && band < 6)
                for (pin = 1; pin <= 84 && n < cap; pin++) {
                    int blk, off;
                    if (!x108_pad_addr(jf, pin, &blk, &off)) continue;
                    /* The pad's own offset selects the pad WITHIN its block;
                     * The line's band selects which record */
                    if (off != 32 && off != 96) continue;
                    if (jed_owned(jf, (size_t)blk,
                                  (off == 32) ? 32 + 4 * band : POFF[band],
                                  bit / 8, bit % 8, 0))
                        slots[n++] = X108_PADSLOT(pin);
                }
        }
        return n;
    }
    {
    const int *BLO_ = P_->row_per_record ? uim6_lo : uim_bank_lo;
    const int *BHI_ = P_->row_per_record ? uim6_hi : uim_bank_hi;
    const int NBANK_ = P_->row_per_record ? 6 : 3;
    /* XC73108 packs FOUR lines per offset in 4 groups of 16, where
     * every smaller device packs EIGHT in 8 groups of 8 -- its records are 64
     * bits over 12 blocks plus padding. Same traversal, same
     * source-in-the-block, three parameters. The "different traversal"
     * followed from the withdrawn reading. */
    int LPO_ = P_->row_per_record ? 4 : 8;      /* lines per offset */
    int GMU_ = P_->row_per_record ? 16 : 1;     /* group multiplier   */
    int blk, bit = fb_bit_p(P_, fb);
    int bank = line / LPO_, grp = line % LPO_, half, n = 0;
    if (bit < 0 || bank >= NBANK_) return 0;
    /* hardcoded 64..125 is XC7336's UIM extent. XC7354 runs to 239,
     * and the profile has carried the right numbers --
     * this loop just never read them. */
    for (blk = profile_for(jf)->uim_blk_lo; blk <= profile_for(jf)->uim_blk_hi; blk++)
        for (half = 0; half < 2; half++) {
            int off = half ? BHI_[bank] : BLO_[bank];
            int cand, cb, co;
            if (!jed_owned(jf, (size_t)blk, off, grp * GMU_, bit, 0)) continue;
            /* The slot base was the literal 64 -- XC7336's
             * `uim_blk_lo`, which every device since has ALSO had, so the
             * constant and the profile field agreed by coincidence. XC73108's
             * UIM starts at block 2 and `2*(blk-64)` goes NEGATIVE. Reading
             * the field changes NOTHING for the four measured devices. */
            /* SUPERSEDES: the source IS in the block on
             * every device, XC73108 included. The traversal is correct once
             *  and  are set. */
            cand = 2 * (blk - P_->uim_blk_lo) + half;
            /* The "does this slot name anything" test was XC7336's.
             * On XC7354 the bands are 16*B and 64+16*B with B,
             * so valid slots were being discarded and every literal fed by a
             * macrocell output failed to resolve. */
            {
                int f2, m2, io2;
                if (!uim_slot_decode_dev(jf, cand, &f2, &m2, &io2)) continue;
            }
            (void)cb; (void)co;
            if (n < cap) slots[n] = cand;
            n++;
        }
    return n;
    }
}

/* XC7354 has its OWN slot rule -- one band index B per
 * function block, pin band 16*B+(mc-1), macrocell band 64+16*B+((mc+8)%12).
 * `decode` was calling the XC7336 version for every device. */
/* THIRD slot-decoding path (after `slot_name_7354` and
 * `pin_of_slot_dev`), and the one `uim_sources_for_line` gates on. It knew
 * only XC7354's bands, so every XC7372 line fed by a MACROCELL OUTPUT was
 * discarded and the decoder reported "the UIM does not resolve this line to a
 * source" on every dense design -- while `uim` printed the very same slot
 * correctly, because that path HAD been taught. One address, three readers,
 * two taught: exactly. */
int uim_slot_decode_dev(const JedFile *jf, int slot, int *srcfb, int *srcmc,
                        int *isout) {
    const DevProfile *p = profile_for(jf);
    /* XC73108's slot carries (fb, mc) directly -- see
     * `uim_sources_for_line`. No band table is involved. */
    if (X108_IS_PAD(slot)) {       /* A dedicated/IO input pad */
        *srcfb = 0; *srcmc = 0; *isout = 0;
        return 1;
    }
    if (X108_IS_SLOT(slot)) {
        *srcfb = X108_SLOT_FB(slot);
        *srcmc = X108_SLOT_MC(slot);
        *isout = 1;
        return 1;
    }

    if (strstr(jf->device, "XC7372")) {
        int f2, m2; size_t k;
        /* PC68 has its OWN dedicated-input table
         * (`xc7372_inpin_pc68`, -- a different 8-of-12-slot
         * subset than PC84's (`xc7372_inpin`) bonded to different
         * physical pins. Checking the wrong table for a package risks
         * naming a pad that package does not actually have wired to
         * that slot; PC68 is checked first and explicitly. */
        if (strstr(jf->device, "PC68")) {
            for (k = 0; k < sizeof xc7372_inpin_pc68 / sizeof xc7372_inpin_pc68[0]; k++)
                if (xc7372_inpin_pc68[k].slot == slot) {
                    *srcfb = 0; *srcmc = 0; *isout = 0; return 1;
                }
        } else
        for (k = 0; k < sizeof xc7372_inpin / sizeof xc7372_inpin[0]; k++)
            if (xc7372_inpin[k].slot == slot) {
                *srcfb = 0; *srcmc = 0; *isout = 0; return 1;
            }
        /* this slot is a KNOWN dedicated-input pad on
         * SOME package of this device (xc7372_dedin_slots) but wasn't in
         * THIS package's own table above -- an unmeasured pad, not a
         * genuine reference to xc7372_slot_cell/xc7372_pad_cell's own
         * macrocell-pin-band position, even though those functions'
         * numeric ranges happen to overlap this slot (the
         * documented overlap). Falling through resolved it anyway,
         * silently substituting a DIFFERENT real pin: P68X65,
         * slot 34 -> pin 60 instead of the design's real 65). Refuse. */
        if (slot_in_set(slot, xc7372_dedin_slots,
                         sizeof xc7372_dedin_slots / sizeof xc7372_dedin_slots[0]))
            return 0;
        if (xc7372_slot_cell(slot, &f2, &m2)) {
            *srcfb = f2; *srcmc = m2; *isout = 1; return 1;
        }
        if (xc7372_pad_cell(slot, &f2, &m2)) {
            *srcfb = f2; *srcmc = m2; *isout = 0; return 1;
        }
        return 0;
    }
    if (p->nfb >= 6) {
        int fb, b, mc, base;
        size_t k;
        /*, CORRECTED -- an over-gate broke this
         * function for EVERY XC7354 package, discovered within the
         * same session by a minimal 2-macrocell same-block-feedback
         * design refusing on PC84: `FB3_MC1`/`FB3_MC7`, no dedicated
         * input involved at all). This block does TWO jobs: identify a
         * DEDICATED-INPUT PAD slot (`xc7354_inpin`, PC68-pin-numbered,
         * package-specific) and identify an ORDINARY MACROCELL UIM
         * source by its crosspoint SLOT NUMBER (the band loop and
         * `xc7354_fb6` below -- a property of the silicon die's
         * crossbar, not of which package pin a macrocell happens to be
         * bonded to). Only the FIRST job is package-specific; gating
         * the whole block broke the second for every package. Gate
         * only the dedicated-input lookup. */
        /* THE DEDICATED-INPUT TABLE WAS NEVER CONSULTED HERE.
         * Measurement showed eight dedicated input pads at slots 32, 33, 34,
         * 35, 37, 48, 49, 50. `pin_of_slot_dev` has looked them up since
         * and `slot_name_7354` -- this function, which
         * is the VALIDITY GATE for `uim_sources_for_line` and `uim_node_lit`,
         * did not. Slots 32..35 and 37 passed only by accident, because FB3's
         * pin-band rule `16*B+(mc-1)` covers 32..40; slots 48, 49 and 50 fall
         * in FB6's band region, which is skipped by rule (FB6 is a table) and
         * whose table starts at 51, so they were REJECTED.
         *
         * That rejection is the whole of the "three UIM lines per
         * High-Density block that carry no crosspoint" and the "four
         * into FB6". The crosspoints are there and `uim` prints them; the
         * counts 3 and 4 are an artefact of WHICH BLOCK the design sat in,
         * because FB3's band rule absorbs its dedicated inputs and FB6's does
         * not. Nothing about the device is involved.
         *
         * A dedicated input has no macrocell behind it, so it reports
         * srcfb = srcmc = 0 with isout = 0: a PAD source, resolved to a
         * package pin by `pin_of_slot_dev`. Checked FIRST, because on this
         * package a dedicated input's slot may also satisfy a macrocell
         * pin-band rule and the measured table is the authority. */
        /* PC44/PC84 each have their OWN dedicated-input
         * table -- a 6-of-8-slot subset of PC68's own 8-slot
         * space, bonded to different physical pins per package. */
        if (strstr(jf->device, "PC68")) {
            for (k = 0; k < sizeof xc7354_inpin / sizeof xc7354_inpin[0]; k++)
                if (xc7354_inpin[k].slot == slot) {
                    *srcfb = 0; *srcmc = 0; *isout = 0; return 1;
                }
        } else if (strstr(jf->device, "PC44")) {
            for (k = 0; k < sizeof xc7354_inpin_pc44 / sizeof xc7354_inpin_pc44[0]; k++)
                if (xc7354_inpin_pc44[k].slot == slot) {
                    *srcfb = 0; *srcmc = 0; *isout = 0; return 1;
                }
        } else if (strstr(jf->device, "PC84")) {
            for (k = 0; k < sizeof xc7354_inpin_pc84 / sizeof xc7354_inpin_pc84[0]; k++)
                if (xc7354_inpin_pc84[k].slot == slot) {
                    *srcfb = 0; *srcmc = 0; *isout = 0; return 1;
                }
        }
        /* this slot is a KNOWN dedicated-input pad on
         * SOME package of this device (xc7354_dedin_slots, from PC68's
         * own full table) but wasn't in THIS package's own table above
         * -- an unmeasured pad, not a genuine macrocell pin-band
         * reference, even though the fb-band loop below's numeric range
         * happens to overlap it. Falling through resolved it anyway,
         * right by accident on PC44 ("no package pin") and silently
         * WRONG on PC84: P84X53/P84X81, slots 32/33 -> pins
         * 77/79 instead of the design's real 53/81). Refuse instead. */
        if (slot_in_set(slot, xc7354_dedin_slots,
                         sizeof xc7354_dedin_slots / sizeof xc7354_dedin_slots[0]))
            return 0;
        for (fb = 1; fb <= 6; fb++) {
            b = xc7354_band(fb);
            if (b < 0) continue;
            base = 16 * b;
            if (fb != 6 && slot >= base && slot <= base + 8) {
                *srcfb = fb; *srcmc = slot - base + 1; *isout = 0; return 1;
            }
            base = 64 + 16 * b;
            for (mc = 1; mc <= 9; mc++)
                if (base + ((mc + 8) % 12) == slot) {
                    *srcfb = fb; *srcmc = mc; *isout = 1; return 1;
                }
        }
        {   size_t k;
            for (k = 0; k < sizeof xc7354_fb6 / sizeof xc7354_fb6[0]; k++)
                if (xc7354_fb6[k].slot == slot) {
                    *srcfb = xc7354_fb6[k].fb; *srcmc = xc7354_fb6[k].mc;
                    *isout = 0; return 1;
                }
        }
        return 0;
    }
    return uim_slot_decode(slot, srcfb, srcmc, isout);
}

int uim_slot_decode(int slot, int *srcfb, int *srcmc, int *isout) {
    static const int fb_of_idx[4] = { 1, 3, 2, 4 };
    if (slot >= 64) {
        int k = slot - 64, band = k / 16, o2 = k % 16;
        if (band > 3 || o2 >= 12) return 0;
        *srcfb = fb_of_idx[band];
        *srcmc = (o2 >= 9) ? (o2 - 8) : (o2 + 4);
        *isout = 1;
    } else {
        int band = slot / 16, m = (slot % 16) + 1;
        if (band > 3 || m > 9) return 0;
        *srcfb = fb_of_idx[band]; *srcmc = m; *isout = 0;
    }
    return 1;
}

int uim_source_for_line(const JedFile *jf, int fb, int line,
                        int *slot, int *srcfb, int *srcmc, int *isout) {
    static const int fb_of_idx[4] = { 1, 3, 2, 4 };
    /* THE UIM LINE AXIS IS THREE BANKS OF EIGHT, NOT ONE OF EIGHT.
     *
     *     line = 8*bank + group      bank 0 = offsets (32, 96)
     *                                bank 1 = offsets (40,104)
     *                                bank 2 = offsets (48,112)
     *     slot = 2*(block-64) + (offset is a LOW offset ? 0 : 1)
     *
     * 3 banks x 8 lines = 24, exactly the 24 inputs the datasheet gives an
     * FFB. Measurement showed as a ZERO that offsets 40, 48, 104 and 112
     * "NEVER carry a source". Across 246 fitter-produced bitmaps that is
     * true -- and it is true because NO DESIGN IN THAT CORPUS EVER NEEDED
     * MORE THAN EIGHT SOURCES INTO ONE FUNCTION BLOCK, so banks 1 and 2 were
     * always empty. The highest UIM line and the highest row slot in all 246
     * are both 7.
     *
     * The first REAL chip needed seventeen into FB3. Its thirteen bits at
     * those offsets map onto exactly the thirteen line indices its rows
     * referenced and the UIM could not supply -- 8..11 in FB1 and 8..16 in
     * FB3 -- with no line left over and no line given two sources. */
    static const int uim_bank_lo[3] = { 32, 40,  48 };
    static const int uim_bank_hi[3] = { 96, 104, 112 };
    int blk, bit = fb_bit_p(profile_for(jf), fb);
    int bank = line / 8, grp = line % 8, half;
    int nfound = 0, nnamed = 0, best = -1;
    if (bit < 0 || bank > 2) return 0;

    /* COLLECT EVERY CROSSPOINT ON THIS LINE, THEN DECIDE.
     *
     * This used to take the FIRST block with a bit set. On the fitter's own
     * bitmaps that is always the only one, so it worked -- but a readback
     * carries bits at UIM addresses the fitter never writes, and several
     * blocks can answer for one line. Taking the first then picked whichever
     * block came lowest, which is not a measurement, it is an accident of
     * loop order.
     *
     * A slot that names no macrocell is not a source: slot 10 in the pin band
     * is "FB1_MC11", and there is no MC11. Those are counted and skipped.
     * If exactly ONE crosspoint names a real source, that is the answer. If
     * SEVERAL do, the line is genuinely ambiguous -- the datasheet's
     * SMARTswitch makes a multi-source line legal -- and this reports failure
     * rather than choosing one. */
    /* hardcoded 64..125 is XC7336's UIM extent. XC7354 runs to 239,
     * and the profile has carried the right numbers --
     * this loop just never read them. */
    for (blk = profile_for(jf)->uim_blk_lo; blk <= profile_for(jf)->uim_blk_hi; blk++)
        for (half = 0; half < 2; half++) {
            int off = half ? uim_bank_hi[bank] : uim_bank_lo[bank];
            int cand, cb, co;
            if (!jed_owned(jf, (size_t)blk, off, grp, bit, 0)) continue;
            nfound++;
            cand = 2 * (blk - 64) + half;
            if (cand >= 64) {
                cb = (cand - 64) / 16; co = (cand - 64) % 16;
                if (cb > 3 || co >= 12) continue;         /* names nothing */
            } else {
                cb = cand / 16; co = (cand % 16) + 1;
                if (cb > 3 || co > 9) continue;           /* names nothing */
            }
            nnamed++; best = cand;
        }
    if (nnamed != 1) return 0;
    {
        *slot = best;
            if (*slot >= 64) {
                int k = *slot - 64, band = k / 16, o2 = k % 16;
                if (band > 3 || o2 >= 12) return 0;
                *srcfb = fb_of_idx[band];
                *srcmc = (o2 >= 9) ? (o2 - 8) : (o2 + 4);
                *isout = 1;
            } else {
                int band = *slot / 16, m = (*slot % 16) + 1;
                if (band > 3 || m > 9) return 0;
                *srcfb = fb_of_idx[band]; *srcmc = m; *isout = 0;
            }
        return 1;
    }
}


/* SLOT -> PACKAGE PIN, per device. `decode.c`'s `pin_of_slot` used an
 * XC7336-only table, so on XC7354 every literal came out on the wrong pin --
 * the design was logically correct and the round cycle failed on PIN SETS
 * DIFFER. The XC7354 answer is the rule inverted, plus the
 * dedicated-input table. */
int pin_of_slot_dev(const JedFile *jf, int slot) {
    /* XC73108 pad slots carry the pin directly. */
    if (X108_IS_PAD(slot)) return X108_PAD_PIN(slot);
    /* XC7372 has its OWN dedicated-input table. `slot_name_7354`
     * consulted it and this function did not, so `uim` printed the right pin
     * while the EMITTER wrote the wrong one and the round cycle failed on
     * PIN SETS DIFFER. Second code path, same table -- exactly. */
    {   char fam[16], pkg[16];
        split_part(jf->device, fam, sizeof fam, pkg, sizeof pkg);
    if (strstr(jf->device, "XC7372")) {
        /* A later change fixed the hardcoded-"PC84" defect for
         * PC84 itself; measurement showed PC68's OWN dedicated-
         * input table (`xc7372_inpin_pc68`, and extends this
         * function to it, using the REAL package throughout (not a
         * hardcoded literal) so `pin_for_macrocell` resolves the correct
         * package's own pin table for the ordinary-macrocell case too. */
        if (strcasecmp(pkg, "PC84") != 0 && strcasecmp(pkg, "PC68") != 0)
            return 0;
        {   size_t k; int f2, m2;
            if (strcasecmp(pkg, "PC68") == 0) {
                for (k = 0; k < sizeof xc7372_inpin_pc68 / sizeof xc7372_inpin_pc68[0]; k++)
                    if (xc7372_inpin_pc68[k].slot == slot) return xc7372_inpin_pc68[k].pin;
            } else
            for (k = 0; k < sizeof xc7372_inpin / sizeof xc7372_inpin[0]; k++)
                if (xc7372_inpin[k].slot == slot) return xc7372_inpin[k].pin;
            if (xc7372_slot_cell(slot, &f2, &m2))
                return pin_for_macrocell(jf->device, pkg, f2, m2);
            if (xc7372_pad_cell(slot, &f2, &m2))
                return pin_for_macrocell(jf->device, pkg, f2, m2);
        }
        return 0;
    }
    const DevProfile *p = profile_for(jf);
    /* A later change fixed the hardcoded-"PC68" defect for PC68
     * itself; measurement showed PC44's and PC84's OWN dedicated-
     * input tables (`xc7354_inpin_pc44`/`xc7354_inpin_pc84`, and
     * extends this function to them, using the REAL package throughout. */
    if (p->nfb == 6 && (strcasecmp(pkg, "PC68") == 0 ||
                        strcasecmp(pkg, "PC44") == 0 ||
                        strcasecmp(pkg, "PC84") == 0)) {
        size_t k;
        int fb, b, mc, base;
        if (strcasecmp(pkg, "PC44") == 0) {
            for (k = 0; k < sizeof xc7354_inpin_pc44 / sizeof xc7354_inpin_pc44[0]; k++)
                if (xc7354_inpin_pc44[k].slot == slot) return xc7354_inpin_pc44[k].pin;
        } else if (strcasecmp(pkg, "PC84") == 0) {
            for (k = 0; k < sizeof xc7354_inpin_pc84 / sizeof xc7354_inpin_pc84[0]; k++)
                if (xc7354_inpin_pc84[k].slot == slot) return xc7354_inpin_pc84[k].pin;
        } else
        for (k = 0; k < sizeof xc7354_inpin / sizeof xc7354_inpin[0]; k++)
            if (xc7354_inpin[k].slot == slot) return xc7354_inpin[k].pin;
        for (k = 0; k < sizeof xc7354_fb6 / sizeof xc7354_fb6[0]; k++)
            if (xc7354_fb6[k].slot == slot)
                return pin_for_macrocell(jf->device, pkg,
                                         xc7354_fb6[k].fb, xc7354_fb6[k].mc);
        for (fb = 1; fb <= 5; fb++) {
            b = xc7354_band(fb);
            if (b < 0) continue;
            base = 16 * b;
            if (slot >= base && slot <= base + 8) {
                mc = slot - base + 1;
                return pin_for_macrocell(jf->device, pkg, fb, mc);
            }
        }
        return 0;
    }
    if (p->nfb == 6) return 0;   /* XC7354 on a package other than PC44/PC68/PC84 */
    return pin_of_slot_7336(slot);
    }
}

int pin_for_macrocell(const char *device, const char *pkg, int fb, int mc) {
    const device_t *d = device_find(device);
    int p, f2, m2;
    if (!d) d = device_find("XC7336");
    /* The same 44 bound as above, in a second function. PC68 and
     * PC84 pads above 44 were unreachable, so every macrocell on them looked
     * like it had no pin and the emitter buried it. */
    for (p = 1; p <= npins_of(pkg); p++)
        if (device_pin_macrocell(d, pkg, p, &f2, &m2) && f2 == fb && m2 == mc)
            return p;
    return 0;
}

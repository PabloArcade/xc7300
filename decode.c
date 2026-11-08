/* decode.c -- turn an XC7336/XC7318 bitmap back into a PLUSASM design.
 *
 * This is the decompiler proper: `.jed` in, `.PLD` + `.VMF` out, which XEPLD
 * recompiles into a functionally identical bitmap on identical pins -- the
 * ROUND CYCLE, the proof standard this tool is held to.
 *
 * Ported from the prototype that passed 39 of 40 round cycles.
 * Every rule below is measured, not assumed.
 *
 * WHAT IT CANNOT RECOVER, and says so rather than guessing:
 *   - SIGNAL NAMES. A bitmap holds no names. An input on package pin 7 is
 *     emitted as `I_7`, a macrocell output as `N_<fb>_<mc>`. The round cycle
 *     compares BEHAVIOUR AT PINS, so names are free to differ.
 *   - anything outside XC7336/XC7318; the caller guards the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp, */
#include "jed.h"
#include "device.h"
#include "decode.h"

/* MEASURED band-1 slot -> PC44 pin. One fit per pin, no inference.
 * The macrocell rule alone is WRONG for slots 24 and 32: those belong to the
 * dedicated input pins 42 and 28, because FB3_MC9's and FB2_MC1's own pins are
 * FOE0/FOE1 and cannot be used as ordinary inputs at all. */
static const struct { int slot, pin; } slotpin[] = {
    { 0, 7},{ 1, 8},{ 2, 9},{ 3,11},{ 4,12},{ 5,13},{ 6,14},{ 7,15},{ 8,16},
    {18, 4},{19, 3},{20, 2},{22,44},{23,43},{24,42},
    {32,28},{33,38},{34,37},{35,36},{36,35},{37,34},{38,33},{39,30},{40,29},
    {48,27},{49,26},{50,25},{51,24},{52,22},{53,20},{54,19},{55,18},{56,17},
};

int pin_of_slot_7336(int slot) {
    size_t i;
    for (i = 0; i < sizeof slotpin / sizeof slotpin[0]; i++)
        if (slotpin[i].slot == slot) return slotpin[i].pin;
    return 0;
}

typedef struct {
    int fb, mc;
    int used, buried, reg, inv, split, clk, oe_used, oe_src, toggle, async_is_set;
    int alloc_hi;             /* blk18: 1 = logic in rows 1..4, row0 is async */
    int async_present;        /* XC7354: blk41/42 say an async term EXISTS */
    int async_row;            /* which row holds it, -1 = none      */
    int async_row_is_logic;   /* exporting repurposes it */
    /* blk41/42 = (0,0) is a FOURTH, previously
     * unhandled state. (1,1) none, (1,0) RESET, (0,1) SET were the only
     * three named -- (0,0) means BOTH a dedicated SET (row0)
     * AND a dedicated RESET (row1) are present on the SAME macrocell
     * simultaneously, confirmed against the real fitter's own .EQN (which
     * prints both .SETF and .RSTF) on a design built specifically to
     * combine them. `async_row`/`async_term` stay row0/SET as before;
     * these two carry row1/RESET only when `async_both` is set. */
    int async_both;
    int async_row2;
    char async_term2[256];
    int lowpwr;
    int localfb;              /* blk16: feeds a neighbour in THIS block  */
    int ptclk;                /* clock is a PRODUCT TERM, row 3 */
    int constval;             /* -1 none, 0 = GND, 1 = VCC */
    char clk_term[256];       /* that term, when ptclk                   */
    int localfb_known;        /* is that block LOCATED on this device? */
    int drives_uim;           /* output appears as a UIM crosspoint */               /* blk17: 1 = PWR LOW */
    char async_term[256];
    int nterm;
    /* this was `term[16]`, and BOTH ways a macrocell can exceed its
     * five rows overrun it. A four-deep EXPORT CHAIN assembles 17 terms from
     * five FAST macrocells Class B), and a High-Density macrocell can
     * hold 5 private + 12 shared = 17. A FAST block can in principle
     * chain all nine of its macrocells at five rows each, so 45; 64 is that
     * with headroom. */
    char term[64][256];   /* export merges a neighbour's rows in */
} Cell;

/* An EXPORTER is a used, COMBINATORIAL macrocell whose neighbour
 * M+1 is used and registered. Its rows belong to the neighbour's equation,
 * because the datasheet transfers product terms to the NEIGHBOURING
 * macrocell. Merge, do not emit it as a second output. */

/* name a WIRED-AND UIM line as a `NODE (UIM)` and return the
 * literal to use for it. One node per (function block, line), reused if the
 * same line appears again. */
typedef struct { int fb, line; char name[24]; char expr[256]; } UimNode;

/* 16 blocks x 21 lines on the largest part, plus margin. */
#define UIM_NODE_MAX 384

static int uim_node_lit(const JedFile *jf, int fb, int line,
                        const int *sl, int nsrc, int neg,
                        Cell cell[13][10], char sig[13][10][32],
                        char *out, size_t cap, UimNode *tab, int *ntab,
                        int *uses_bonded_member,
                        int *seen_in, int *nin) {
    int node_bonded = 0, node_invpad = 0;
    int i, k;
    char e[256];
    for (i = 0; i < *ntab; i++)
        if (tab[i].fb == fb && tab[i].line == line) {
            snprintf(out, cap, "%s%s", neg ? "/" : "", tab[i].name);
            return 1;
        }
    /* this cap was 32 and it is the LAST thing standing between a
     * saturated XC73108 and a clean decode -- with the addressing fixed
     * every line resolves, and then the node table overflows
     * and eight lines per design report "could not be named".
     *
     * Rule 7aa: when you raise a cap, ask what the NEXT device needs. A UIM
     * node exists per (function block, wired-AND line). The worst case is
     * every block using every one of its lines as a wired-AND node:
     *     XC73108   12 blocks x 21 lines = 252
     *     XC73144   16 blocks x 21 lines = 336
     * 384 covers both with margin. This is the fourth cap in this tool that
     * silently excluded the larger part `decode_nfb` 6->8->12). */
    if (*ntab >= UIM_NODE_MAX) return 0;
    e[0] = '\0';
    for (k = 0; k < nsrc; k++) {
        int sfb, smc, isout, pin;
        char one[40];
        /* this called the XC7336 slot rule, so on XC7354 every
         * wired-AND source failed to decode and the whole line was
         * refused -- the last of the five assumptions */
        if (!uim_slot_decode_dev(jf, sl[k], &sfb, &smc, &isout)) {
            fprintf(stderr, "      source %d of %d: slot %d does not decode\n",
                    k + 1, nsrc, sl[k]);
            return 0;
        }
        if (isout) {
            if (!cell[sfb][smc].used) {
                /* An honest failure beats a wrong name: the UIM says this
                 * macrocell drives the line and the configuration says the
                 * macrocell is unused. One of the two readings is wrong and
                 * the decoder must not pick. */
                fprintf(stderr, "      source %d of %d: FB%d_MC%d claims the "
                        "line but is not marked used\n", k + 1, nsrc, sfb, smc);
                return 0;
            }
            /* A BONDED macrocell cannot carry its inversion in
             * blk21..24 the way a buried one does -- flipping that
             * field would flip what the PIN drives. So the fitter inverts its
             * LINE instead, exactly as it does for a pad, and the table in
             * covers DEDICATED INPUTS only.
             *
             * `D71`'s `UIM_7 = /P80 * /O_1_5` is this: `O_1_5` drives pin 56,
             * and pin 56 is the one output that still differed after the pad
             * correction. Arm the guard rather than emit it. */
            /* A BONDED macrocell that is INVERTED. The fitter gives
             * the UIM `/OB` through blk21..24, but the PIN must still drive
             * `OB`, so something re-inverts at the pad -- and this decoder
             * does not read it. Measured: `HAN` and `HBN`, whose only
             * difference from their positive twins is `U1 = /OB * /S4_2`,
             * each fail at EXACTLY the pin their bonded member drives (56 and
             * 57). `D71`'s pin 56 is the same shape.
             *
             * The guard required an inverted PAD member as well, so
             * it let both through and an earlier version emitted them. Arm on the bonded
             * member being inverted, whether or not a pad is involved. */
            if (!cell[sfb][smc].buried && cell[sfb][smc].inv) node_bonded = 1;
            if (cell[sfb][smc].split)
                snprintf(one, sizeof one, "/%s", sig[sfb][smc]);
            else
                snprintf(one, sizeof one, "%s", sig[sfb][smc]);
        } else {
            pin = pin_of_slot_dev(jf, sl[k]);
            if (!pin) {
                fprintf(stderr, "      source %d of %d: slot %d is a pad with "
                        "no pin\n", k + 1, nsrc, sl[k]);
                return 0;
            }
            /* The SAME correction the ordinary rows get. A member
             * of a wired-AND node is read from the same inverted line, so it
             * needs `literal = column XOR pad_inversion(pin)` too. Applying
             * it only to ordinary rows left `D48` at one differing pin and
             * `D71` at two -- both on the output fed by the node itself. */
            {   int pinv = x108_pad_inverted(jf, pin);
                if (pinv > 0) { node_invpad = 1;
                                snprintf(one, sizeof one, "/I_%d", pin); }
                else            snprintf(one, sizeof one, "I_%d", pin);
            }
            /* A package pin reached ONLY through a UIM wired-AND
             * node was written into the node's equation but never added to
             * `seen_in`, so it was never DECLARED. A design whose inputs all
             * arrive that way emitted a bare `INPUTPIN` with nothing after it
             * -- `cl268`, syntax error, no fit. Every dense `gt_ba_*` design
             * is exactly that shape: eight three-input wired-AND nodes
             * feeding two macrocells, so the ENTIRE input list lived in the
             * UIM nodes and the declaration came out empty. */
            if (pin < 128 && !seen_in[pin]) { seen_in[pin] = 1; (*nin)++; }
        }
        if (k) strncat(e, "*", sizeof e - strlen(e) - 1);
        strncat(e, one, sizeof e - strlen(e) - 1);
    }
    snprintf(tab[*ntab].name, sizeof tab[0].name, "U_%d_%d", fb, line);
    snprintf(tab[*ntab].expr, sizeof tab[0].expr, "%s", e);
    tab[*ntab].fb = fb; tab[*ntab].line = line;
    /* A node that mixes an INVERTED PAD line with a BONDED
     * macrocell member is the one shape still unhandled. `D71`'s
     * `UIM_7 = /P80 * /O_1_5` is exactly it, and pin 56 -- the pin `O_1_5`
     * drives -- was the single output still wrong after the pad fix. A
     * bonded macrocell cannot carry its inversion in blk21..24 (that would
     * flip what the pin OUTPUTS), so its line is inverted instead and
     * The table does not cover it.
     *
     * Arming on a bonded member ALONE was far too wide -- it refused `D48`,
     * `D60`, `B2` and `K64`, all of which round-cycle. It is the
     * CONJUNCTION. */
    if (node_bonded) *uses_bonded_member = 1;
    snprintf(out, cap, "%s%s", neg ? "/" : "", tab[*ntab].name);
    (*ntab)++;
    return 1;
}


/* CAN THIS MACROCELL'S PACKAGE PIN CARRY AN OUTPUT?
 *
 * FB3_MC6 on PC44 maps to PIN01, and PIN01 is MR. The macrocell exists and can
 * hold logic; the PIN cannot be driven. The emitter assigned an OUTPUTPIN
 * there and PLUSASM rejected the design outright:
 *
 *   pc36:[Error]Port 'N_3_6'(type O) cannot be placed on pin 'PIN01'
 *               (type VCC,VSS or MR).
 *   pc17:[Error]No pins copied.
 *
 * The device table records pin 1 as MR, and the emitter had to
 * INTERPOLATE FB3_MC6's UIM slot for exactly this reason -- the knowledge was
 * present and the emitter did not consult it. Note the chip's own blk12/13
 * reads "permanently ENABLED" for this macrocell: the fitter leaves the
 * output-enable field alone when the output goes nowhere, so the CONFIG WORD
 * cannot be used to detect this. The PACKAGE can.
 *
 * XC7336Q: DOES IT SHARE THIS RESTRICTION? UNTESTABLE, not merely
 * untested (the open item (ii), closed here,.
 * `-p XC7336Q-5PC44` (and every other XC7336Q spelling tried) produces
 * ZERO output from PLUSASM under the installed XACT 5.0.0 -- no `.LOG`,
 * no `.ERR`, no `.PAR`, nothing, matching the toolchain's
 * documented "an unrecognised device string produces NO output, not an
 * error" behaviour exactly. Confirmed directly: `XC7336Q` and its own
 * internal code do not appear anywhere in `xact/X/XACT/DATA/
 * PARTLIST.XCT`. XC7336Q is XACT-6-only in this project's own history
 * (the citation of `model6.chp`, itself absent from this
 * checkout, -- a fresh, working XACT 6.01 install would be
 * needed to ask this question at all, out of this closure's own scope.
 * The shared-die assumption already baked into `vq44_36q_mc`/
 * `pq44_36q_mc`/`pc44_36q_mc` (FB3_MC6 excluded, same as XC7336's own
 * `pc44_mc`/`pq44_36_mc`) is therefore STILL an assumption, not a
 * measurement, for this one device string -- left as it stands. */
static int pin_can_drive(const char *device, const char *pkg, int fb, int mc,
                         int *pinout) {
    const device_t *d = device_find(device);
    int p;
    const char *fn;
    /* PG144/BG225 have no numeric pin at all, so
     * pin_for_macrocell()'s numeric search (below) can never find one and
     * every macrocell on these two packages was silently treated as
     * unbonded -- confirmed by direct test, not assumed. `device_
     * macrocell_ball()` is a COMPLETE 108-of-108 enumeration for XC73108
     * (every macrocell has a real ball; the table carries no MR/GND/VCC
     * entries to filter, unlike the numeric package tables), so a hit
     * there is sufficient on its own -- no `device_pin_function()`
     * special-pin check applies to these two packages because nothing in
     * this table is anything but an ordinary macrocell ball. `pinout` is
     * left at 0 here: the caller's own numeric use of it (a diagnostic
     * PIN%02d note) does not apply to an alphanumeric ball, and the real
     * translation for the `.VMF` writer goes through `device_macrocell_
     * ball()` directly, not through this out-parameter. */
    if (d && (strcasecmp(pkg, "PG144") == 0 || strcasecmp(pkg, "BG225") == 0)) {
        char ball[8];
        if (pinout) *pinout = 0;
        return device_macrocell_ball(d, pkg, fb, mc, ball, sizeof ball);
    }
    p = pin_for_macrocell(device, pkg, fb, mc);
    if (pinout) *pinout = p;
    if (!p || !d) return 0;
    fn = device_pin_function(d, pkg, p);
    if (!fn || !*fn) return 1;                 /* ordinary I/O */
    /* PLUSASM's own words: "cannot be placed on pin (type VCC,VSS or MR)".
     * Those three, and a dedicated INPUT, are the only ones that cannot carry
     * an output. The dual-function pins CAN: pin 1 is I/FO/FI/MR, pin 5 is
     * FO/FCLK0, pin 39 is FO/FOE1 -- all Fast Outputs as well, and a real chip
     * drives PIN05. Only the assignment PLUSASM rejects is
     * excluded, and the first version of this check was too broad: it buried
     * FB2_MC1 and FB3_MC9 as well, whose pins are FOE1 and FOE0 and are
     * perfectly good outputs. */
    if (strcmp(fn, "MR") == 0) return 0;
    if (strncmp(fn, "GND", 3) == 0) return 0;
    if (strncmp(fn, "VCC", 3) == 0) return 0;
    if (strcmp(fn, "I") == 0) return 0;         /* dedicated input */
    return 1;
}


/* function-block count and package, taken from the device table. */
static int decode_nfb(const char *dev) {
    const device_t *d = device_find(dev);
    /* The cap was 6 and XC7372 has EIGHT function blocks, so it
     * silently decoded as a 4-block part. */
    /* The cap was 8 and XC73108 has TWELVE, so it silently decoded
     * as a 4-block part -- the row loop never reached FB7 and the emitter
     * reported "NOT ONE product term resolved (0 dropped)" because no cell
     * it examined had any. THIRD time this constant has excluded the next
     * device: 6 -> 8 -> 12 here. `cell[13][10]` already sizes
     * for twelve. */
    return (d && d->n_fbs > 0 && d->n_fbs <= 12) ? d->n_fbs : 4;
}
static const char *decode_pkg(const char *dev) {
    /* this knew only PC68 and PC44, so an XC7372-*PC84 part
     * resolved as PC44, `pin_for_macrocell` found no pad for any macrocell
     * and the emitter buried ALL of them -- `cl311: No output pin declared`
     * on every refit. Same shape as on XC7318.
     *
     * FIXING THE SYMPTOM LEFT THE CAUSE. Adding
     * "PC84" here cured the two packages someone had hit; every OTHER
     * package this tool's own vendor tables claim to support (PQ100,
     * PQ160, PQ44, VQ44...) still fell through to the "PC44" default --
     * a WRONG, unrelated 44-pin package, not "unknown". On XC73108-
     * PQ160 that made `pin_for_macrocell` consult a package XC73108
     * has no table for at all, so EVERY macrocell looked unbonded and
     * decoded as an internal node instead of an OUTPUTPIN -- confirmed
     * directly on a fully packed, `.MAP`-checked PQ160 design (zero
     * OUTPUTPINs emitted). `split_part` (main.c) already extracts the
     * real package from the device string correctly for every OTHER
     * purpose in this tool (the `info`/target-gate code, --
     * this function had its own, narrower, stale copy of the same job.
     * Rule 4: one address, one place. */
    static char pkg[16];
    char fam[16];
    split_part(dev, fam, sizeof fam, pkg, sizeof pkg);
    return pkg[0] ? pkg : "PC44";
}

/* closure of the open item (i): PLUSASM's/FITEQN's
 * `.MAP`/`.PIN`/`.VMF` interface uses ONE canonical internal pad
 * numbering for the XC7336 die REGARDLESS of `-p` package spelling --
 * confirmed twice: experimentally, `-p XC7336-5PC44` and
 * `-p XC7336-5PQ44` give byte-identical `.MAP` pad numbers on the
 * identical design) and from the vendor's own data,
 * `MODEL.CHP`'s own device-string alias table maps every
 * `XC7336-*PC44`/`XC7336-*PQ44` spelling to the SAME internal chip
 * code, `733644` -- there is no separate PQ44 pin table in the
 * vendor's own device model at all). `decode_pkg()`'s own real
 * package (PQ44/VQ44/PC44) is correct for HUMAN-FACING naming --
 * `.PLD` diagnostics, `pin_can_drive`'s own special-pin check, the
 * `pins` command -- because those describe the REAL board. A `.VMF`
 * freeze directive is different: PLUSASM reads `PINnn` back in ITS
 * OWN internal numbering, so freezing with the display package's
 * external PQ44 pin number targets the WRONG physical pad (or
 * collides with an input's own internal-numbered pin, the
 * reproduction: "PIN42 claimed by an input and by N_3_3"). This
 * returns the numbering PLUSASM actually expects in a `.VMF`, which
 * for XC7336's own die is always PC44's, no matter what package the
 * design declares. Verified only for XC7336/XC7336Q -- XC7354/XC7372/
 * XC73108 do NOT share this (a direct test, `-p 7354PC84`
 * vs `-p 7354PC68` on the identical `PARTITION FB2_1/2/3` design,
 * gives GENUINELY DIFFERENT `.MAP` pad numbers, 82/69/70 vs 66/58/57
 * -- those dies have real, package-specific internal numbering and
 * their own tables are already correct as freeze targets). XC7318
 * shares XC7336's OWN vendor alias-table shape (one internal code,
 * `731844`, for both PC44 and PQ44) per the same `MODEL.CHP` reading,
 * but has no PQ44 table wired in this tool at all yet -- not fixed
 * here, named for whoever wires one. */
static const char *freeze_pkg(const char *dev) {
    return (strncasecmp(dev, "XC7336", 6) == 0) ? "PC44" : decode_pkg(dev);
}



/* rows 0..4 are the macrocell's PRIVATE rows; rows 5..16 are the
 * twelve SHARED product terms of its block, included only where this
 * macrocell has CLAIMED them. The two are read through separate
 * address helpers in main.c and dispatched here, so no address is re-derived
 * (methodology rule 4). A FAST block has no pool and never enters the second
 * branch. */
/* how many PRIVATE product-term rows this macrocell has. Five
 * everywhere except XC7372's MC1, whose row record is 48 bits and holds six.
 *
 * made non-static and declared in decode.h so
 * every reader shares ONE definition. Before this, `main.c`'s `mc_in_use`
 * carried its own hand-duplicated copy of the exact same `mc==1 && XC7372`
 * test (fixed independently, after `mc_in_use`'s own hardcoded
 * `row<5` missed a genuine K0108 export term) -- two copies of one rule,
 * exactly the "one table, two readers, one taught" shape.
 * Three MORE readers in `main.c` (`cmd_rows`, and `cmd_used` twice) still
 * had their OWN third/fourth/fifth hardcoded `row < 5`, never updated when
 * gave an exporting MC1 six rows. Empirically confirmed harmless
 * on the entire real 1497-file corpus (all 18 XC7372 designs, 136
 * FB{1-8}_MC1 checks): the notional 6th row never holds real content
 * anywhere in it -- a genuine, confirmed zero, not a guess -- but the
 * constant was still WRONG in principle (silently omits a row `decode`
 * itself, since this fix, correctly reads), so it is corrected here for
 * The same reason already established: a row-count model
 * that changes in one place and not its readers is a standing invitation
 * to a K0108-shaped defect the day a real design finally reaches it. */
int mc_priv_rows_x(const JedFile *jf, int fb, int mc) {
    /* XC7372's MC1 row record is 48 bits -- SIX groups. Measured at
     * a buried FB1_MC1: 1 term -> group 0; 2 -> {2,5}; 3 -> {2,3,5};
     * 4 -> {2,3,4,5}; 5 -> NOFIT. Groups 2..5 are FOUR, which is the
     * datasheet's EXPORT INCREMENT: "transfers terms in increments
     * of four to the NEIGHBOURING macrocell"). Reading only 0..4 loses group
     * 5 on every MC1 with two or more terms.
     *
     * An earlier attempt returned 6 here and took the campaign 56 -> 34,
     * because the SHARED-TERM INDEX is computed as `r - priv` and shifting it
     * misaligned every shared claim on MC1. The shared base must stay at 5. */
    /* TESTED, FOUR WAYS, NONE CARRIED:
     *   6 private rows, shared base following priv   56 PASS -> 34
     *   6 private rows, shared base pinned at 5      56 PASS -> 42
     *   `row_g_fast = 1` device-wide                 56 PASS -> 7
     *   the same shift for MC1 alone                 56 PASS -> 21
     * Group 5 IS used by the fitter and reading it as an ordinary sixth row
     * is NOT the mechanism. The datasheet says export moves terms in
     * INCREMENTS OF FOUR, so groups 2..5 are one export BLOCK, not four
     * independent rows -- they are shared with the neighbour and counting
     * them as MC1's own double-counts. The merge path, not the row window,
     * is where this belongs. */
    /* THE SIXTH ROW BELONGS TO AN EXPORTING MC1 ONLY.
     * XC7372's MC1 record is 48 bits (six groups) and the datasheet says
     * export moves a block of FOUR to the neighbour. Measured at a buried
     * FB1_MC1: 1 term -> group 0; 2 -> {2,5}; 3 -> {2,3,5}; 4 -> {2,3,4,5}.
     * Groups 2..5 ARE that block of four, so an EXPORTING MC1 needs six rows
     * while an ordinary one needs five. Applying six unconditionally took
     * The campaign 56 PASS -> 42; gating it on the cell being
     * BURIED -- which an exporter must be -- is the narrower reading. */
    if (mc == 1 && fb >= 1 && strstr(jf->device, "XC7372")) {
        int a = cfg_attr(jf, fb, mc, 0), b = cfg_attr(jf, fb, mc, 1);
        if (a == 1 && b == 1) return 6;      /* buried: may be an exporter */
    }
    return 5;
}

/* MC1 on a FAST block is the EXPORT DONOR position, and its row
 * record is 48 bits -- SIX groups -- where every other macrocell's is 40.
 * MEASURED: every passing export design has its donor at MC3..MC8; BOTH
 * failures have it at MC1 (FB1_MC1 and FB2_MC1). The sixth group holds the
 * donor's extra exported term, which is why reading it as an ordinary row
 * everywhere makes things worse (56 PASS -> 34) while not reading it loses
 * exactly one term from an MC1 export chain. */

static int rowN_state(const JedFile *jf, int fb, int mc, int r) {
    /* XC7372's MC1 record is 48 bits -- SIX groups -- and MEASURED
     * with 1..5 terms at a buried FB1_MC1 the fitter fills:
     *     1 term  -> group 0        3 terms -> groups 2,3,5
     *     2 terms -> groups 2,5     4 terms -> groups 2,3,4,5   (5 = NOFIT)
     * So MC1 has SIX private rows and group 5 is always the last one used.
     * Reading 0..4 lost it on every design with two or more terms -- K0055's
     * donor read two rows where the fitter exported three. */
    /* this used to hardcode `r - 5` (the comment,
     * "base stays 5") instead of calling `mc_priv_rows_x` a second time --
     * correct everywhere `mc_priv_rows_x` still returns 5, but the SAME
     * function returns 6 for XC7372's buried-MC1-exporter case (line 359
     * above), and this line never picked that up. The outer contradiction
     * check at decode.c's own row loop (`r - nprivrows`, computed ONCE from
     * the real `mc_priv_rows_x` return) already did this correctly; this
     * function's hardcoded sibling did not, so the two disagreed on the
     * shared-index for row `r` whenever `mc_priv_rows_x` != 5 -- reachable
     * ONLY on that one XC7372 case today. B372017 (row6, index0 by the
     * correct formula) was misread as index1 here, which nothing claims,
     * manufacturing a false DISABLED/contradiction refusal on a slot
     * `shared_pt_state` itself reports ACTIVE (`shared0  slot1 * slot2`).
     * Fixed by computing the real private-row count once and reusing it,
     * matching the outer loop's own already-correct pattern exactly. */
    int nprivrows = mc_priv_rows_x(jf, fb, mc);
    if (r < nprivrows) return row_state(jf, fb, mc, r);
    if (!shared_pt_claimed(jf, fb, mc, r - nprivrows)) return ROW_DISABLED;
    return shared_pt_state(jf, fb, r - nprivrows);
}
static int rowN_slot(const JedFile *jf, int fb, int mc, int r, int k, int *neg) {
    int nprivrows = mc_priv_rows_x(jf, fb, mc);
    if (r < nprivrows) return slot_used(jf, fb, mc, r, k, neg);
    return shared_pt_slot_used(jf, fb, r - nprivrows, k, neg);
}

static int is_exporter(const Cell *c, const Cell *next) {
    /* TWO of this signature's four conditions did not hold on
     * XC7354, and the merge fired on macrocells that export nothing.
     *
     * (a) `!c->localfb` reads attribute block 16 through a HARDCODED address
     *     in `decode_emit`, but `cfg_localfb` is NOT LOCATED on XC7354 --
     *     `config` prints "local feedback : NOT LOCATED on this device" for
     *     the very same macrocell. The emitter was reading a real fuse with
     *     no evidence of what it means, so the condition was noise. It is now
     *     `c->localfb_known`, and drops out of the test where unlocated.
     *
     * (b) "has nowhere for its terms to go EXCEPT the export chain" ignored
     *     the UIM. A buried combinatorial macrocell feeding a UIM wired-AND
     *     node is an ORDINARY OUTPUT that happens not to reach a pin.
     *     `gt_ba_C0021` is exactly this: FB3_MC1 and FB3_MC2 each hold four
     *     terms and both drive UIM line 0, which the fitter's own `.eqn`
     *     states as `/Q_JOIN = /Q_0 * /Q_1`. Neither is at the 4-pterm limit,
     *     so neither needs a split at all. The merge folded MC1's four terms
     *     into MC2, emitted one 8-term equation, and left `N_3_1` referenced
     *     by the UIM node but never declared -- `cl126`, no fit.
     *
     * That is the failure mode with the sign reversed: a later change fixed a
     * merge that did not fire when it should, and installed one that fires
     * when it should not. The discriminator is now MEASURED on both devices
     * rather than inferred from an absence. */
    return c->used && !c->reg && c->buried && next->used &&
           !(c->localfb_known && c->localfb) && !c->drives_uim;
}

int decode_emit(const char *jedpath, const char *base) {
    JedFile jf;
    /* this was `cell[7][10]`, indexed by FUNCTION BLOCK, so it held
     * six. XC7372 has EIGHT blocks and XC73108 has TWELVE -- an 8-block part
     * wrote past the end and segfaulted the moment `decode_nfb`'s cap was
     * raised. Sized for twelve. */
    static Cell cell[13][10];
    char pldname[256], vmfname[256], dname[256];
    FILE *pf, *vf;
    /* seen_in was sized 64. PC68 pins run to 68 and PC84 to 84, so a
     * literal on pin 65 overflowed it and was never DECLARED --
     * `cl126: The equation input 'I_65' is not declared`. */
    int fb, mc, r, k, seen_in[128], nin = 0, useclk[2];
    /* Generalized to every device: the
     * macrocell (if any) that PHYSICALLY drives the FastCLK pin, when the
     * fitter promoted a product-term clock onto it (RH336/XC7336 first
     * found it on an NFB<6 device; P004/the combination sweep found
     * the identical mechanism on XC7354-PC68, NFB==6, which this originally
     * did not cover -- see ck_pin/cl_pin below). 0 means "none" -- the
     * ordinary case where CK/CL is fed externally and no macrocell claims
     * that pin at all. */
    int ck_drv_fb = 0, ck_drv_mc = 0, cl_drv_fb = 0, cl_drv_mc = 0;
    /* The FastCLK pin numbers, MIRRORED from the freeze-write
     * branches far below (search "" there) -- computed once, here,
     * so the driven-clock detection and the pin-conflict checker's
     * reservation agree with what actually gets frozen. 0 means
     * "unmeasured for this device/package, left unfrozen" (matches the
     * write branches' own fallback). Do NOT hand-duplicate these numbers
     * a third time; keep this a single source of truth if this is ever
     * touched again. */
    int ck_pin = 0, cl_pin = 0;
    int NSHARED;   /* size of the block's shared pool */
    int nreg = 0, rclk_pin = 0;   /* registered input pads */
    int nce = 0;                  /* any pad with a clock enable */
    /* set once rclk_pin is known -- see the comment
     * at that computation. When true, the RCLK group reuses CK/CL's own
     * name and pin instead of a separate "RC" (same physical net). */
    int merge_rc_ck = 0, merge_rc_cl = 0;
    const char *rc_name = "RC";
    int ndrop = 0, nterm_total = 0, nused = 0;
    /* The two halves condition. A wired-AND UIM node
     * can invert a signal's LINE for the whole device; every ordinary row that
     * then wants the plain signal selects the COMPLEMENT column, and this
     * decoder reads that as a negation. Neither half alone is a defect --
     * sparse corpus designs carry 2-3 genuine negations and no UIM node at
     * all -- so the alarm is the CONJUNCTION. */
    int saw_wired_and = 0, saw_negated_literal = 0, bonded_member = 0;
    /* The emitter was written for four function blocks and PC44, and
     * said so in eight loops and four string literals. Both now come from the
     * DEVICE, in one place each. */
    int NFB = 4; const char *PKG = "PC44";
    UimNode unode[UIM_NODE_MAX]; int nunode = 0;
    int useoe[2];
    char sig[13][10][32];

    if (jed_load(jedpath, &jf) != 0) {
        fprintf(stderr, "cannot read %s\n", jedpath);
        return 1;
    }

    print_target(&jf);
    if (geometry_gate(&jf)) { jed_free(&jf); return 2; }
    if (target_gate(&jf, 1)) { jed_free(&jf); return 2; }
    /* XC7354 ENABLED FOR TESTING. Everything the emitter needs is
     * measured -- both UIM source bands, the polarity, async and
     * clock-source fields, the group bases, the
     * complete PC68 pinout -- and the emitter is device-driven.
     * What was never done is VALIDATION, so this is
     * enabled in order to attempt a round cycle, not because one has passed. */
    /* XC73108 added. Its profile is built ENTIRELY from measured
     * tables with no inherited field, but like XC7354 and
     * XC7372 before it this is enabled TO ATTEMPT A ROUND CYCLE, not because
     * one has passed. */
    if (strstr(jf.device, "XC73144")) {      /* its own message */
        device_is_73144(&jf); jed_free(&jf); return 2;
    }
    /* XC7236A added -- 4 blocks, 36 macrocells, QF 128400, and a
     * PC44 pinout MEASURED by fitting 36 probes (34 bonded). Its geometry
     * matches XC7336's exactly, so the same paths apply. NOT yet
     * round-cycled: admitted to decoding, and the round cycle is the test
     * that follows. */
    if (!(strstr(jf.device, "XC7336") || strstr(jf.device, "XC7318") ||
          strstr(jf.device, "XC7354") || strstr(jf.device, "XC7372") ||
          strstr(jf.device, "XC7236A") ||
          strstr(jf.device, "XC73108"))) {
        fprintf(stderr,
            "REFUSING to decode '%s'.\n"
            "The emitter is exercised on XC7336, XC7318 and XC7354 only.\n",
            jf.device);
        jed_free(&jf);
        return 2;
    }
    NFB = decode_nfb(jf.device);
    PKG = decode_pkg(jf.device);
    memset(cell, 0, sizeof cell);
    memset(seen_in, 0, sizeof seen_in);
    useclk[0] = useclk[1] = 0;
    useoe[0] = useoe[1] = 0;

    for (fb = 1; fb <= NFB; fb++)
        for (mc = 1; mc <= 9; mc++) {
            Cell *c = &cell[fb][mc];
            c->fb = fb; c->mc = mc;
            c->used = mc_in_use(&jf, fb, mc);
            /* A MACROCELL WITH NO ACTIVE ROW CAN STILL DRIVE ITS
             * PIN -- with a CONSTANT. The in-use rule (one of five
             * owned rows ACTIVE) cannot see it, and it is correct not to:
             * the rule was validated against the fitter's `.RES` and the
             * fitter does not list these either. The bitmap does.
             *
             *   `/Q = GND`   ALL FIVE ROWS DISABLED. The sum of no products
             *                is 0.
             *   `/Q = VCC`   ONE ROW ALL-ZERO (ROW_UNUSED). A product term
             *                with no literal selected is always TRUE, so the
             *                sum is 1.
             *
             * Neither state is ROW_ACTIVE, so both read as UNUSED and the
             * macrocell's OUTPUT PIN VANISHED from the rebuild. `gt_ba_C0001`
             * declares nine outputs in FB1 and rebuilt with six: Q1_1, Q1_5
             * and Q1_9 are each `/Q = GND`.
             *
             * THE DISCRIMINATOR IS THE OUTPUT ENABLE. A genuinely unused
             * macrocell has blk12/13 (blk17/18 on HD) PERMANENTLY DISABLED --
             * buried, driving nothing. A constant driver has it PERMANENTLY
             * ENABLED. Measured on 8 probes crossing {GND, VCC} x {inverted,
             * not} x {FAST, HD}: every one reports the output enable ENABLED,
             * and the row shape splits GND from VCC exactly as above. In
             * `gt_ba_C0001` the three constants read ENABLED and the two
             * genuinely unused macrocells (FB2_MC8, FB2_MC9) read DISABLED,
             * which is the counter-example class. */
            c->constval = -1;
            /* A High-Density macrocell can hold NO PRIVATE ROWS AT
             * ALL and still have logic, if its only product term came from
             * The block's SHARED pool. `mc_in_use` sees no
             * ACTIVE row and says unused; the constant test then reads
             * "no active row + output enabled" as a CONSTANT and emitted
             * `/Q = GND` for `H77064`'s FB6_MC8, whose fitter equation is
             * `/S6_8 := N2 * N5`.
             * A macrocell that has CLAIMED a shared term is in use and is not
             * a constant. Checked before both. */
            /* EIGHTH..TENTH `NFB == 6` guards -- shared product
             * terms, the export merge and this one all excluded XC7372. */
            /* reads XC7354's shared-pool
             * addresses. XC73108's shared terms are NOT located, so on that
             * device the test claimed 24 idle macrocells (MC1/3/6/8 of six
             * blocks) and the emitter declared them as equation-less NODEs. */
            /* RE-TESTED under rule 0ae, and it holds -- AT THE
             * TIME. "XC73108's shared_pt blocks 25..40 at offsets 96/104"
             * described a table this file no longer has: the CURRENT
             * `shared_pt[]` (below) uses offsets 0/8/16, not 96/104 --
             * this guard's own justification names a field that was later
             * replaced (the `cfg_bit()`-based fix, which this same
             * function's own comment says already "transfers as written"
             * for every device, row_per_record included). A STALE guard
             * left in place past the fix it was protecting against is
             * exactly this project's own named "workaround outlives its
             * cause" trap.
             *
             * CONFIRMED STALE, not just suspected --
             * `xc7300dump rows`/`uim` (both already use `shared_pt_claimed`
             * for XC73108, via this exact function) correctly resolve a
             * shared-only macrocell's real content (FB10_MC4 in
             * `QA029.JED`: `shared0 slot1*slot3`, both slots UIM-resolve
             * cleanly) while THIS blanket exclusion still forced `c->used
             * = 0` for it, so the constant-detection block below wrongly
             * read "no active row" as `/N = GND` -- a genuine silent-
             * wrong-decode the real fitter's own `.MAP`/`.EQN` contradicts
             * directly (real content: `I3*I5`-equivalent via the shared
             * pool, not a constant). Removing `!device_row_per_record`
             * fixes it with ZERO regression on the full real-design corpus
             * (fail-before/pass-after, every real design unaffected). */
            if (!c->used && NFB >= 6 && fb > 2) {
                int i, nsh = shared_pt_count(&jf);
                for (i = 0; i < nsh; i++)
                    if (shared_pt_claimed(&jf, fb, mc, i)) { c->used = 1; break; }
            }
            if (!c->used) {
                int b0 = cfg_attr(&jf, fb, mc, 0), b1 = cfg_attr(&jf, fb, mc, 1);
                if (b0 >= 0 && b1 >= 0 && !(b0 && b1)) {
                    int rr, anyzero = 0, anydis = 0;
                    for (rr = 0; rr < mc_priv_rows_x(&jf, fb, mc); rr++) {
                        int st = row_state(&jf, fb, mc, rr);
                        if (st == ROW_UNUSED)   anyzero = 1;
                        if (st == ROW_DISABLED) anydis  = 1;
                    }
                    /* on XC73108 an IDLE macrocell reads b0=b1=0 and
                     * has EVERY row UNUSED, so this test claimed all 108 of
                     * them as constant drivers and the emitter wrote 28 `=
                     * VCC` nodes -- which PLUSASM rejects with `cl238: not
                     * used as an equation output`. A macrocell genuinely
                     * driven to a constant has at least one DISABLED row;
                     * an idle one has none. */
                    /* The `!row_per_record` term here was DEAD.
                     * It against a symptom whose cause
                     * turned out to be the block cap, and re-testing
                     * under rule 0ae gives byte-identical output on 40
                     * XC73108 designs with and without it. `anydis` alone
                     * carries the test. Removed rather than left as a
                     * misleading comment. */
                    if (anydis) {
                        c->constval = anyzero;   /* 1 = VCC, 0 = GND */
                        c->used = 1;
                    }
                }
            }
            if (!c->used) continue;
            c->reg     = cfg_attr(&jf, fb, mc, 2);
            /* blk45/46 is a THREE-STATE field on an XC7354 High-
             * Density macrocell, and `cmd_config` has printed all
             * three since then:
             *     (1,x) combinatorial
             *     (1,0)/(x,0) registered, clocked from FastCLK
             *     (0,1) registered, CLOCK FROM A PRODUCT TERM
             * `cfg_attr(...,2)` reads blk45 ALONE, so the emitter called a
             * product-term-clocked macrocell COMBINATORIAL and dropped both
             * the register and the clock -- the clock's own product term then
             * came out as an extra logic term. The reader knew; the writer
             * did not. Same shape as
             *
             * THE CLOCK TERM IS AT ROW 3. Measured on 12 probes with
             * `OPTIONS OFF CLOCK_OPT`, crossing logic-term counts 1..4 with
             * {no async, .RSTF, .SETF}: row 3 holds the clock in all twelve
             * while the LOGIC rows move around it (rows 0,1,2,4 with no
             * async; 1,2,4 with .SETF at row0; 0,2,4 with .RSTF at row1).
             * CONTROL: with FastCLK the same designs put ORDINARY LOGIC in
             * row 3 -- P354 above is `slot6 * slot7` there -- so row 3 is not
             * reserved and this is not a tautology.
             *
             * FAST blocks never produced a product-term clock: with CLOCK_OPT
             * off the fitter still routed every FB1 probe to FastCLK. Recorded
             * as an OBSERVATION, not a proof that they cannot. */
            c->ptclk = 0;
            /* was `NFB == 6`, so XC7372 (EIGHT blocks) never
             * entered and `async_present` stayed false -- every SETF/RSTF
             * design folded its async row into the logic. The pair is
             * {41,42} here and the encoding is XC7354's:
             * none 1,1  SET 0,1  RESET 1,0. */
            /* SEVENTH `NFB == 6` guard. XC7372 never detected a
             * PRODUCT-TERM CLOCK, so every `CLKF = <signal>` macrocell
             * decoded COMBINATORIAL and lost its register. */
            if (NFB >= 6 && fb > 2) {
                int r45 = cfg_bit(&jf, fb, mc, 45), r46 = cfg_bit(&jf, fb, mc, 46);
                if (r45 == 0 && r46 == 1) { c->reg = 1; c->ptclk = 1; }
            }
            /* polarity is blk19 on XC7336 and the blk21-24 FIELD on
             * XC7354. Reading blk19 there gave the wrong sense and
             * every registered async design rebuilt INVERTED -- 16 of 16
             * vectors differing on a design that was otherwise exact. */
            c->inv = cfg_polarity_bit(&jf, fb, mc);
            /* The PIN/UIM SPLIT. This macrocell stores the
             * COMPLEMENT for the UIM while its PIN drives the TRUE equation.
             * Emitting `/N = eq` gets the UIM right and the PIN wrong -- which
             * is why `HAN` failed at exactly the pin its member drives.
             *
             * Emit `N = eq` instead and NEGATE every reference to it: the
             * logic is identical and the pin then drives the true value. The
             * fitter re-derives blk19/21..24 for itself.
             *
             * `JN` -- inverted with blk19 = 1, an ORDINARY inversion -- is NOT
             * this and must keep the current behaviour. */
            c->split = cfg_pin_uim_split(&jf, fb, mc);
            if (c->split) c->inv = 0;
            c->clk     = cfg_attr(&jf, fb, mc, 3);
            c->toggle  = !cfg_attr(&jf, fb, mc, 4);
            c->oe_used = !cfg_attr(&jf, fb, mc, 0);
            c->oe_src  = cfg_attr(&jf, fb, mc, 1);
            /* blk12/13 = 1,1 is "permanently disabled" -- the
             * macrocell drives no pin and must be emitted as a NODE. */
            c->buried  = cfg_attr(&jf, fb, mc, 0) && cfg_attr(&jf, fb, mc, 1);
            /* no drivable package pin => it CANNOT be an output pin,
             * whatever the output-enable field says. */
            if (!c->buried && !pin_can_drive(jf.device, PKG, fb, mc, NULL)) {
                c->buried = 1;
                int pp = pin_for_macrocell(jf.device, PKG, fb, mc);
                const device_t *dd = device_find(jf.device);
                const char *ff = (pp && dd)
                               ? device_pin_function(dd, PKG, pp) : NULL;
                fprintf(stderr, "note: FB%d_MC%d drives PIN%02d, which is %s "
                                "and cannot carry an output -- emitted as a "
                                "buried NODE\n",
                        fb, mc, pp, ff ? ff : "unusable");
            }
            /* on XC7354 the async term is a TWO-BIT field, blk41/42:
             * (1,1) none, (1,0) RESET, (0,1) SET. `cfg_attr(...,6)`
             * reads blk41 alone, which is 1 for BOTH none and reset, so the
             * emitter could not tell "no async" from "reset" and wrote
             * neither. And `alloc_hi` below reads blk18, which on XC7354 is
             * The HD OUTPUT-ENABLE SELECT, not an allocation bit
             * measured that XC7354 has no allocation bit at all. */
            /* was `NFB == 6`, so XC7372 (EIGHT blocks) never
             * entered and `async_present` stayed false -- every SETF/RSTF
             * design folded its async row into the logic. The pair is
             * {41,42} here and the encoding is XC7354's:
             * none 1,1  SET 0,1  RESET 1,0. */
            if (NFB >= 6 && fb > 2) {
                int a1 = cfg_bit(&jf, fb, mc, 41), a2 = cfg_bit(&jf, fb, mc, 42);
                c->async_is_set = (a1 == 0 && a2 == 1);
                c->async_present = !(a1 == 1 && a2 == 1) && a1 >= 0 && a2 >= 0;
                /* (0,0) is a FOURTH state, previously read identically to
                 * (1,0) RESET (async_present true, async_is_set false) --
                 * measured directly (R6SRS, the isolated
                 * reproducer) against the real fitter's own .EQN, which
                 * prints BOTH `.SETF` and `.RSTF` on the same macrocell:
                 * (0,0) means a DEDICATED SET (row0) AND a DEDICATED RESET
                 * (row1) both exist. The RESET half was already handled
                 * correctly by the existing (async_present, !async_is_set)
                 * path -- this only adds the missing SET half back. */
                c->async_both = (a1 == 0 && a2 == 0);
            } else if (NFB >= 6) {
                /* was `NFB == 6`. XC7372's FB1/FB2 are FAST as well,
                 * so every `.SETF` on them was folded into the logic there
                 * too -- the very defect this branch exists to prevent.
                 *
                 * XC7354 FAST BLOCKS HAVE AN ASYNC SET. This branch
                 * said `async_present = 0` flatly, so every `.SETF` on FB1 or
                 * FB2 was folded into the logic as an extra product term -- a
                 * decode that reads correctly and writes a DIFFERENT TRUTH
                 * TABLE, once more. `gt_m9_M0255` is `/Q:= /A + /B`
                 * with `Q.SETF = R`; it decoded as a three-term equation with
                 * no `.SETF` at all.
                 *
                 * Measurement showed the async field on FB3..FB6 ONLY, excluding
                 * The FAST blocks because PLUSASM refuses `.RSTF` there,
                 * cl447). That exclusion was then read as "FAST
                 * blocks have no async". `.SETF` is a different question and
                 * the fitter accepts it.
                 *
                 * THERE IS NO CONFIG BIT. Matched triples on FB1 and FB2 at
                 * MC1 and MC5 -- no async / `.SETF` / `.RSTF` -- move NOTHING
                 * at offset 96 or 104; every differing fuse is in the AND
                 * array at row 0. All four `.RSTF` probes were refused with
                 * cl447, so on a FAST block the sense is always SET. The
                 * signal is POSITIONAL, and physical: only a flip-flop can
                 * have an async set.
                 *
                 *   REGISTERED + row0 ACTIVE    -> row0 IS the `.SETF` term
                 *   REGISTERED + row0 DISABLED  -> no async
                 *   COMBINATORIAL               -> no async, row0 is logic
                 *
                 * CONTROL, both directions present: 208 of 208 single-
                 * macrocell XC7354 FAST designs. 12 registered with row0
                 * active all carry `.SETF`; 20 registered with row0 disabled
                 * carry none; and the 140 COMBINATORIAL cells with row0
                 * ACTIVE are the counter-example class that makes this a test
                 * rather than a tautology -- row0 is ordinary logic there.
                 * Single-macrocell designs only: a design has many macrocells
                 * and only one carries the `.SETF`, which is the confound
                 * met. */
                c->async_is_set = 1;
                /* this was `c->reg && row_state(row0) == ROW_ACTIVE`,
                 * which is not a fuse read -- it ASSUMES every registered FAST
                 * macrocell with an active row 0 has an async SET term. A
                 * macrocell whose row 0 IS ITS LOGIC then had that logic
                 * diverted into `async_term` and never counted, and with no
                 * other row the whole design decoded to nothing: the FB1/FB2
                 * refusal
                 *
                 * An async SET at row 0 requires LOGIC SOMEWHERE ELSE -- a
                 * macrocell that only sets and never computes has no output
                 * worth registering. So row 0 is the async term only if some
                 * other row is also active. If row 0 stands alone it is the
                 * logic, which is exactly the shape the fitter emits for
                 * `/N:= (I)`. The branch is kept for its real case. */
                {   int _r, _other = 0;
                    for (_r = 1; _r < 5; _r++)
                        if (row_state(&jf, fb, mc, _r) == ROW_ACTIVE) { _other = 1; break; }
                    c->async_present = c->reg && _other &&
                        row_state(&jf, fb, mc, 0) == ROW_ACTIVE;
                }
            } else {
                c->async_is_set = cfg_attr(&jf, fb, mc, 6);
                c->async_present = 0;
            }
            c->lowpwr  = cfg_attr(&jf, fb, mc, 5);
            /* this hardcoded 16 is the address the profile owns as
             * `cfg_localfb`, and on XC7354 the profile says NOT LOCATED.
             * Reading it anyway gave `is_exporter` a condition with no
             * meaning behind it (methodology rule 4: one address, one
             * place). Track whether it is located and let the test skip it
             * where it is not. */
            c->localfb_known = localfb_blk(&jf) >= 0;
            c->localfb = c->localfb_known
                       ? !cfg_bit(&jf, fb, mc, localfb_blk(&jf)) : 0;
            c->drives_uim = mc_drives_uim(&jf, fb, mc);
            /* row0 is the ASYNC set/reset term ONLY when
             * blk18 says the logic lives in rows 1..4. With blk18 = 0 the
             * logic IS in row0, and skipping it emits an empty equation. */
            /* was `NFB == 6`. Measurement showed that XC7354 has NO
             * allocation bit, and an `alloc` matched pair on XC7372 moved no
             * configuration bit on either block type -- so reading blk18
             * there is reading an UNMEASURED fuse. */
            c->alloc_hi = (NFB >= 6) ? 0 : cfg_bit(&jf, fb, mc, 18);
            c->async_row = -1;
            c->async_row2 = -1;
            snprintf(sig[fb][mc], sizeof sig[fb][mc], "N_%d_%d", fb, mc);
        }

    /* build each macrocell's terms from its rows, resolving every literal
     * slot through the UIM to a pin or a macrocell output */
    for (fb = 1; fb <= NFB; fb++)
        for (mc = 1; mc <= 9; mc++) {
            Cell *c = &cell[fb][mc];
            if (!c->used) continue;
            nused++;
            if (c->alloc_hi) c->async_row = 0;
            /* on XC7354 the async term is identified by ROW INDEX --
             * row 0 for SET, row 1 for RESET, re-confirmed here on
             * 10 probes crossing 1/2/3/5/7 logic terms with SETF and RSTF:
             * the fitter puts the async term in row0 for SET and row1 for
             * RESET at every term count).
             *
             * It was previously pulled out by POSITION IN THE COLLECTED TERM
             * LIST, `c->term[0]` or `c->term[1]`. The list holds only ACTIVE
             * rows, and it continues into the twelve SHARED
             * rows -- so a macrocell whose row0 is DISABLED has `term[1]`
             * pointing at a SHARED PRODUCT TERM, and a `.RSTF` design then
             * took a shared row as its reset and left the real reset as
             * logic. `F20255` is exactly that: row0 disabled, row1 holding
             * the reset, and all its logic in shared rows. */
            /* FIFTH `NFB == 6` guard on this device. With it, XC7372's
             * `async_row` stayed -1, the async row was collected as ORDINARY
             * LOGIC, and every SETF/RSTF design decoded without its async
             * term -- even though `async_present` was correct. */
            /* and ONLY on a REGISTERED macrocell. Opening this to
             * eight blocks let combinatorial cells take an async row, and
             * PLUSASM refuses the result -- `cl334: Non-registered '='
             * equation output has .setf or .rst`. */
            if (NFB >= 6 && c->async_present && c->reg) {
                c->async_row = c->async_is_set ? 0 : 1;
                if (c->async_both) { c->async_row = 0; c->async_row2 = 1; }
            }
            /* THE DATASHEET STATES IT -- "when product-terms are
             * assigned to adjacent Macrocells, the product-term normally
             * dedicated to the Set or Reset function BECOMES THE INPUT TO THE
             * MACROCELL REGISTER". A macrocell that EXPORTS therefore has
             * FIVE logic rows, not four, and the row skipped as "async" is
             * carrying LOGIC. Marked here; the export merge below consumes
             * it. */
            c->async_row_is_logic = 0;
            /* only a HIGH-DENSITY block has a shared pool. `.MAP`
             * reports `Shared pterms - 0/0` for a FAST block. */
            NSHARED = (NFB >= 6 && fb > 2) ? shared_pt_count(&jf) : 0;
            {   int nprivrows = mc_priv_rows_x(&jf, fb, mc);
                for (r = 0; r < nprivrows + NSHARED; r++) {
                char buf[256]; int nl = 0, rs = rowN_state(&jf, fb, mc, r);
                buf[0] = 0;
                /* A shared row can be CLAIMED
                 * (the macrocell's own config bit says "I use shared slot
                 * i") while its CONTENT reads as ROW_DISABLED (all 48
                 * content fuses at their unprogrammed default) -- a
                 * genuine contradiction, not a legitimate "claimed but
                 * empty" state (the MIN1/SH108 evidence never
                 * shows this: every claim there had real content).
                 * MEASURED: `shared_pt[]` indices 1, 6 and 9 (the three
                 * entries at group=1, regardless of offset) read
                 * DISABLED whenever genuinely claimed and needed --
                 * a test design (a
                 * PLAIN clock, no product-term-clock interaction at all,
                 * ruling out a "clock pin doubling as
                 * data" explanation for this specific defect) needs only
                 * slots 0 and 1, and slot 1 reads DISABLED. The correct
                 * address for group=1 is UNKNOWN -- guessing one is how
                 * was introduced. Silently treating a
                 * contradiction as ROW_UNUSED-and-earlier's
                 * behaviour) drops a real term and emits a wrong design;
                 * refusing instead is the same trade already
                 * makes for every other unresolved case.
                 *
                 * (the round cycle):
                 * the WRONG address this reads is not a fixed value --
                 * on `B108S01` (a 13-term design forcing 8 of 12 shared
                 * slots, indices 1/3/9/10 all ODD-group
                 * corpus finding) the SAME contradiction reads as
                 * ROW_UNUSED (all content fuses at 0) instead of
                 * ROW_DISABLED (all at 1) -- content, not a flag, so it
                 * can land on either state depending on what else the
                 * bitmap happens to hold there. `rs == ROW_DISABLED`
                 * caught only the unlucky-all-1s half; the all-0s half
                 * passed `rs != ROW_ACTIVE` below and was DROPPED
                 * SILENTLY with no refusal at all -- confirmed by a real
                 * round cycle, `*** 1 OUTPUT(S) DIFFER ***` and
                 * `*** PIN SETS DIFFER ***` on 5 of 8 freshly generated
                 * shared-pool designs, none of which had EVER been round-
                 * cycled before. A genuinely claimed slot is
                 * never legitimately anything but ACTIVE (same evidence
                 * already cites), so the correct guard is "CLAIMED
                 * and not ACTIVE", not "CLAIMED and specifically DISABLED".
                 *
                 * `rs` is computed ONCE (was twice, plus a redundant
                 * `mc_priv_rows_x` per row) -- SH108's decode time was
                 * measured at ~20s with the naive version, back to
                 * effectively instant here. No behavioural change, this
                 * is the same check reusing its own result. */
                if (r >= nprivrows &&
                    shared_pt_claimed(&jf, fb, mc, r - nprivrows) &&
                    rs != ROW_ACTIVE) {
                    ndrop++;
                    if (ndrop <= 8)
                        fprintf(stderr, "  FB%d_MC%d row%d: shared product-term "
                                "slot %d is CLAIMED but its content reads as "
                                "%s (contradiction, not a legitimate "
                                "empty claim) -- the address for this slot is "
                                "not correctly known\n",
                                fb, mc, r, r - nprivrows,
                                rs == ROW_DISABLED ? "DISABLED" : "UNUSED");
                    continue;
                }
                if (rs != ROW_ACTIVE) continue;
                for (k = 0; k < 24; k++) {
                    int neg, srcfb, srcmc, isout, slot, pin;
                    char lit[48];
                    if (!rowN_slot(&jf, fb, mc, r, k, &neg)) continue;
                    if (getenv("XC73_TRACE_SLOT"))
                        fprintf(stderr, "  SLOT HIT FB%d_MC%d row%d k=%d neg=%d\n",
                                fb, mc, r, k, neg);
                    {
                        /* this was `sl[8]` with a cap of 8, but
                         * `uim_sources_for_line` COUNTS every source and
                         * stores only the first `cap` -- so a wired-AND line
                         * with more than eight sources returned nsrc > 8 and
                         * the loop below READ PAST THE END of an 8-element
                         * stack array. `gt_m5_P0367` is a 24-source wired-AND
                         * (`UIM_0 = /I1 * I2 * ... * I24`) and decoded to two
                         * literals out of the garbage. Same class as the
                         * out-of-bounds read, which real chips also found.
                         * A line can legally take every slot in the array. */
                        int sl[256];
                        int nsrc = uim_sources_for_line(&jf, fb, k, sl,
                                       (int)(sizeof sl / sizeof sl[0]));
                        if (getenv("XC73_TRACE_SLOT"))
                            fprintf(stderr, "    -> uim_sources_for_line(fb=%d,"
                                    " line=%d) = %d\n", fb, k, nsrc);
                        if (nsrc == 0) {
                            nl = -1; ndrop++;
                            if (ndrop <= 8)
                                fprintf(stderr, "  FB%d_MC%d row%d slot%d: the UIM "
                                        "does not resolve this line to a source\n",
                                        fb, mc, r, k);
                            else if (ndrop == 9)
                                /* this list is CAPPED at eight and said
                                 * so nowhere, so `K63` read as "8 unresolved"
                                 * under two tool versions when the real counts
                                 * were 22 and 10. A truncated list that looks
                                 * complete is the same failure as a checker
                                 * that reports nothing (rule 0ah). */
                                fprintf(stderr, "  ... further unresolved terms "
                                        "not listed; the total is on the "
                                        "REFUSING line below\n");
                            break;
                        }
                        if (nsrc > 1) {
                            saw_wired_and = 1;
                            /* A WIRED-AND line. PLUSASM expresses it
                             * as `NODE (UIM) x` with `x = s1 * s2;` -- logic
                             * inside the UIM, using no macrocell. Emitting one
                             * source and dropping the rest, as v1.24 did,
                             * makes a design that is NOT equivalent. */
                            if (!uim_node_lit(&jf, fb, k, sl, nsrc, neg, cell, sig,
                                              lit, sizeof lit, unode, &nunode,
                                              &bonded_member,
                                              seen_in, &nin)) {
                                nl = -1; ndrop++;
                                if (ndrop <= 8)
                                    fprintf(stderr, "  FB%d_MC%d row%d slot%d: "
                                            "wired-AND line, %d sources, could "
                                            "not be named\n", fb, mc, r, k, nsrc);
                                break;
                            }
                            if (nl > 0) strncat(buf, "*", sizeof buf - strlen(buf) - 1);
                            strncat(buf, lit, sizeof buf - strlen(buf) - 1);
                            nl++;
                            continue;
                        }
                        slot = sl[0];
                        {   int _ok = uim_slot_decode_dev(&jf, slot, &srcfb, &srcmc, &isout);
                            if (getenv("XC73_TRACE_SLOT"))
                                fprintf(stderr, "    -> uim_slot_decode_dev(slot=%d)"
                                        " = %d  (fb=%d mc=%d isout=%d)\n",
                                        slot, _ok, srcfb, srcmc, isout);
                            if (!_ok) {
                                nl = -1; ndrop++;
                                /* (silent-drop audit): every other
                                 * failure branch in this loop prints which
                                 * row/slot it dropped; this one incremented
                                 * ndrop silently, so a refusal caused ONLY by
                                 * this branch said "listed above" with
                                 * nothing actually listed. Still always
                                 * refuses (ndrop>0 below) -- this only fixes
                                 * the message, not a correctness gap. */
                                if (ndrop <= 8)
                                    fprintf(stderr, "  FB%d_MC%d row%d slot%d: "
                                            "source slot %d does not decode to "
                                            "a known macrocell or pad\n",
                                            fb, mc, r, k, slot);
                                break;
                            }
                        }
                    }
                    if (isout) {
                        /* A BURIED macrocell's feedback is inverted
                         * relative to a pin-driving one, but only on the
                         * FFB's OWN feedback path (same function block).
                         * Across the UIM it is not. Both halves measured. */
                        /* The trigger is LOCAL FEEDBACK, not BURIED.
                         * Measurement showed the inversion on buried sources
                         * and generalised from them; then found blk16,
                         * which marks a macrocell that feeds a neighbour in
                         * its own block WHETHER OR NOT it also drives a pin.
                         * W6 is that case -- source drives its pin, blk16 set
                         * -- and the old test missed it, so the feedback came
                         * out inverted and the round cycle failed 21 of 80. */
                        /* on XC7354 the `srcfb == fb` restriction is
                         * WRONG, and the trigger is the SOURCE BLOCK TYPE.
                         * 24 probes -- source in FB1, FB5 or FB6, target in
                         * FB3 or FB6, source buried or pin-driving, source
                         * equation written inverted or not -- and the fitter
                         * names the feedback `S_INV` in EXACTLY ONE cell of
                         * the twenty-four shapes: SOURCE IS A BURIED FAST
                         * MACROCELL whose equation was written non-inverted.
                         * It does so for targets in FB3 AND FB6, so the
                         * inversion crosses blocks and the same-block test
                         * suppressed it.
                         *
                         * That follows: a FAST macrocell's output
                         * is ALWAYS stored inverted. One that drives a pin is
                         * re-inverted by its output buffer, so the UIM sees
                         * the true value; a BURIED one is not, so the UIM sees
                         * the complement. A High-Density source never shows it
                         * -- buried or not, 16 of the 24 probes.
                         *
                         * XC7336 keeps the rule exactly: all four of
                         * its blocks are FAST, `localfb` IS located there, and
                         * W6's case was measured with the same-block test. */
                        /* The same-block flip is a FAST-BLOCK
                         * property. Measurement showed it on
                         * XC7336, where ALL FOUR blocks are FAST, so
                         * "same block" and "FAST source" were the same
                         * condition and could not be told apart there.
                         *
                         * `V0033` separates them. `/S6_6 = N5 * S5_9 * S6_1`
                         * reads TWO buried feedbacks: FB5_MC9 from another
                         * block, and FB6_MC1 from its OWN block, both
                         * High-Density. The bitmap holds `/slot8 * /slot9`
                         * for them, and both nodes are emitted inverted
                         * does not apply -- they are HD -- but the
                         * fitter wrote them inverted anyway). The cross-block
                         * one came out RIGHT as `/N_5_9`; the same-block one
                         * came out `N_6_1` where `/N_6_1` was needed. The
                         * flip fired on a HIGH-DENSITY same-block source and
                         * should not have.
                         *
                         * tried the opposite widening -- any buried
                         * FAST source, ANY target block -- and that took the
                         * gated campaign 14 -> 5. The correct reading is the
                         * INTERSECTION the two experiments agree on: the
                         * source must be BURIED (or locally fed back), in the
                         * SAME BLOCK, and that block must be FAST. */
                        /* EXPERIMENT: drop the BURIED requirement on
                         * XC7354 -- a same-block FAST source is inverted on
                         * the UIM whether or not it drives a pin. */
                        /* was `NFB == 6`, so XC7372 took XC7336's
                         * localfb/buried branch instead of this one. */
                        if (NFB >= 6
                            ? (srcfb == fb && srcfb <= 2)
                            : ((cell[srcfb][srcmc].localfb ||
                                cell[srcfb][srcmc].buried) && srcfb == fb))
                            neg = !neg;
                        /* A negated MACROCELL literal is NOT the
                         * defect. When a UIM node needs `/S4_1` the fitter
                         * inverts the SOURCE MACROCELL (blk21..24 -> 1111),
                         * the decoder reads that polarity and emits
                         * `/N_4_1 = ...`, and every consumer referencing the
                         * true signal writes `/N_4_1`. Measured on `NN`:
                         * decode -> refit -> ALL OUTPUT PINS AGREE. */
                        if (cell[srcfb][srcmc].split) neg = !neg;
                        snprintf(lit, sizeof lit, "%s%s", neg ? "/" : "",
                                 sig[srcfb][srcmc]);
                    } else {
                        pin = pin_of_slot_dev(&jf, slot);
                        if (getenv("XC73_TRACE_SLOT"))
                            fprintf(stderr, "    -> pin_of_slot_dev(slot=%d) = %d\n", slot, pin);
                        if (!pin) { nl = -1; ndrop++;
                            if (ndrop <= 8)
                                fprintf(stderr, "  FB%d_MC%d row%d slot%d: source "
                                        "slot %d has no package pin\n",
                                        fb, mc, r, k, slot);
                            break; }
                        /* A negated INPUT-PAD literal is the defect.
                         * A pin has no macrocell whose polarity bit could
                         * carry the inversion, so when a UIM node needs `/P7`
                         * the line is inverted with nothing to record it here
                         * and every ordinary row that wants plain `P7` reads
                         * as `/P7`. `D48` (`/P7`) and `D71` (`/P80`) fail;
                         * `NN`, whose negated members are MACROCELLS, passes. */
                        /* The pad's LINE may be inverted device-wide,
                         * in which case a row that wants the plain
                         * signal selects the COMPLEMENT column. The column
                         * alone is therefore not the literal's polarity:
                         *
                         *     literal = column XOR pad_inversion(pin)
                         *
                         * The table is a COMPLETE enumeration of the twelve
                         * dedicated inputs. Where it does not apply
                         * -- an I/O pad used as an input, another
                         * package, another device -- it returns -1 and the
                         * guard still refuses rather than guessing. */
                        {   int pinv = x108_pad_inverted(&jf, pin);
                            if (pinv > 0) neg = !neg;
                            else if (pinv < 0 && neg) saw_negated_literal = 1;
                        }
                        snprintf(lit, sizeof lit, "%sI_%d", neg ? "/" : "", pin);
                        if (pin < 128 && !seen_in[pin]) { seen_in[pin] = 1; nin++; }
                    }
                    if (nl > 0) strncat(buf, "*", sizeof buf - strlen(buf) - 1);
                    strncat(buf, lit, sizeof buf - strlen(buf) - 1);
                    nl++;
                }
                if (getenv("XC73_TRACE_SLOT"))
                    fprintf(stderr, "    row%d finished: nl=%d  (<=0 means the "
                            "whole row is discarded)\n", r, nl);
                if (nl <= 0) continue;
                /* on XC7354 the ASYNC term is the row AFTER the
                 * logic, not row0. Measured on A101 -- `Q := A*B; Q.RSTF = R`
                 * decodes to row0 = slot0*slot1 (the logic) and row1 = slot2
                 * (the reset). XC7336 is the other way round, which is what
                 * blk18 disambiguates there and why measurement showed no such bit
                 * here: the ORDER is fixed on this device. */
                if (getenv("XC73_TRACE_SLOT"))
                    fprintf(stderr, "    row%d: async_row=%d ptclk=%d -> %s\n",
                            r, c->async_row, c->ptclk,
                            (c->async_row >= 0 && r == c->async_row) ? "DIVERTED to async_term"
                            : ((c->ptclk && r == 3) ? "DIVERTED to clk_term" : "counted"));
                if (c->async_row >= 0 && r == c->async_row) {
                    snprintf(c->async_term, sizeof c->async_term, "%s", buf);
                    continue;
                }
                if (c->async_row2 >= 0 && r == c->async_row2) {
                    snprintf(c->async_term2, sizeof c->async_term2, "%s", buf);
                    continue;
                }
                /* row 3 is the CLOCK, not logic, when blk45/46 says
                 * the clock comes from a product term. Taken BEFORE the async
                 * reindexing below, which counts logic terms only. */
                if (c->ptclk && r == 3) {
                    snprintf(c->clk_term, sizeof c->clk_term, "%s", buf);
                    continue;
                }
                snprintf(c->term[c->nterm], sizeof c->term[0], "%s", buf);
                c->nterm++; nterm_total++;
                }
            }
            /* on XC7354 the ASYNC term is the LAST active row, not
             * row0. Measured on A101 -- `Q := A*B; Q.RSTF = R` gives
             * row0 = slot0*slot1 (the logic) and row1 = slot2 (the reset).
             * XC7336 is the other way round, which blk18 disambiguates there
             * and is why measurement showed no such bit here: the ORDER IS FIXED. */
            /* The async row DIFFERS BY SENSE on XC7354.
             *   A101  Q := A*B ; Q.RSTF = R
             *         row0 logic, row1 RESET      -- async is LAST
             *   A102  /Q := /B + /A ; Q.SETF = R
             *         row0 SET, rows1-2 logic     -- async is FIRST
             * That is why "the last active row" fixed RESET and broke SET.
             * */
            /* The async term sits at a FIXED ROW on XC7354 --
             * ROW 0 for SET, ROW 1 for RESET -- whatever the logic does
             * around it:
             *   A101  Q := A*B ; RSTF        row0 logic, ROW1 reset
             *   A104  Q := A*B + C*D ; RSTF  row0 logic, ROW1 reset, row2 logic
             *   A102  /Q := /B + /A ; SETF   ROW0 set, rows1-2 logic
             * "last active row" fitted A101 by accident and broke on A104,
             * where the async term is in the MIDDLE. */
            /* if the async row was the cell's ONLY active row, the
             * cell now has NO logic and would be declared an output with no
             * equation -- `cl238: since 'N_2_3' is not used as an equation
             * output it is not allowed in ...`. The old term-index path was
             * guarded by `nterm > 1` and never hit this. Give the term back:
             * a register whose only product term is its async term is read as
             * ordinary logic, which is what the fitter reports. */
            if (c->async_term[0] && c->nterm == 0) {
                snprintf(c->term[0], sizeof c->term[0], "%s", c->async_term);
                c->nterm = 1;
                c->async_term[0] = '\0';
            }
            /* same rule, for the SECOND async row (async_both only) -- a
             * macrocell with BOTH .SETF and .RSTF and no other logic at all
             * would otherwise still emit an equation-less output. */
            if (c->async_term2[0] && c->nterm == 0) {
                snprintf(c->term[0], sizeof c->term[0], "%s", c->async_term2);
                c->nterm = 1;
                c->async_term2[0] = '\0';
            }
            if (c->reg && !c->ptclk) useclk[c->clk] = 1;
        }

    /* merge each exporter's terms into its neighbour */
    for (fb = 1; fb <= NFB; fb++)
        for (mc = 1; mc < 9; mc++) {
            Cell *c = &cell[fb][mc], *n = &cell[fb][mc + 1];
            /* THE EXPORT MERGE MUST NOT FIRE ON A HIGH-DENSITY
             * BLOCK. Export is the FAST block's overflow mechanism,
             * past its private rows a FAST macrocell SPLITS onto the
             * PRECEDING one). A High-Density macrocell overflows into the
             * block's TWELVE SHARED PRODUCT TERMS instead
             * and never recruits a neighbour.
             *
             * `P20008` is the case: FB3_MC8 is buried, combinatorial, and its
             * neighbour FB3_MC9 is used, so `is_exporter` fired and folded
             * MC8's five rows into MC9 -- which HAS ONLY FOUR OF ITS OWN AND
             * NEEDED NOTHING. `N_3_8` vanished from the emitted source and
             * `N_3_9` came out with nine terms against the fitter's four.
             *
             * MC8 is not an exporter, it is an ordinary buried node that the
             * fitter kept after the design's only consumer simplified away
             * -- its equation reduces to a form that no longer mentions it.
             * added `!drives_uim` for exactly this shape and it cannot help
             * here, because a node nothing reads drives nothing. THE BLOCK
             * TYPE is the discriminator that does not depend on the design. */
            if (NFB >= 6 && fb > 2) continue;
            if (!is_exporter(c, n)) continue;
            /* AND THE BORROWER MUST BE FULL.
             *
             * A macrocell with a free product-term row has no reason to
             * recruit its neighbour, and MEASURED over every design this
             * project has fitted with the fitter's own `.EXPORT` as the
             * label, it never does: 5 of 5 genuine exports have the borrower
             * at ZERO free logic rows, and no design with a free row exports.
             * The condition is one-sided by construction -- it cannot suppress
             * a real export, only the invented ones.
             *
             * `PIND` is the case this fixes: `FB1_MC1` holds an INDEPENDENT
             * buried node, `FB1_MC2` has row4 free and needs nothing, and the
             * merge folded MC1's terms into MC2 anyway -- a fourth product
             * term on pin 13 that the fitter never wrote (an earlier attempt was
             * reverted for want of a discriminator). The question is "has the DONOR anywhere else to
             * put its terms"; the question belongs on the BORROWER. */
            {   int rr, freerow = 0;
                for (rr = 1; rr < mc_priv_rows_x(&jf, fb, mc + 1); rr++)
                    if (rowN_state(&jf, fb, mc + 1, rr) == ROW_DISABLED)
                        freerow = 1;
                /* BUT NOT FOR AN INTERMEDIATE LINK IN A CHAIN.
                 *
                 * The rule -- "a macrocell with a free logic row has no
                 * reason to recruit its neighbour" -- holds for a borrower
                 * that KEEPS the terms. It is wrong for one that PASSES THEM
                 * ON: an intermediate exporter's own rows say nothing about
                 * how many terms flow through it.
                 *
                 * `gt_m5_P0245`, `P0307` and `P0251` are three-and four-deep
                 * chains where the middle link had a free row, the guard
                 * fired, and the FIRST macrocell was left as a standalone
                 * node feeding nothing -- each design losing exactly one
                 * export and one output pin.
                 *
                 * Simply dropping the guard fixes those three and CHANGES THE
                 * DECODE OF ALL 30 passing designs sampled -- far too blunt.
                 * The discriminator is whether the borrower exports onward. */
                if (freerow && mc + 2 <= 9
                    && is_exporter(n, &cell[fb][mc + 2]))
                    freerow = 0;
                if (freerow && !getenv("XC73_NOFREEROW")) continue;
            }
            /* The donor is exporting, so its Set/Reset product term
             * IS a logic term -- take it back before merging. */
            if (c->async_term[0] && c->nterm < 64) {
                snprintf(c->term[c->nterm], sizeof c->term[0], "%s",
                         c->async_term);
                c->nterm++;
                c->async_term[0] = 0;
            }
            if (c->async_term2[0] && c->nterm < 64) {
                snprintf(c->term[c->nterm], sizeof c->term[0], "%s",
                         c->async_term2);
                c->nterm++;
                c->async_term2[0] = 0;
            }
            /* The bound was 16 and the excess was dropped SILENTLY.
             * `gt_m5_P0307` is a four-deep chain of 17 terms, so it lost
             * exactly one -- a decode that assembles, fits, and behaves
             * differently from the chip. All four Class B designs were one
             * term short for this reason and nothing said so. Truncation is
             * now IMPOSSIBLE at this size and LOUD if it ever happens again,
             * because the rule is that a missing term is not a partial
             * answer but a wrong one. */
            for (r = 0; r < c->nterm; r++) {
                if (n->nterm >= (int)(sizeof n->term / sizeof n->term[0])) {
                    fprintf(stderr, "FB%d_MC%d: export chain exceeds %d terms "
                            "-- REFUSING to truncate\n",
                            fb, mc + 1,
                            (int)(sizeof n->term / sizeof n->term[0]));
                    ndrop++;
                    break;
                }
                snprintf(n->term[n->nterm], sizeof n->term[0], "%s", c->term[r]);
                n->nterm++;
            }
            c->used = 0;   /* it is not an output of its own */
        }

    /* FIND THE FastCLK-PROMOTED-PRODUCT-TERM DRIVER, IF
     * ANY. On an NFB<6 device CK/CL are ALWAYS PIN05/PIN06
     * and are normally fed EXTERNALLY -- no macrocell in the fuse map
     * claims that pin at all. `RH336` shows a different, genuine case: the
     * fitter promoted a product-term clock (`R1.CLKF = PTC1`, an ordinary
     * NODE in the source) onto the FastCLK resource, and the macrocell
     * implementing that promotion (`.MAP`: "FCLKIO_1 FB3-1 pin 6") is
     * bonded to the SAME physical pin decode already reserves for CL --
     * decode's own ordinary-output naming then ALSO claims it
     * (`pin_for_macrocell` correctly reports pin 6 for FB3-1), and the
     * pin-conflict checker correctly refuses ("PIN06 claimed by CL and by
     * N_3_1") rather than emit a broken .VMF.
     *
     * VERIFIED, not guessed (`/tmp/fclk_test/FCT1`, kept as
     * a test design): `FASTCLOCK CK; CK =
     * (I1*I2); R1.CLKF = CK;` -- no separate OUTPUTPIN for the driver, no
     * VMF freeze -- is legal PLUSASM and the fitter's own `.EQN`/`.PIN`
     * reproduce the IDENTICAL FCLKIO promotion, on the identical pin.
     * Since pin_for_macrocell is a FIXED geometric map (exactly one
     * (fb,mc) can ever sit at pin 5, one at pin 6), a USED, non-buried
     * macrocell found there can only be this driver -- the ordinary
     * external-clock case leaves that pin undriven by any macrocell at
     * all, so this scan is a clean positive/negative, not a heuristic.
     *
     * this was GATED `if (NFB < 6)` with
     * ck/cl HARDCODED to pin 5/6 -- i.e. it only ever looked for the
     * driver on XC7318/XC7336. XC7354-PC68's own combination sweep
     * hit the IDENTICAL mechanism (`Z1.CLKF=I7` promoted
     * onto `FCLKIO_0`, `.MAP`: "FB6-3") on a device this scan never
     * covered at all -- ck_drv_fb/cl_drv_fb stayed 0, the exclusion in
     * the checker and the writer below never fired, and PLUSASM's own
     * `pc35` (`Physical pin 'PIN08' occurs more than once`) was the only
     * thing that caught it, 5 of 91 pairs, all one identical signature.
     * `ck_pin`/`cl_pin` below MIRROR the freeze-write branches' own pin
     * choice for every device/package this decoder already measures (0
     * where unmeasured, matching those branches' own "leave unfrozen"
     * fallback) -- generalizing this scan to any of them costs nothing
     * for the cases it already covered (NFB<6 gives ck_pin=5/cl_pin=6,
     * identical to before) and closes the gap for the rest. */
    if (strstr(jf.device, "XC73108")) {
        if (strstr(jf.device, "PC84")) { ck_pin = 9; cl_pin = 10; }
    } else if (strstr(jf.device, "XC7372")) {
        ck_pin = 9; cl_pin = 10;
    } else if (NFB != 6) {
        ck_pin = 5; cl_pin = 6;
    } else if (strstr(jf.device, "PC84")) {
        ck_pin = 9; cl_pin = 10;
    } else if (strstr(jf.device, "PC68")) {
        ck_pin = 8; cl_pin = 9;
    }
    for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++) {
        Cell *c = &cell[fb][mc];
        int p;
        if (!c->used || c->buried) continue;
        /* freeze_pkg, not PKG: ck_pin/cl_pin above are hardcoded in
         * PLUSASM's OWN internal numbering (matching the literal
         * "CK PIN05"-style freeze lines this function writes below),
         * so the comparison must be in that same numbering --
         * closure, see freeze_pkg's own comment. */
        p = pin_for_macrocell(jf.device, freeze_pkg(jf.device), fb, mc);
        if (useclk[0] && ck_pin && p == ck_pin) { ck_drv_fb = fb; ck_drv_mc = mc; }
        if (useclk[1] && cl_pin && p == cl_pin) { cl_drv_fb = fb; cl_drv_mc = mc; }
    }
    if (ck_drv_fb) snprintf(sig[ck_drv_fb][ck_drv_mc],
                            sizeof sig[0][0], "CK");
    if (cl_drv_fb) snprintf(sig[cl_drv_fb][cl_drv_mc],
                            sizeof sig[0][0], "CL");
    if ((ck_drv_fb || cl_drv_fb) && getenv("XC73_TRACE_CKDRV"))
        fprintf(stderr, "CKDRV %s ck=%d/%d cl=%d/%d\n", jedpath,
                ck_drv_fb, ck_drv_mc, cl_drv_fb, cl_drv_mc);

    /* `base` is the OUTBASE, and it may carry a DIRECTORY. The
     * files must still be written where the caller asked, but the PLUSASM
     * DESIGN NAME is an identifier, not a path: `xc7300dump decode board.jed
     * out/board` used to emit `CHIP out/board XEPLD`, which PLUSASM rejects.
     * Take the last path component for the name and keep the full path for
     * the filenames. */
    {
        const char *p1 = strrchr(base, '/'), *p2 = strrchr(base, '\\');
        const char *cut = (p1 > p2) ? p1 : p2;
        snprintf(dname, sizeof dname, "%s", cut ? cut + 1 : base);
    }
    if (!dname[0]) snprintf(dname, sizeof dname, "DESIGN");
    /* A design named F<n> collides with the JEDEC F<n> default-fuse field,
     * a trap already paid for. Say so rather than emit it silently. */
    if ((dname[0] == 'F' || dname[0] == 'f') && dname[1] >= '0' && dname[1] <= '9')
        fprintf(stderr, "warning: design name \"%s\" collides with the JEDEC "
                        "F<n> default-fuse field -- rename it\n", dname);
    /* A term whose literals cannot be resolved used to be dropped by
     * A bare `continue`. A later change fixed exactly that once -- "a partial design
     * is worse than a refusal: it can still round-cycle as a PASS on the pins
     * that remain" -- but not on this path, and the first REAL readback hit
     * it: 25 macrocells, every term dropped, and a .PLD written with an EMPTY
     * EQUATIONS section that PLUSASM cannot assemble. Refuse instead. */
    if (nterm_total == 0) {
        /* this message used to say
         * unconditionally "This is unexpected ... every
         * previously known case is fixed. Please report the
         * bitmap." That claim went stale the moment the dedicated-input/
         * ordinary-macrocell UIM-source slot gap
         * became a KNOWN, OPEN, currently-live cause of this exact
         * refusal -- 7 of the 200-design corpus's own 39 "OTHER"
         * refusals hit this branch, all 7 via that gap, not a new
         * anomaly. Naming a live, understood cause "unexpected" and
         * asking for a bug report on it is a diagnostic that fell behind
         * the decoder's own knowledge (the same "diagnostic lag" class as
         * The cmd_config/all fixes) -- corrected here,
         * without touching the actual (unfixed) addressing gap itself. */
        fprintf(stderr,
            "\nREFUSING to write %s.PLD: %d macrocell(s) are in use and NOT ONE\n"
            "product term could be resolved to a source (%d dropped).\n"
            "An equation-less .PLD does not assemble, and a PARTIAL one is\n"
            "worse -- it can round-cycle as a PASS on the pins that remain.\n"
            "\n"
            "\n"
            "This has more than one known cause. If every dropped term above\n"
            "reads \"source slot N has no package pin\", this is a KNOWN, OPEN\n"
            "gap in the dedicated-input/ordinary-macrocell UIM-source tables\n"
            "for some XC7354/XC7372 packages -- no report\n"
            "needed. Otherwise this is new; please report the bitmap. Useful\n"
            "diagnostics:\n"
            "    XC73_TRACE_ROW=1  xc7300dump decode %s x   (row states)\n"
            "    XC73_TRACE_SLOT=1 xc7300dump decode %s x   (per-term path)\n",
            base, nused, ndrop, jedpath, jedpath);
        jed_free(&jf);
        return 3;
    }
    snprintf(pldname, sizeof pldname, "%s.PLD", base);
    snprintf(vmfname, sizeof vmfname, "%s.VMF", base);
    if (!(pf = fopen(pldname, "wb"))) { perror(pldname); jed_free(&jf); return 1; }
    if (!(vf = fopen(vmfname, "wb"))) { perror(vmfname); fclose(pf); jed_free(&jf); return 1; }

    /*.PLD files must be CRLF traps) */
    fprintf(pf, "TITLE %s\r\n\r\nCHIP %s XEPLD\r\n\r\n", dname, dname);
    /* WITHOUT THESE THREE LINES PLUSASM TREATS THE FILE AS AN
     * INCLUDE and rejects every pin declaration:
     *     cl416: INPUTPIN, OUTPUTPIN, IOPIN, CEPIN and FOEPIN can not be
     *            used in a schematic pld and pld include file
     * The fit still completes, so nothing looks wrong -- but every declared
     * output becomes internal. On a saturated XC73108 the refit collapsed
     * from 102 partitions to **44 macrocells**; with the three lines present
     * it is **102 of 108**.
     *
     * Sparse designs round-cycled anyway because they declare few pins and
     * lose little, which is why 2784 corpus macrocells never showed it.
     * blamed this `cl416` on stale DOSBox state and called
     * that too broad -- it was neither. The EMITTER omits the options. */
    fprintf(pf, "MINIMIZE OFF\r\nLOGIC_OPT OFF\r\n"
                "OPTIONS OFF UIM_OPT REG_OPT CLOCK_OPT FOE_OPT\r\n\r\n");
    /* CARRY THE POWER SETTING. `PWR LOW` is a top-level PLUSASM
     * declaration and blk17 records it per macrocell. `config`
     * decoded it from the first day and the EMITTER never wrote it, so a
     * round cycle silently produced a STANDARD-POWER part from a low-power
     * one. That is not a logic change -- it is a propagation-delay change,
     * which on timing-critical arcade logic is exactly the kind of difference
     * that makes a rebuilt board behave differently on the bench.
     *
     * Found by comparing two real chips: identical rows, identical UIM, and
     * 25 differing blk17 bits. An UNUSED macrocell's config word is all ones,
     * so it reads as LOW spuriously -- only USED macrocells count. */
    /* An output enable references OA or OB and the emitter never
     * DECLARED them, so every decode of a design using `.TRST` failed to
     * assemble: `cl126: The equation input 'OA' is not declared`. PLUSASM
     * takes FOEPIN for a fast output-enable pin (it is in cl416's own list
     * of pin statements). Declare exactly the ones used. */
    for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++)
        if (cell[fb][mc].used && cell[fb][mc].oe_used)
            useoe[cell[fb][mc].oe_src ? 1 : 0] = 1;
    {
        int nlow = 0, nuse = 0;
        for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++)
            if (cell[fb][mc].used) { nuse++; if (cell[fb][mc].lowpwr) nlow++; }
        if (nuse && nlow == nuse)
            fprintf(pf, "PWR LOW\r\n\r\n");
        else if (nlow)
            fprintf(stderr, "warning: %d of %d used macrocells are LOW power. "
                            "`PWR LOW` is a TOP-LEVEL declaration and cannot "
                            "express a mixture, so it is NOT emitted and the "
                            "rebuild will be standard power.\n",
                    nlow, nuse);
    }
    /* EMIT A CLOCK ONLY IF ONE IS USED. The test was
     * `useclk[0] || !useclk[1]`, which emits CK whenever FCLK1 is unused --
     * INCLUDING when the design has no registers at all. The first real chip
     * is entirely combinatorial, so it got an INPUTPIN CK it never needed,
     * pinned to PIN05. PIN05 is FCLK0 *or* FB3_MC2's output, not both, and
     * that chip drives it as an output: the .VMF assigned PIN05 twice and the
     * fit would have failed on the first try. */
    /* The output-enable signal is a PLAIN INPUTPIN. Declaring it
     * `FOEPIN` gives `cl344: A FOE input pin is not allowed in this
     * equation`, and the original designs that fit declare theirs as an
     * ordinary input. Without ANY declaration it was
     * `cl126: The equation input 'OA' is not declared`. */
    /* A dedicated input pad can carry an INPUT REGISTER, which the
     * fitter declares as `INPUTPIN ( RCLK = RC ) A B` with `FASTCLOCK RC`
     * and reports as `I-R` in `.pin`. Emitting those pads as plain inputs
     * dropped the RCLK pin from the design entirely, so the rebuild used one
     * fewer package pin than the original Class A). The registered
     * pads go in their OWN `INPUTPIN` statement, the plain ones in another. */
    for (k = 0; k < 128; k++)
        if (seen_in[k] && input_pad_registered(&jf, k)) {
            if (!rclk_pin) rclk_pin = input_pad_rclk_pin(&jf, k);
            nreg++;
        }
    /* `gt_port4_xc7354.jed`/`gt_s33_port_4.jed`'s own
     * REAL source (`.eqn` on file: `INPUTPIN ( RCLK = CLK )`, `FASTCLOCK
     * CLK`, every register's own `.CLKF = CLK`) uses exactly ONE clock
     * net for BOTH the RCLK-registered input pads AND the ordinary
     * macrocell registers -- there is no separate "CK" and "RC" on the
     * real board at all. This decoder's own emission ALWAYS invents two
     * distinct synthetic names ("CK" for a directly-driven macrocell
     * clock, "RC" for the RCLK pad clock) because it cannot tell from
     * the bitmap alone whether they are the same physical net -- correct
     * when they land on different pins (the common case), but when
     * `rclk_pin` computes to the SAME pin `ck_pin`/`cl_pin` already
     * owns, writing them as two separately-frozen signals produces a
     * genuinely self-conflicting `.VMF` (`CK PIN08` / `RC PIN08` both),
     * which the fix correctly detects and REFUSES
     * -- but a same-pin collision on a REAL, working board is exactly
     * the signature of "these were never two nets," not of "this
     * design cannot exist." Verified directly on a fresh matched pair
     * (`/tmp/ckrc_test/CKRC1.PLD`, both clocks live, UNFROZEN so the
     * real fitter chooses): the fitter puts a lone CK-driven register
     * bank and a lone RCLK pad on DIFFERENT pins (CK->PIN08, RC->PIN09)
     * when they are genuinely separate, confirming decode's OWN
     * ck_pin=8/cl_pin=9 constants are right in the general case --
     * the collision is real evidence of a single shared net, not of a
     * measurement bug in `input_pad_rclk_pin`. Merge: reuse the CK (or
     * CL) name for the RCLK group instead of inventing "RC" whenever
     * the two computed pins coincide. */
    merge_rc_ck = nreg && rclk_pin && useclk[0] && !ck_drv_fb &&
                  rclk_pin == ck_pin;
    merge_rc_cl = !merge_rc_ck && nreg && rclk_pin && useclk[1] &&
                  !cl_drv_fb && rclk_pin == cl_pin;
    rc_name = merge_rc_ck ? "CK" : merge_rc_cl ? "CL" : "RC";
    if (nreg) {
        /* A registered input pad may also carry a CLOCK ENABLE, and
         * the two forms cannot share one statement -- the enable is part of
         * the pad's declaration. Emit the plain group and the enabled group
         * separately. */
        int g, n2;
        for (g = 0; g < 2; g++) {
            n2 = 0;
            for (k = 0; k < 128; k++) {
                if (!seen_in[k] || !input_pad_registered(&jf, k)) continue;
                if ((input_pad_ce(&jf, k) ? 1 : 0) != g) continue;
                if (!n2) { fprintf(pf, g ? "INPUTPIN ( RCLK = %s CE = EN )"
                                         : "INPUTPIN ( RCLK = %s )", rc_name);
                           n2 = 1; nce |= g; }
                fprintf(pf, " I_%d", k);
            }
            if (n2) fprintf(pf, "\r\n");
        }
    }
    /* this SECOND INPUTPIN statement (the plain-input
     * group) used to be written UNCONDITIONALLY, even when it would come
     * out completely empty -- no plain (non-registered) input pin AND no
     * CK/CL flag for this NFB range. `gt_s58_jr1.jed` (XC7354-PC68,
     * NFB==6) has its one input entirely absorbed by the REGISTERED group
     * above (`INPUTPIN ( RCLK = RC ) I_43`), so this statement printed a
     * bare `INPUTPIN\r\n` with nothing after it -- `cl268`, syntax error,
     * no fit. Same defect SHAPE already named for the wired-AND-
     * only case (comment near the top of this file), a different trigger:
     * here every input goes through the nreg/RCLK group instead of
     * through a UIM node. Pre-count so the statement is skipped entirely
     * when it would be empty, exactly as the clock pin already does for the
     * plain OUTPUTPIN group below. */
    {   int nplain = 0;
        for (k = 0; k < 128; k++)
            if (seen_in[k] && !input_pad_registered(&jf, k)) nplain++;
        if (nplain || (useclk[0] && NFB < 6 && !ck_drv_fb) ||
                      (useclk[1] && NFB < 6 && !cl_drv_fb)) {
            fprintf(pf, "INPUTPIN%s%s%s%s",
                    /* this was `NFB == 4`, which declared CK/CL for
                     * XC7336 only. XC7318 has TWO blocks, so it matched
                     * neither this test nor the `NFB == 6` FASTCLOCK
                     * branch and the clock was declared NOWHERE --
                     * `cl126: The equation input 'CK' is not declared`, on
                     * every registered XC7318 design. */
                    /* was `NFB != 6`, so XC7372 (EIGHT blocks)
                     * declared CK as an INPUTPIN here AND as a FASTCLOCK
                     * below -- `cl382: Pin 'CK' has been declared with
                     * different pin types`. Third guard of this shape on
                     * this device; the test must be on the FASTCLOCK
                     * branch's condition, not on a block count. */
                    /* A DRIVEN CK/CL (a macrocell's own product term
                     * promoted onto the FastCLK pin, RH336) is declared
                     * FASTCLOCK below instead -- declaring it INPUTPIN too
                     * gives `cl382: Pin 'CK' has been declared with
                     * different pin types`, same shape as just
                     * above. */
                    (useclk[0] && NFB < 6 && !ck_drv_fb) ? " CK" : "",
                    (useclk[1] && NFB < 6 && !cl_drv_fb) ? " CL" : "",
                    /* OA/OB are declared FOEPIN below, not
                     * INPUTPIN. */
                    "", "");
            for (k = 0; k < 128; k++)
                if (seen_in[k] && !input_pad_registered(&jf, k))
                    fprintf(pf, " I_%d", k);
        }
        /* The trailing "\r\n" a few lines below terminates this
         * statement's line whether or not it was printed -- when skipped,
         * that leaves one blank line, which PLUSASM ignores, the same way
         * the `nreg` block above already leaves one when BOTH of its own
         * groups are empty. */
    }
    /* An output enable was emitted as `Q.TRST = OA`. The
     * fitter ACCEPTS that only by treating OA as an ORDINARY INPUT -- a real
     * FOE signal in an equation is refused outright, `cl344: A FOE input pin
     * is not allowed in this equation`. With one enable nobody noticed; with
     * TWO, the second landed on an I/O pad (67) instead of the FOE pad (64)
     * and the pin sets differed.
     *
     * The fitter's own corpus designs declare it, never equate it:
     *     OUTPUTPIN ( FOE = OA ) S1_3 S1_4
     *     FOEPIN OA
     * The same shape as the `INPUTPIN ( RCLK = RC )`. Outputs are now
     * grouped by which enable drives them, and `.TRST` is gone. */
    /* terminate the INPUTPIN list HERE, unconditionally. It used to
     * be the "\r\n" prefixed to the plain OUTPUTPIN group, so a design whose
     * outputs are ALL FOE-controlled had no plain group, the newline was
     * never written, and `INPUTPIN ... OUTPUTPIN ( FOE = OA ) ...` ran
     * together on one line -- `cl268`. Same defect as earlier, from the
     * opposite direction, and the first version of that fix caused it. */
    fprintf(pf, "\r\n");
    {
        int grp;
        for (grp = -1; grp <= 1; grp++) {
            int any = 0;
            for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++) {
                Cell *c = &cell[fb][mc];
                int g = c->oe_used ? (c->oe_src ? 1 : 0) : -1;
                if (!c->used || c->buried || g != grp) continue;
                /* The FastCLK driver is declared FASTCLOCK, not
                 * OUTPUTPIN -- it does not take a package pin of its own,
                 * it IS the CK/CL net. */
                if ((fb == ck_drv_fb && mc == ck_drv_mc) ||
                    (fb == cl_drv_fb && mc == cl_drv_mc)) continue;
                if (!any) {
                    /* this wrote "\r\n" BEFORE each group, which put
                     * a BLANK LINE between the plain OUTPUTPIN statement and
                     * the FOE ones -- PLUSASM reads that as the end of the
                     * declaration and rejects what follows, `cl268`. The
                     * corpus writes the groups on consecutive lines. */
                    if (grp < 0) fprintf(pf, "OUTPUTPIN");
                    else fprintf(pf, "OUTPUTPIN ( FOE = O%c )", grp ? 'B' : 'A');
                    any = 1;
                }
                fprintf(pf, " %s", sig[fb][mc]);
            }
            if (any) fprintf(pf, "\r\n");
        }
        if (useoe[0] || useoe[1])
            fprintf(pf, "FOEPIN%s%s\r\n",
                    useoe[0] ? " OA" : "", useoe[1] ? " OB" : "");
    }
    {
        int any = 0;
        for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++)
            if (cell[fb][mc].used && cell[fb][mc].buried) any = 1;
        if (any) {
            fprintf(pf, "\r\nNODE");
            for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++)
                if (cell[fb][mc].used && cell[fb][mc].buried)
                    fprintf(pf, " %s", sig[fb][mc]);
            fprintf(pf, "\r\n");
        }
    }
    /* `NODE (UIM)` is a DECLARATION and PLUSASM requires every
     * declaration BEFORE the first `PARTITION`. These were emitted AFTER the
     * partition block, which the assembler rejects with
     *     cl416: Pin-statement can only be used in top-level files!
     * -- a misleading message for a misplaced declaration. On a sparse
     * design there are no wired-AND nodes, so nothing was emitted there and
     * the ordering never mattered; a saturated XC73108 emits 69 of them and
     * the refit collapses from 102 partitions to 44 macrocells.
     *
     * blamed this `cl416` on stale DOSBox state and called
     * that reading too broad. It was neither: the tool writes it. */
    for (k = 0; k < nunode; k++)
        fprintf(pf, "NODE (UIM) %s\r\n", unode[k].name);
    fprintf(pf, "\r\n");
    for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++)
        if (cell[fb][mc].used)
            fprintf(pf, "PARTITION FB%d_%d %s\r\n", fb, mc, sig[fb][mc]);
    /* REFUSE ON ANY DROPPED TERM, not only when NOTHING resolves.
     * made `decode` refuse an equation-less `.PLD`; a design that loses
     * SOME of its product terms is the same defect with a smaller blast
     * radius, and it emitted anyway. A106 lost two of four terms, printed the
     * two dropped lines on stderr, and still wrote a file -- which is how
     * sixteen XC7354 round cycles failed carrying a design that was missing
     * half its logic. A caller that does not read stderr cannot tell. */
    /* THE LITERAL-POLARITY GUARD.
     *
     * Measurement showed that a UIM wired-AND node can invert a signal's LINE
     * for the whole device: `UIM_0 = /P7 * /N_2_3` makes every ordinary row
     * that wants plain `P7` select the COMPLEMENT column, which is correct
     * hardware and reads here as `/P7`. In `D48` that is sixteen literals
     * wrong out of eighteen, and the design fails 15 of 19 pins.
     *
     * The fix needs the per-signal line-inversion fuse and IT IS NOT LOCATED.
     * Guessing a fuse address is how was introduced, so this refuses
     * instead of emitting a design whose literals may be inverted.
     *
     * NEITHER HALF ALONE IS THE CONDITION. Measured over the corpus: sparse
     * historic probes carry 2-3 genuine negations with no UIM node anywhere
     * (`A2`, `B01`, `B03`, `B04`), and `D60`, `B2`, `K64` carry 9, 18 and 27
     * UIM nodes with ZERO negations and round-cycle clean. It is the
     * CONJUNCTION that separates them, on every design measured:
     *
     *     wired-AND + negation   D48, D71, K63      FAIL
     *     wired-AND, no negation D60, B2, K64       PASS
     *     negation, no wired-AND A2, B01, B03, B04  decode unchanged
     *     neither                BN2                PASS
     */
    /* GATED TO XC73108. This guard was built from XC73108 evidence
     * and applied to every device without ever being tested on
     * another one -- the one-block sample in a different costume.
     *
     * Measured on XC7336: the fitter will not even put a negated pin member
     * in the UIM there (`MN` came back as a `Fast function block`), so the
     * line inversion never arises. And `MQ` -- a genuine UIM node
     * PLUS a genuinely negated pad literal, exactly this guard's premise and
     * exactly `gt_hn_K071`'s shape -- ROUND-CYCLES 2 of 2 with the guard
     * bypassed. Refusing it was a FALSE ALARM.
     *
     * XC7354 is still unmeasured, so it keeps the guard: an untested device
     * gets the conservative answer, which is the whole point. */
    /* XC7354 measured too, and it is a false alarm there as well.
     * `NP54` (a UIM node, positive), `NQ54` (a UIM node PLUS a genuinely
     * negated pad literal -- this guard's exact premise) and `NN54` (the
     * fitter declined the UIM entirely) ALL round-cycle 2 of 2 with the
     * guard bypassed, control firing.
     *
     * So the mechanism is XC73108's. On XC7336 and XC7354 the fitter
     * does not produce it, and refusing there only cost real designs --
     * `gt_port4_xc7354` and `gt_hn_K071` were both refused for their whole
     * lives on evidence gathered from a different part.
     *
     * XC7318 and XC7372 remain unmeasured and keep the guard. */
    if (NFB == 12
        && saw_wired_and && (saw_negated_literal || bonded_member) && ndrop == 0
        && !getenv("XC73_NOGUARD")) {
        fprintf(stderr,
            "\nREFUSING to write %s: this design has a wired-AND UIM node AND\n"
            "at least one NEGATED row literal. A UIM node can invert a\n"
            "signal's line device-wide, and this decoder cannot yet\n"
            "tell that apart from a genuinely negated literal -- the fuse that\n"
            "records it is NOT LOCATED. The equations would assemble and fit\n"
            "and behave differently from the chip.\n"
            "  Run with XC73_DUMP_ROWS=1 to see every polarity decision.\n",
            base);
        /* The `.PLD` is already open and partly written by this point -- the
         * refusal below removes it for the same reason. A refusal
         * that leaves a truncated file behind is worse than no refusal. */
        fclose(pf);
        remove(pldname);
        return 4;
    }
    if (ndrop > 0) {
        fprintf(stderr,
            "\nREFUSING to write %s: %d product term(s) could not be\n"
            "resolved and are listed above. A design missing terms is not a\n"
            "partial answer, it is a WRONG one -- it assembles, it fits, and\n"
            "it behaves differently from the chip it came from.\n",
            pldname, ndrop);
        fclose(pf);
        remove(pldname);
        jed_free(&jf);
        return 3;
    }
    /* The RCLK signal is a FASTCLOCK and must be declared, or
     * PLUSASM reports it undeclared.: skip this when
     * merged into CK/CL -- that FASTCLOCK declaration is written below
     * and already covers the (single, shared) signal. */
    if (nreg && !merge_rc_ck && !merge_rc_cl) fprintf(pf, "\r\nFASTCLOCK RC\r\n");
    /* The enable is ACTIVE LOW and declared negated. */
    if (nce) fprintf(pf, "CEPIN /EN\r\n");
    /* CK and CL were emitted as ordinary `INPUTPIN`s. PLUSASM then
     * treats them as plain inputs, so `CK PIN08` in the `.VMF` is REFUSED --
     * `col40: Signal 'CK' assigned to FastClock pin 'PIN08' not used as
     * FastClock` -- which is why the freeze had to be reverted. The
     * signal has to be DECLARED a FASTCLOCK before its pad can be frozen.
     * XC7336 keeps the old form, where the pads are frozen and the part is
     * byte-identical. */
    /* was `NFB == 6`, so XC7372 (EIGHT blocks) froze CK to a pad
     * without declaring it a FastClock -- `col40: Signal 'CK' assigned to
     * FastClock pin 'PIN09' not used as FastClock`. Same shape as */
    if (NFB >= 6 && (useclk[0] || useclk[1]))
        fprintf(pf, "\r\nFASTCLOCK%s%s\r\n",
                useclk[0] ? " CK" : "", useclk[1] ? " CL" : "");
    /* The NFB<6 DRIVEN case -- declare only the driven half(s)
     * FASTCLOCK; an undriven half on the SAME device stays INPUTPIN
     * (above) exactly as before. */
    else if (ck_drv_fb || cl_drv_fb)
        fprintf(pf, "\r\nFASTCLOCK%s%s\r\n",
                ck_drv_fb ? " CK" : "", cl_drv_fb ? " CL" : "");
    fprintf(pf, "\r\nEQUATIONS\r\n\r\n");
    for (k = 0; k < nunode; k++)
        fprintf(pf, "%s = (%s);\r\n", unode[k].name, unode[k].expr);
    for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++) {
        Cell *c = &cell[fb][mc];
        /* `c->nterm == 0` used to skip here, which is right for a
         * macrocell with nothing in it and WRONG for a constant driver, whose
         * term count is legitimately zero. */
        if (!c->used) continue;
        if (c->constval < 0 && c->nterm == 0) continue;
        /* An inverted LHS IS legal PLUSASM input. Emitting the
         * decoded form directly is exact -- no De Morgan, no expansion. */
        if (c->constval >= 0) {
            /* A constant is written exactly as the fitter writes it,
             * with no parentheses -- `/Q1_1 = GND`. */
            fprintf(pf, "%s%s = %s;\r\n", c->inv ? "/" : "", sig[fb][mc],
                    c->constval ? "VCC" : "GND");
            continue;
        }
        fprintf(pf, "%s%s %s (", c->inv ? "/" : "", sig[fb][mc],
                c->reg ? ":=" : "=");
        for (r = 0; r < c->nterm; r++)
            fprintf(pf, "%s%s", r ? " + " : "", c->term[r]);
        fprintf(pf, ");\r\n");
        if (c->reg)
            fprintf(pf, "%s.CLKF = %s;\r\n", sig[fb][mc],
                    c->ptclk ? c->clk_term : (c->clk ? "CL" : "CK"));
        /* guard at the EMIT as well. `c->reg` is not final when
         * `async_row` is chosen, so a cell that ends up COMBINATORIAL could
         * still carry an async term and PLUSASM refuses the file --
         * `cl334: Non-registered '=' equation output has .setf or .rst`. */
        if (c->async_term[0] && c->reg)
            fprintf(pf, "%s.%s = %s;\r\n", sig[fb][mc],
                    (c->async_both || c->async_is_set) ? "SETF" : "RSTF",
                    c->async_term);
        /* async_both: row0 (async_term, above) is always the SET half and
         * row1 (async_term2, here) is always the RESET half -- both emitted
         * on the same macrocell, matching the real fitter's own .EQN. */
        if (c->async_term2[0] && c->reg)
            fprintf(pf, "%s.RSTF = %s;\r\n", sig[fb][mc], c->async_term2);
        /* The enable is in the DECLARATION now, not here. */
    }
    fclose(pf);

    /* if the JEDEC carried no package, the one supplied with
     * `--package` must reach the `.VMF` -- otherwise the gate lets the decode
     * through and XEPLD is still handed a target with no pinout, which is the
     * exact failure the gate exists to prevent. */
    {   char _f[64], _p[32];
        split_part(jf.device, _f, sizeof _f, _p, sizeof _p);
        if (!_p[0] && g_package && g_package[0])
            fprintf(vf, "733644 %s-%s\r\n", _f, g_package);
        else
            fprintf(vf, "733644 %s\r\n", jf.device);
    }
    /* CHECK THE PINLIST BEFORE WRITING IT.
     * A pin claimed twice is a guaranteed fit failure, and the emitter is the
     * only place that can see it -- the fitter will just report a conflict
     * against a source file that looks fine. This is the artifact-level
     * equivalent of a checker's positive control: verify the thing you are
     * about to hand over. */
    {
        int owner[128], p, dup = 0;
        const char *oname[128];
        for (p = 0; p < 128; p++) { owner[p] = 0; oname[p] = NULL; }
        /* PIN05/PIN06 are XC7336's FCLK0/FCLK1, and stopped
         * FREEZING them on other devices -- but left this CHECKER reserving
         * them unconditionally. On PC68 the clock is not frozen at all and
         * pin 5 is a DEDICATED INPUT, so a design legitimately using it was
         * reported as a conflict with a clock that is never written to the
         * `.VMF`. The reservation must match the writer exactly: reserve a
         * pad only when we are about to claim it.
         * generalized from `if (NFB == 4)` (hardcoded pins 5/6)
         * to `ck_pin`/`cl_pin` (mirrors the writer for every device this
         * decoder measures a FastCLK pad for, 0 = none). Was NFB==4-only,
         * same gap class as the writer's own missing guards fixed
         * alongside it -- unlikely to have been reachable in practice (a design
         * where CK is BOTH externally-fed by convention AND happens to
         * physically collide with an ordinary macrocell only needed
         * catching on NFB==4 so far), but there is no reason to leave the
         * same class of gap open right next to the one that just bit. */
        if (ck_pin && useclk[0] && !ck_drv_fb) { owner[ck_pin] = 1; oname[ck_pin] = "CK"; }
        if (cl_pin && useclk[1] && !cl_drv_fb) { owner[cl_pin] = 1; oname[cl_pin] = "CL"; }
        /* `rclk_pin` (the registered-input-pad RCLK
         * pad, written as `RC PIN%02d` near the end of this function) was
         * NEVER added here, so a design with BOTH a directly-driven CK/CL
         * AND a registered input pad whose RCLK resolves to the SAME
         * physical pin (`input_pad_rclk_pin`'s three-way PIN08/09/10-style
         * choice can coincide with `ck_pin`/`cl_pin`) passed this checker
         * with 0 conflicts, then failed at PLUSASM with `pc35: Physical
         * pin 'PINnn' occurs more than once` -- found on two real corpus
         * designs (`gt_port4_xc7354.jed`, `gt_s33_port_4.jed`, both
         * XC7354-PC68, both CK+RC colliding on PIN08) during a
         * round-cycle sweep. Same class as the check,
         * simply never extended to this THIRD pin-claiming path. */
        /* when merge_rc_ck/merge_rc_cl is set, rclk_pin
         * IS ck_pin/cl_pin -- the same physical net, already reserved
         * two lines above under the CK/CL name. Claiming it AGAIN here
         * under a second name ("RC") would report a false collision
         * against the very merge that resolves it. Skip entirely. */
        if (nreg && rclk_pin && !merge_rc_ck && !merge_rc_cl) {
            if (owner[rclk_pin]) {
                fprintf(stderr, "PIN%02d claimed by %s and by RC\n",
                        rclk_pin, oname[rclk_pin]);
                dup++;
            }
            owner[rclk_pin] = 1; oname[rclk_pin] = "RC";
        }
        for (k = 0; k < 128; k++)
            if (seen_in[k]) {
                if (k < 128 && owner[k]) {
                    fprintf(stderr, "PIN%02d claimed by %s and by I_%d\n",
                            k, oname[k], k);
                    dup++;
                }
                owner[k] = 1; oname[k] = "an input";
            }
        for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++) {
            if (!cell[fb][mc].used || cell[fb][mc].buried) continue;
            /* The FastCLK driver macrocell IS the CK/CL pin --
             * it is already reserved above and must not claim it a
             * second time under its own name. */
            if ((fb == ck_drv_fb && mc == ck_drv_mc) ||
                (fb == cl_drv_fb && mc == cl_drv_mc)) continue;
            /* freeze_pkg: `owner[k]` above was populated from `seen_in[]`'s
             * own RAW internal pad numbers (the same numbering the input
             * freeze lines below use) -- comparing against PKG's own
             * DISPLAY numbering here is exactly what produced the
             * own false "PIN42 claimed by an input and by N_3_3" collision
             * (two unrelated pads, one internal-numbered, one PQ44-
             * external-numbered, coinciding by coincidence of digits). */
            p = pin_for_macrocell(jf.device, freeze_pkg(jf.device), fb, mc);
            if (!p || p >= 128) continue;
            if (owner[p]) {
                fprintf(stderr, "PIN%02d claimed by %s and by %s\n",
                        p, oname[p], sig[fb][mc]);
                dup++;
            }
            owner[p] = 1; oname[p] = sig[fb][mc];
        }
        if (dup) {
            fprintf(stderr,
                "\nREFUSING to write %s: %d pin conflict(s). A .VMF that\n"
                "assigns one pin twice fails the fit immediately, and the\n"
                "error points at the source rather than at this tool.\n",
                vmfname, dup);
            fclose(vf); remove(vmfname); remove(pldname);
            jed_free(&jf); return 4;
        }
    }
    /* OA/OB are NOT frozen in the.VMF -- the fitter assigns the
     * FOE pin itself, and forcing one is what `-f` then rejects. */
    /* PIN05/PIN06 are XC7336's FCLK0/FCLK1. On PC68 the FastCLK pads
     * are elsewhere, so freezing these put the clock on the wrong pin and the
     * round cycle failed on PIN SETS DIFFER. Leave the clock UNFROZEN and let
     * The fitter place it, exactly as the output enable is handled. */
    /* "leave the clock unfrozen and let the fitter place it" was
     * only ever a stopgap, and it costs a round cycle whenever the fitter
     * picks a different pad than the original -- 6 of 63 designs in the big
     * campaign landed the clock on PIN04 instead of PIN08.
     *
     * The PC68 FastCLK pads are now MEASURED: 10 probes, five function
     * blocks (FB1, FB2, FB3, FB5, FB6) crossed with the clock frozen to
     * PIN08 and PIN09, every one verified in the fitter's own `.pin`:
     *
     *     FCLK0 = PIN08     FCLK1 = PIN09
     *
     * PIN10 is REFUSED as a macrocell clock on all five blocks
     * (`col42`), so it is not a third FastCLK for this purpose. */
    if (strstr(jf.device, "XC73108")) {
        /* XC73108 fell through to the `NFB != 6` branch below and
         * inherited XC7318's PC44 pads, so a clock the fitter had placed on
         * PIN09 was frozen to PIN05 -- a DEDICATED INPUT here -- and the
         * refit died on `pc36: Port 'CK'(type FCLK) cannot be placed on pin
         * 'PIN05'(type I)`. The round cycle failed at the pins on EVERY
         * registered XC73108 design. No probe has a
         * clock, so nothing caught it until a RANDOMISED design did.
         *
         * MEASURED on PC84 with one, two and three distinct clocks (with only
         * ONE the fitter puts them on the same pad and nothing distinguishes
         * them):
         *
         *     one clock    CKA -> PIN09
         *     two clocks   CKA -> PIN09   CKB -> PIN10
         *     three        the third gets NO clock pad; PIN12 is CLK-type but
         *                  the fitter uses it as an OUTPUT instead
         *
         * PC84 therefore has exactly TWO FastCLK input pads:
         *     FCLK0 = PIN09     FCLK1 = PIN10
         * The same pair XC7372 uses on this package, which is what
         * a shared package should give.
         *
         * OTHER PACKAGES ARE NOT MEASURED. Freezing a guessed pad is exactly
         * how this defect arose, so leave the clock unfrozen there and say
         * so rather than write a pin that may not be a clock pad at all. */
        if (strstr(jf.device, "PC84")) {
            /* guard added -- see the NFB!=6 branch's own comment
             * a few lines down for why an unguarded freeze here duplicates
             * the driver macrocell's own pin entry when the clock is
             * promoted rather than external. Unmeasured whether XC73108-
             * PC84 can actually reach the promoted-clock case; guarding
             * costs nothing if it never fires. */
            if (useclk[0] && !ck_drv_fb) fprintf(vf, "CK PIN09\r\n");
            if (useclk[1] && !cl_drv_fb) fprintf(vf, "CL PIN10\r\n");
        } else if (useclk[0] || useclk[1]) {
            fprintf(stderr, "note: XC73108 FastCLK pads are measured on PC84 "
                    "only; leaving the clock\n      unfrozen on this package. "
                    "The refit may place it on a different pin.\n");
        }
    } else if (strstr(jf.device, "XC7372")) {
        /* XC7372's CLK pads are PIN09 and PIN10 on PC84, measured
         * with BOTH clocks live in one design -- with only one in
         * use the fitter puts them on the same pad and nothing distinguishes
         * them). `NFB != 6` sent this device down XC7318's PC44 path and
         * froze CK to PIN05, which is a DEDICATED INPUT here: the emitter
         * then wrote PIN05 twice and PLUSASM refused the file --
         * `pc35: Physical pin 'PIN05' occurs more than once`.
         * guard added, same reasoning as the NFB!=6 branch below. */
        if (useclk[0] && !ck_drv_fb) fprintf(vf, "CK PIN09\r\n");
        if (useclk[1] && !cl_drv_fb) fprintf(vf, "CL PIN10\r\n");
    } else if (NFB != 6) {
        /* XC7318 shares PC44 with XC7336 and its CLK pads are the
         * same two, pins 5 and 6 -- read from the fitter's own `.pin`. */
        /* A DRIVEN CK/CL is not fed externally -- freezing it to
         * a pin here is the wrong direction (that pin is an OUTPUT of the
         * driver macrocell's own logic, not an input the .VMF should
         * claim). Leave it unfrozen and let the fitter place it, exactly
         * as the undriven case already leaves OA/OB unfrozen. */
        if (useclk[0] && !ck_drv_fb) fprintf(vf, "CK PIN05\r\n");
        if (useclk[1] && !cl_drv_fb) fprintf(vf, "CL PIN06\r\n");
    } else if (strstr(jf.device, "PC84")) {
        /* this branch used to write PIN08/PIN09
         * UNCONDITIONALLY for every XC7354 package -- the pads
         * were measured on PC68 ONLY, and nothing here ever checked
         * which package the JEDEC actually named. On PC84, PIN08 is
         * VSS (measured directly)
         * -- freezing a clock there cannot even assemble
         * (`pc36: Port 'CK'(type FCLK) cannot be placed on pin
         * 'PIN08'(type VCC,VSS or MR)`), which is exactly the T7354
         * round-cycle failure this fix closes.
         *
         * MEASURED on XC7354-PC84, two clocks live in one design (the
         * same precaution used -- with only one clock the
         * fitter puts it on either pad and nothing distinguishes them):
         * CKA -> PIN09, CKB -> PIN10 -- the SAME pair PC84 already
         * gives XC7372 and XC73108, matching "a
         * shared package should give the same pads" exactly.
         * guard added, same reasoning as the NFB!=6 branch. */
        if (useclk[0] && !ck_drv_fb) fprintf(vf, "CK PIN09\r\n");
        if (useclk[1] && !cl_drv_fb) fprintf(vf, "CL PIN10\r\n");
    } else if (strstr(jf.device, "PC68")) {
        /* now that CK/CL are declared FASTCLOCK (above), the pads
         * can be frozen: FCLK0 = PIN08, FCLK1 = PIN09.
         * `useclk[]` is set only for macrocells whose blk45/46 says
         * REGISTERED-from-FastCLK, so a product-term-clocked design never
         * reaches here and never declares a FastClock it does not use.
         * MEASURED ON PC68 ONLY -- kept as the fallback for
         * PC68 and for a bare device string with no package suffix; any
         * OTHER XC7354 package is unmeasured territory, same as
         * XC73108's "OTHER PACKAGES ARE NOT MEASURED" note just above.
         *
         * "a product-term-clocked design
         * never reaches here" (comment above) turned out to mean "never
         * reaches here WITHOUT ALSO being registered from FastCLK" --
         * The P004 (`Z1.CLKF=I7`, promoted by the REAL FITTER onto
         * FCLKIO_0) sets `useclk[0]` (Z1 IS registered from FastCLK once
         * promoted) while ALSO being driven by a macrocell (FB6-3) at
         * this exact pin, and this branch had no `!ck_drv_fb` guard --
         * unlike the NFB!=6 branch below, which got one and
         * was never generalized here. Fixed: 5 of 91 pairs' `pc35`
         * REFIT_FAILED verdicts traced to exactly this. */
        if (useclk[0] && !ck_drv_fb) fprintf(vf, "CK PIN08\r\n");
        if (useclk[1] && !cl_drv_fb) fprintf(vf, "CL PIN09\r\n");
    } else if (useclk[0] || useclk[1]) {
        fprintf(stderr, "note: XC7354 FastCLK pads are measured on PC68 "
                "and PC84 only; leaving the clock\n      unfrozen on "
                "this package. The refit may place it on a different "
                "pin.\n");
    }
    /* An earlier freeze WITHOUT the FASTCLOCK declaration failed. Writing `CK PIN08` fixes
     * the designs whose clock wandered, and BREAKS the ones where the fitter
     * routed the clock as an ORDINARY INPUT rather than a FastClock --
     * `col40: Signal 'CK' assigned to FastClock pin 'PIN08' not used as
     * FastClock`, and 6 designs went from a pin-set failure to NO FIT AT ALL.
     * `P20019`'s own `.pin` shows PIN08 with type CLK and use `I`.
     * The pad is measured (below); WHICH DESIGNS ACTUALLY USE THE FASTCLOCK
     * PATH is not, and that is what the freeze needs. */
    /* PG144/BG225's dedicated pads carry no numeric
     * pin -- `k` here is the arbitrary RANK `x108_pad_addr` assigned them
     * (main.c). Translate rank -> real ball name for these two packages
     * only; every other package keeps the numeric PIN%02d form
     * unchanged. `x108_pad_ball` returns 0 (and leaves `ballbuf` alone)
     * for any other package, so the fallback is exactly the pre-existing
     * line. */
    for (k = 0; k < 128; k++) if (seen_in[k]) {
        char ballbuf[8];
        if (x108_pad_ball(&jf, k, ballbuf, sizeof ballbuf))
            fprintf(vf, "I_%d PIN%s\r\n", k, ballbuf);
        else
            fprintf(vf, "I_%d PIN%02d\r\n", k, k);
    }
    /* freeze the RCLK pad too, or the fitter picks a CLK pad of its
     * own and the rebuild lands on a different pin than the original. */
    /* XC7372/PC84's RCLK pad is PIN12 and its CEN pad is PIN74 --
     * read from the fitter's own `.pin`, which labels them `CLK` and `CEN`.
     * `rclk_pin` comes from XC7354's clock-select table and `EN PIN60` was
     * hard-coded, so every registered-input design rebuilt with the wrong
     * pads and the round cycle failed on PIN SETS DIFFER. */
    /* when merged, CK's (or CL's) own freeze line
     * above/below already claims this exact pin under that name -- do
     * NOT also write "RC PINxx" for the identical physical pin, or the.
     * VMF is self-conflicting again, just under the opposite name. */
    if (nreg && rclk_pin && !merge_rc_ck && !merge_rc_cl)
        fprintf(vf, "RC PIN%02d\r\n", rclk_pin);
    /* CEN pad on PC68, measured. */
    /* PIN74 is the CEN pad AND FB3_MC3's output pad -- the two are
     * mutually exclusive. Freezing `EN` there while a macrocell drives it
     * made PLUSASM refuse the file (`pc35: Physical pin 'PIN74' occurs more
     * than once`). Let the fitter place `EN` itself when the pad is taken;
     * it is a CEN pin either way and the round cycle compares PINS, so a
     * collision is a wrong file while a free choice is not. */
    if (nce) {
        int cen_taken = 0;
        if (strstr(jf.device, "XC7372")) {
            int f2, m2;
            if (device_pin_macrocell(device_find(jf.device), PKG, 74, &f2, &m2)
                && cell[f2][m2].used && !cell[f2][m2].buried) cen_taken = 1;
        }
        if (!cen_taken)
            fprintf(vf, strstr(jf.device, "XC7372") ? "EN PIN74\r\n"
                                                    : "EN PIN60\r\n");
    }
    for (fb = 1; fb <= NFB; fb++) for (mc = 1; mc <= 9; mc++) {
        int p;
        if (!cell[fb][mc].used || cell[fb][mc].buried) continue;
        /* The FastCLK driver is not an ordinary output pin to
         * freeze under its own name -- it IS CK/CL, whose freeze (or
         * deliberate non-freeze) is handled above. Freezing it here too
         * duplicated the entry this loop exists to avoid (this is the
         * SAME loop shape as the owner[] conflict check a few hundred
         * lines up -- both walk every used, non-buried macrocell). */
        if ((fb == ck_drv_fb && mc == ck_drv_mc) ||
            (fb == cl_drv_fb && mc == cl_drv_mc)) continue;
        /* PG144/BG225 have no numeric pin --
         * `pin_for_macrocell` always returns 0 for them (no entry in
         * `device_pin_macrocell`'s tables), so this freeze line was
         * silently skipped for every non-buried macrocell on these two
         * packages (harmless only because `pin_can_drive`, above, used
         * to bury EVERY one of them first -- now that it does not,
         * skipping the freeze here would leave the fitter to place the
         * output on ANY pin it likes, which is not what the original
         * bitmap's own placement was). Freeze it to the real ball via
         * `device_macrocell_ball` instead. */
        if (strcasecmp(PKG, "PG144") == 0 || strcasecmp(PKG, "BG225") == 0) {
            char ballbuf[8];
            const device_t *dd = device_find(jf.device);
            if (dd && device_macrocell_ball(dd, PKG, fb, mc, ballbuf, sizeof ballbuf))
                fprintf(vf, "%s PIN%s\r\n", sig[fb][mc], ballbuf);
            continue;
        }
        /* freeze_pkg, not PKG: this IS the .VMF freeze directive
         * PLUSASM reads back on a -f refit -- closure. */
        p = pin_for_macrocell(jf.device, freeze_pkg(jf.device), fb, mc);
        if (p) fprintf(vf, "%s PIN%02d\r\n", sig[fb][mc], p);
    }
    fprintf(vf, "; CHIP %s\r\n", dname);
    fclose(vf);

    printf("%s -> %s and %s\n", jedpath, pldname, vmfname);
    /* this hint said `-p 7336` on every device, so an XC7354 decode
     * printed instructions that refit it as the wrong part. A stale string in
     * The tool's own output is the class an earlier sweep checked, and this one
     * survived because no XC7354 decode had ever run when it was written. */
    {
        const char *dnum = jf.device;
        while (*dnum && (*dnum < '0' || *dnum > '9')) dnum++;
        {
            char num[8]; size_t i = 0;
            while (dnum[i] && dnum[i] >= '0' && dnum[i] <= '9' && i < sizeof num - 1)
                { num[i] = dnum[i]; i++; }
            num[i] = '\0';
            if (!num[0]) snprintf(num, sizeof num, "7336");
            printf("\n  Recompile with:\n"
                   "     PLUSASM -e %s -p %s\n"
                   "     FITEQN  -e %s -p %s -f\n"
                   "     MAKEJED -n %s -s <signature>\n",
                   base, num, base, num, base);
        }
    }
    printf("\n  Signal NAMES are not in a bitmap and are not recovered:\n"
           "  I_<n> is \"the signal on package pin n\", N_<fb>_<mc> a macrocell\n"
           "  output. The round cycle compares BEHAVIOUR AT PINS,\n"
           "  so the names may differ from the original; the pins may not.\n");
    jed_free(&jf);
    return 0;
}

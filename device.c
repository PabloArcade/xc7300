/* device.c -- XC7300 family device table.
 *
 * SOURCES, and which parts are measured versus taken from the datasheet:
 *
 *   macrocell / FB / pin counts   XC7300 family datasheet table, and the
 *                                 XC7336 datasheet
 *   QF (fuse count)               MEASURED from real MAKEJED output. Only the
 *                                 two marked so are measured; the rest are
 *                                 UNKNOWN and deliberately left 0 rather than
 *                                 estimated. A wrong QF would silently mis-map
 *                                 every fuse, which is worse than no answer.
 *   pin tables                    XC7336 PC44/PQ44 from the datasheet pinout,
 *                                 cross-checked against a FITEQN .PIN report.
 */

#include "device.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* XC7336 PC44 -- datasheet "XC7336 Pinouts", Output column.
 * Index is the PC44 package pin; value is the macrocell it drives, encoded
 * as fb*100 + mc (so FB1_MC1 = 101). 0 = not a macrocell output pin. */
/* Pin 1 was previously 306
 * (FB3_MC6) here, taken from the datasheet's "MC3-6" annotation on this
 * MR-shared pin -- WRONG: only TWO of
 * these 36 macrocell-pin entries were EVER confirmed against a real
 * FITEQN .PIN report (pin1 was not one of them; it rode in on the
 * datasheet's own text like the other 34). A fresh, controlled test
 * (a 1-macrocell design forcing PARTITION FB3_6 onto an
 * OUTPUTPIN, `-p XC7336-5PC44` AND the historical bare `-p 7336`, both
 * resolving to "Target Device: XC7336-5PC44") gets, both times:
 * `es31:[Error] Could not fit equation 'O36' into FB3-6! Associated chip
 * pin supports only nodes.` -- and the SAME design retargeted
 * `-p XC7336-5PQ44` fails identically. FB3-6 is the ONLY macrocell, of
 * all 36 on this die, whose `.PAR` "Pin Avl" column ever prints a bare
 * `-` (every other slot, including the OTHER MR/FCLK/FOE dual-role
 * pins, prints a real type and places cleanly). This is a genuine,
 * package-independent property of the die: FB3_MC6 has NO package pin
 * at all, on either package tried -- it is buried-only, full stop, not
 * a decoder table bug. Every one of this project's own PLD sources that
 * ever partitions FB3_6 (grep "PARTITION FB3_6\b") already targets a
 * NODE there, never an OUTPUTPIN -- the corpus already "knew" this by
 * construction; only this literal table disagreed. 0, not 306. */
static const short pc44_mc[45] = {
    0,
    /*  1 */ 0,   /*  2 */ 305, /*  3 */ 304, /*  4 */ 303, /*  5 */ 302,
    /*  6 */ 301, /*  7 */ 101, /*  8 */ 102, /*  9 */ 103, /* 10 */ 0,
    /* 11 */ 104, /* 12 */ 105, /* 13 */ 106, /* 14 */ 107, /* 15 */ 108,
    /* 16 */ 109, /* 17 */ 409, /* 18 */ 408, /* 19 */ 407, /* 20 */ 406,
    /* 21 */ 0,   /* 22 */ 405, /* 23 */ 0,   /* 24 */ 404, /* 25 */ 403,
    /* 26 */ 402, /* 27 */ 401, /* 28 */ 0,   /* 29 */ 209, /* 30 */ 208,
    /* 31 */ 0,   /* 32 */ 0,   /* 33 */ 207, /* 34 */ 206, /* 35 */ 205,
    /* 36 */ 204, /* 37 */ 203, /* 38 */ 202, /* 39 */ 201, /* 40 */ 309,
    /* 41 */ 0,   /* 42 */ 0,   /* 43 */ 308, /* 44 */ 307
};

/* Special-function pins on PC44.
 *
 * CORRECTED. The previous version mixed PQ44 pin numbers into
 * the PC44 table -- FOE at 33/34, VCCIO at 26, VCCINT/VPP at 35, a GND at 25,
 * an input at 22 -- which are all PQ44 numbers for these functions. The
 * XC7336 datasheet prints PQ44 and PC44 in ONE table with different numbers
 * per package, and warned about exactly this.
 *
 * Every entry below is confirmed against the FITTER'S OWN .PIN report, which
 * is independent of the datasheet: MR 1, CLK 5/6, VSS 10/23/31, VCC 21/32/41,
 * input-only 28/42, FOE 39/40. */
static const char *pc44_special(int pin) {
    switch (pin) {
    case 1:  return "MR";        /* I/FO/FI/MR */
    case 5:  return "FCLK0";     /* FO/FCLK0   */
    case 6:  return "FCLK1";     /* FO/FCLK1   */
    case 39: return "FOE1";      /* FO/FOE1, drives MC2-1 */
    case 40: return "FOE0";      /* FO/FOE0, drives MC3-9 */
    case 10: case 23: case 31: return "GND";
    case 32: return "VCCIO";
    case 21: return "VCCINT";
    case 41: return "VCCINT/VPP";
    case 28: case 42: return "I";   /* input only (I/FI) */
    default: return NULL;
    }
}

/* Macrocell and function-block counts are the XC7300 family table from the
 * product specification. QF is MEASURED: a trivial
 * design fitted on each device and the count read from MAKEJED's own output,
 * re-measured.
 *
 * XC73144 has NO ACCEPTED SPELLING. `-p 73144`, `73144A`, `7144` and `731441`
 * are all rejected by FITEQN with "Missing or invalid device name", although
 * XACT/DATA/73144.SPD exists. Its QF is therefore genuinely unknown and is
 * reported as such -- NOT estimated from the macrocell count, because a
 * plausible wrong QF mis-maps every fuse and is harder to catch than an
 * absent one. */
/* XC7354 PC68 macrocell pinout, extracted from the FITTER'S OWN.MAP
 * reports across designs touching all six blocks -- 49 of 54 macrocells.
 * A 0 means the macrocell has NO PACKAGE PIN on PC68: the part has 54
 * macrocells and the package cannot bond them all out, so those five can only
 * be used as BURIED nodes. That is why they never fitted as outputs. */
static const int pc68_mc[6][9] = {
    /* FB1 */ {4,12,13,15,17,19,21,22,23},
    /* FB2 */ {66,58,57,56,55,52,51,48,46},
    /* FB3 */ {0,0,60,61,62,64,44,45,47},
    /* FB4 */ {38,53,54,0,0,0,39,40,42},
    /* FB5 */ {32,33,35,37,16,18,25,31,36},
    /* FB6 */ {27,28,8,9,10,29,6,24,26},
};

/* PC68 special-function pins, from a .PIN report:
 *   MR 1 | input-only 2,3,5,11,43,65 | VSS 7,14,34,41,49
 *   VCC 20,30,50,59,63 | CLK 8,9,10 | CEN 60,61 | FOE 62,64
 * Note 8,9,10 and 60,61,62,64 are DUAL-FUNCTION: they are also the I/O pins
 * of FB6_MC3..MC5 and FB3_MC3..MC6 respectively. */
static const char *pc68_special(int pin) {
    switch (pin) {
    case 1: return "MR";
    case 8: case 9: case 10: return "CLK";
    case 60: case 61: return "CEN";
    case 62: case 64: return "FOE";
    case 7: case 14: case 34: case 41: case 49: return "GND";
    case 20: case 30: case 50: case 59: case 63: return "VCC";
    case 2: case 3: case 5: case 11: case 43: case 65: return "I";
    default: return NULL;
    }
}

/* XC7318 / PC44, MEASURED. The tool had NO pin table for this
 * device, so `pin_for_macrocell` returned 0 for every macrocell and the
 * emitter buried ALL of them -- "FB1_MC1 drives PIN00, which is unusable".
 * Nothing on XC7318 could round-cycle, and nobody had tried.
 *
 * Read from the fitter in ONE design placing all eighteen macrocells and
 * reading its `.MAP`:
 *
 *   FB1_MC1..9 ->  7  8  9 11 12 13 14 15 16
 *   FB2_MC1..9 -> 39 38 37 36 35 34 33 30 29
 *
 * FB2_MC1's pad (39) is ALSO an FOE pin, exactly as FB3_MC6's pad is MR on
 * XC7336/PC44. Dedicated inputs: 2 3 4 17 18 19 20 22 24 25 26 27
 * 28 42 43 44. CLK 5, 6. FOE 39, 40. MR 1. */
static const int pc44_18_mc[45] = {
    0,
    0,   0,   0,   0,   0,   0, 101, 102, 103,   0,   /*  1..10 */
  104, 105, 106, 107, 108, 109,   0,   0,   0,   0,   /* 11..20 */
    0,   0,   0,   0,   0,   0,   0,   0, 209, 208,   /* 21..30 */
    0,   0, 207, 206, 205, 204, 203, 202, 201,   0,   /* 31..40 */
    0,   0,   0,   0                                  /* 41..44 */
};

static const char *pc44_18_special(int pin) {
    switch (pin) {
    case 1:  return "MR";
    case 5: case 6:   return "CLK";
    case 39: case 40: return "FOE";
    case 10: case 23: case 31: return "GND";
    case 21: case 32: case 41: return "VCC";
    case 2: case 3: case 4: case 17: case 18: case 19: case 20: case 22:
    case 24: case 25: case 26: case 27: case 28: case 42: case 43: case 44:
        return "I";
    default: return NULL;
    }
}

/* XC7354 / PC44, MEASURED. `PARTLIST.XCT` lists
 * `7354PC44` and the tool resolved PC44 as XC7336 or XC7318 only, so an
 * `XC7354-*PC44` bitmap decoded against the WRONG PIN TABLE entirely.
 * 54 probes, one macrocell per design, read from the fitter's `.MAP`:
 * 29 of 54 macrocells have a pad and TWENTY-FIVE HAVE NONE -- the 44-pin
 * package strands nearly half the die, with FB3, FB4 and FB5 almost
 * entirely unreachable. FB6_MC3/MC4 land on pins 5 and 6, the CLK pads. */
/* pin 40 was 0, missing FB3_MC5 (305) -- found by
 * cross-referencing `xact/X/XACT/DATA/MODEL.CHP`'s
 * own `735444` column against a genuine, independently-fetched XC7354
 * datasheet (fpgakey.com, distinct from the XC7336 mirror
 * used), both agreeing; CONFIRMED by a real fitter
 * (`-p 7354PC44`, `PARTITION FB3_5` forced onto an `OUTPUTPIN`, `.MAP`
 * places it at pad 40 with zero errors). Wired. */
static const int pc44_54_mc[45] = {
    0,
    0,   0,   0,   0, 603, 604,   0, 102, 103,   0,   /*  1..10 */
  104, 105, 106, 107, 108, 109, 507, 601, 602, 606,   /* 11..20 */
    0, 508,   0, 509, 407, 408, 409,   0, 209, 208,   /* 21..30 */
    0,   0, 207, 206, 205, 204, 203, 202, 303, 305,   /* 31..40 */
    0,   0,   0,   0                                  /* 41..44 */
};

/* XC7372 / PC84, MEASURED. 60 of 72 macrocells have a pad; FB3 and
 * FB8 are almost entirely unbonded in this package. Read from the fitter's
 * `.MAP`, one macrocell per design, 72 probes. */
static const int pc84_72_mc[85] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0, 803,
  804, 809, 805, 102, 103, 104,   0, 105, 106, 107,
  108, 109,   0, 701, 702, 703, 706,   0, 707, 708,
  709, 601, 602, 603, 704, 705, 604, 605,   0, 606,
  607, 608,   0, 609, 501, 502, 503, 504, 505,   0,
  506, 405, 404, 507, 508, 509, 401, 402, 406, 403,
    0, 407, 408, 409,   0, 209, 208, 207, 206, 205,
  204, 203, 202,   0, 303, 304, 305, 306,   0,   0,
    0,   0,   0,   0,   0
};

/* XC73108 / PC84, MEASURED -- one pin-driving macrocell per design,
 * 108 probes, 60 fitted and 48 refused as unbonded. FB3, FB4, FB11 and FB12
 * are entirely unbonded and MC1 is unbonded in EVERY block. FB5's four
 * bonded cells land on the CEN/FOE pads (74..77) and FB10's on the CLK pads
 * (9..12), shared exactly as XC7372 shares them. Encoded fb*100 + mc. */

/* XC73144 / PQ160, from the VENDOR's own chip database
 * (`model6.chp`, Foundation 1.5i). Extraction method VALIDATED first on
 * XC73108/PC84, where the same parser reproduced all 60 measured pads
 * exactly -- a positive control before trusting a device never fitted here.
 *
 * 144 macrocells across SIXTEEN blocks (FFB 4 + FB 12,; 124 bonded
 * on PQ160. Encoded fb*100 + mc.
 *
 * CONFIRMED against the XEPLD 6.0.1 fitter running under DOSBox,
 * on the first XC73144 designs this project has ever fitted:
 *
 *   the 20 UNBONDED macrocells, from the fitter's own `hi811` refusals when
 *   asked to place an output on every one of the 144:  20 of 20 exact
 *   the pin -> macrocell assignment, from the `.PIN` and `.MAP` of a design
 *   driving all 124 bonded macrocells:               124 of 124 exact
 *
 * The fitter also states the geometry directly -- `Function Blocks 16/16,
 * Macrocells 124/144` and "137 I/O pins available on XC73144-10PQ160". */
static const int pq160_144_mc[161] = {
        0,     0,   704,   305,   201,   303,   705,   701,
      706,   702,     0,     0,   707,     0,   708,     0,
      709,     0,     0,     0,     0,     0,     0,     0,
        0,  1401,     0,  1402,     0,  1408,     0,     0,
     1407,  1403,  1406,  1404,   101,  1409,   403,   405,
        0,     0,  1405,   601,   102,   602,     0,   103,
      603,   104,   604,     0,   407,   409,   105,   605,
      106,   607,   107,   108,   109,     0,  1201,  1202,
     1203,  1306,  1307,   606,  1206,   608,     0,  1207,
      609,  1208,   501,  1209,   502,  1101,   503,  1102,
        0,     0,  1103,  1308,   504,  1309,  1204,   505,
     1205,   506,  1104,   507,  1105,   508,     0,  1106,
      509,  1107,  1108,     0,     0,  1109,  1001,  1002,
     1003,  1601,  1004,  1602,  1005,  1603,     0,  1006,
     1604,   905,  1605,   904,  1609,  1007,   806,   807,
        0,     0,  1008,  1606,  1009,  1607,   901,     0,
     1608,  1507,  1501,   808,   809,  1508,   902,   906,
      903,     0,   907,   908,   909,     0,   209,   208,
      207,  1509,   206,  1502,   205,   309,   307,  1503,
      204,  1504,   203,  1505,   202,     0,  1506,   703,
        0
};

static const int pc84_108_mc[85] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,1003,
 1004,1009,1005, 102, 103, 104,   0, 105, 106, 107,
  108, 109,   0, 901, 902, 903, 906,   0, 907, 908,
  909, 801, 802, 803, 904, 905, 804, 805,   0, 806,
  807, 808,   0, 809, 701, 702, 703, 704, 705,   0,
  706, 605, 604, 707, 708, 709, 601, 602, 606, 603,
    0, 607, 608, 609,   0, 209, 208, 207, 206, 205,
  204, 203, 202,   0, 503, 504, 505, 506,   0,   0,
    0,   0,   0,   0,   0
};

/* XC7236A / PC44, MEASURED -- one pin-driving macrocell per design,
 * 36 probes, 34 fitted. The die has no record in `model6.chp`, so unlike
 * every other package table here this one came from FITS, not the vendor
 * file. Encoded fb*100 + mc. */
static const int pc44_36a_mc[45] = {
        0,     0,   201,     0,     0,   204,   205,     0,   206,
      207,   208,   209,     0,   101,   102,   103,   104,     0,
      105,   106,   107,   108,   109,     0,   409,   408,   407,
      406,   405,     0,   404,   403,   402,   401,     0,   309,
      308,   307,   306,     0,   305,   304,   303,   302,   301
};

/* BALL-GRID packages. A BGA position is alphanumeric (`F3`,
 * `E1`) and will not fit the integer pin tables, so these are keyed by
 * STRING. Extracted from `model6.chp` by the parser that reproduces
 * three measured pinouts exactly. NOT bitmap-confirmed. */
typedef struct { const char *ball; short fbmc; } ball_t;

static const ball_t bg225_108_mc[] = {   /* ZINFANDEL 73108, 108 macrocell balls */
    {"N3",101}, {"P4",102}, {"P5",103}, {"N6",104},
    {"P7",105}, {"R6",106}, {"P8",107}, {"R8",108},
    {"N8",109}, {"B1",201}, {"A2",202}, {"C5",203},
    {"A3",204}, {"B5",205}, {"C6",206}, {"B7",207},
    {"A6",208}, {"A7",209}, {"F14",1201}, {"E15",1202},
    {"D15",1203}, {"E13",1204}, {"B15",1205}, {"A14",1206},
    {"C11",1207}, {"A12",1208}, {"C13",1209}, {"B10",1101},
    {"A5",1102}, {"A4",1103}, {"B4",1104}, {"B3",1105},
    {"C3",1106}, {"C10",1107}, {"A11",1108}, {"B6",1109},
    {"K2",1001}, {"L1",1002}, {"N2",1003}, {"M3",1004},
    {"P3",1005}, {"P1",1006}, {"L3",1007}, {"M1",1008},
    {"P2",1009}, {"R9",901}, {"R10",902}, {"P9",903},
    {"M14",904}, {"N15",905}, {"N10",906}, {"R12",907},
    {"P12",908}, {"P13",909}, {"N12",801}, {"P14",802},
    {"N14",803}, {"M15",804}, {"K14",805}, {"J13",806},
    {"J15",807}, {"H14",808}, {"G13",809}, {"G15",701},
    {"F15",702}, {"G14",703}, {"F13",704}, {"E14",705},
    {"D14",706}, {"B14",707}, {"B13",708}, {"B12",709},
    {"A13",601}, {"B9",602}, {"A10",603}, {"D13",604},
    {"C14",605}, {"C9",606}, {"B8",607}, {"A8",608},
    {"C8",609}, {"E3",501}, {"E2",502}, {"B2",503},
    {"C2",504}, {"D2",505}, {"C1",506}, {"F2",507},
    {"G2",508}, {"F1",509}, {"R2",401}, {"N5",402},
    {"R4",403}, {"P6",404}, {"N7",405}, {"P10",406},
    {"R7",407}, {"R11",408}, {"R13",409}, {"N11",301},
    {"R14",302}, {"N13",303}, {"P15",304}, {"L13",305},
    {"L14",306}, {"K13",307}, {"L15",308}, {"K15",309},
    { NULL, 0 }
};

static const ball_t bg225_144_mc[] = {   /* CABERNET 73144, 144 macrocell balls */
    {"N3",101}, {"P4",102}, {"P5",103}, {"N6",104},
    {"P7",105}, {"R6",106}, {"P8",107}, {"R8",108},
    {"N8",109}, {"B1",201}, {"A2",202}, {"C5",203},
    {"A3",204}, {"B5",205}, {"C6",206}, {"B7",207},
    {"A6",208}, {"A7",209}, {"F14",1601}, {"E15",1602},
    {"D15",1603}, {"E13",1604}, {"B15",1605}, {"A14",1606},
    {"C11",1607}, {"A12",1608}, {"C13",1609}, {"B10",1501},
    {"A5",1502}, {"A4",1503}, {"B4",1504}, {"B3",1505},
    {"C3",1506}, {"C10",1507}, {"A11",1508}, {"B6",1509},
    {"K2",1401}, {"L1",1402}, {"N2",1403}, {"M3",1404},
    {"P3",1405}, {"P1",1406}, {"L3",1407}, {"M1",1408},
    {"P2",1409}, {"M10",1301}, {"L10",1302}, {"L12",1303},
    {"K12",1304}, {"K11",1305}, {"L11",1306}, {"M11",1307},
    {"J12",1308}, {"G12",1309}, {"R9",1201}, {"R10",1202},
    {"P9",1203}, {"M14",1204}, {"N15",1205}, {"N10",1206},
    {"R12",1207}, {"P12",1208}, {"P13",1209}, {"N12",1101},
    {"P14",1102}, {"N14",1103}, {"M15",1104}, {"K14",1105},
    {"J13",1106}, {"J15",1107}, {"H14",1108}, {"G13",1109},
    {"G15",1001}, {"F15",1002}, {"G14",1003}, {"F13",1004},
    {"E14",1005}, {"D14",1006}, {"B14",1007}, {"B13",1008},
    {"B12",1009}, {"A13",901}, {"B9",902}, {"A10",903},
    {"D13",904}, {"C14",905}, {"C9",906}, {"B8",907},
    {"A8",908}, {"C8",909}, {"F12",801}, {"E12",802},
    {"D11",803}, {"E10",804}, {"D10",805}, {"E11",806},
    {"D12",807}, {"D9",808}, {"D7",809}, {"E3",701},
    {"E2",702}, {"B2",703}, {"C2",704}, {"D2",705},
    {"C1",706}, {"F2",707}, {"G2",708}, {"F1",709},
    {"R2",601}, {"N5",602}, {"R4",603}, {"P6",604},
    {"N7",605}, {"P10",606}, {"R7",607}, {"R11",608},
    {"R13",609}, {"N11",501}, {"R14",502}, {"N13",503},
    {"P15",504}, {"L13",505}, {"L14",506}, {"K13",507},
    {"L15",508}, {"K15",509}, {"K4",401}, {"L4",402},
    {"M4",403}, {"M5",404}, {"L5",405}, {"L6",406},
    {"M7",407}, {"M6",408}, {"M9",409}, {"F5",301},
    {"F4",302}, {"J4",303}, {"E4",304}, {"G4",305},
    {"E6",306}, {"E5",307}, {"D6",308}, {"D5",309},
    { NULL, 0 }
};


/* package pinouts from the VENDOR chip database `model6.chp`
 * (Foundation 1.5i). The extractor was positive-controlled against the
 * three tables this project MEASURED and reproduces them exactly:
 *     XC7372/PC84   60 of 60      XC73108/PC84  60 of 60
 *     XC73144/PQ160 124 of 124
 * XC7354/PC44 DISAGREES with the measured table by about two pins and
 * the MEASURED one is kept -- a vendor file does not outrank a fit.
 * These tables are NOT bitmap-confirmed. Encoded fb*100 + mc. */

/* The vendor table above was WRONG -- a uniform
 * +1 PIN SHIFT against every fitter-confirmed position, plus three
 * positions (FB6 MC1/MC2/MC6) the vendor source omitted outright and
 * zero-filled. Found and replaced by building two exhaustive designs
 * (`PINCHK3`/`PINCHK4`) that
 * PARTITION every one of XC7354's 54 macrocells to a distinct pin,
 * -p 7354PC84, and reading the fitter's own `.PIN`+`.MAP` directly.
 * ALL 54 macrocells bond to a real pin on PC84 -- "51 bonded" in the
 * comment above is also wrong for this device; there are no gaps.
 * Zero duplicate macrocells, zero conflicts between the two designs'
 * overlapping declarations. This is now the MEASURED table (matches
 * this project's own naming rule: not vendor-sourced, not
 * "NOT bitmap-confirmed" -- ground truth from a real fit). */
static const int pc84_54_mc[85] = {   /* XC7354 735484, all 54 bonded */
        0,     0,     0,     0,   101,     0,   607,     0,     0,   603,
      604,     0,   605,     0,   102,   103,     0,   104,   505,   105,
      506,   106,     0,     0,     0,     0,   107,     0,   108,   109,
      608,   507,   609,   601,     0,     0,   602,   606,     0,   508,
      501,   502,     0,   503,   509,   504,   401,   407,   408,     0,
      409,   404,   405,     0,   307,   308,   206,   309,   406,   207,
        0,     0,     0,     0,     0,   208,   209,   402,   403,   202,
      203,   204,   205,     0,   303,   304,   305,   301,     0,   302,
      306,     0,   201,     0,     0
};

/* DELETED, (was `pc68_54_mc`, XC7354/PC68). Its own
 * 3-entry gap (pins 27/28/29, FB6_MC1/MC2/MC6) was found and about to
 * be fixed from two independent vendor sources agreeing with
 * `pc44_54_mc`'s own sibling gap -- until checking `device_pin_
 * macrocell()`'s own dispatch showed this table is NEVER READ at all:
 * `vt[]` has no `("XC7354","PC68")` entry, and the SEPARATE PC68
 * special-case block just below it unconditionally handles XC7354/PC68
 * through `pc68_mc[6][9]` instead, independently
 * measured, extensively real-corpus-exercised). Fixed-then-checked
 * anyway, as a cross-validation: the corrected array is BYTE-FOR-BYTE
 * IDENTICAL to `pc68_mc` at all 49 bonded positions, 0 mismatches --
 * confirming the fix was right (independent agreement with an
 * already-live, already-measured table) while confirming the table
 * itself was always dead weight. Deleted rather than kept fixed-but-
 * dead, same reasoning as the `pc44_36_mc` deletion above (and that
 * comment's own now-corrected mistake, which named this table as a
 * "live" counter-example when it was not). */

/* SAME +2 PIN SHIFT bug as `pq100_108_mc` above, found and fixed the
 * same way. Also currently UNREACHABLE:
 * XC7372/PQ100 is "NOT MEASURABLE with this toolchain" at all,
 * PLUSASM produces no output -- absent from PARTLIST.XCT),
 * so this fix is data-grounds-only, zero regression risk, same as
 * above. */
static const int pq100_72_mc[101] = {   /* XC7372 7372100, 72 bonded */
        0,   304,   201,   305,   306,     0,     0,   307,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,   808,
        0,     0,   803,   804,   101,   809,     0,   805,   102,   103,
      104,     0,   105,   106,   801,   107,   108,   109,     0,   701,
      702,   703,   706,   802,     0,   707,   806,   708,   709,   601,
      602,     0,   603,   704,   705,   604,   605,     0,   606,   807,
      607,   608,     0,   609,   501,   502,   503,   504,   505,     0,
      506,   405,   404,   301,   507,     0,   508,   509,   401,   302,
      308,   402,   406,   403,     0,   407,   408,   409,     0,   209,
      208,   207,   309,   206,   205,   204,   203,   202,     0,   303,
        0
};

static const int pc68_72_mc[69] = {   /* XC7372 737268, 47 bonded */
        0,     0,     0,     0,     0,     0,     0,     0,   803,   804,
      805,   102,   103,   104,     0,   105,   106,   107,   108,   109,
        0,   706,   707,   708,   709,   601,   602,   603,   604,   605,
        0,   606,   607,   608,     0,   609,   501,   502,   503,   504,
      505,     0,   506,   507,   508,   509,   401,   402,   403,     0,
        0,   209,   208,   207,   206,   205,   204,   203,   202,     0,
      303,   304,   305,     0,     0,     0,     0,     0,     0
};

static const int pq160_108_mc[161] = {   /* XC73108 73108160, 108 bonded */
        0,     0,   504,     0,   201,     0,   505,   501,   506,   502,
        0,     0,   507,     0,   508,     0,   509,     0,     0,     0,
        0,     0,     0,     0,     0,  1001,     0,  1002,     0,  1008,
        0,     0,  1007,  1003,  1006,  1004,   101,  1009,     0,     0,
        0,     0,  1005,   401,   102,   402,     0,   103,   403,   104,
      404,     0,     0,     0,   105,   405,   106,   407,   107,   108,
      109,     0,   901,   902,   903,     0,     0,   406,   906,   408,
        0,   907,   409,   908,   301,   909,   302,   801,   303,   802,
        0,     0,   803,     0,   304,     0,   904,   305,   905,   306,
      804,   307,   805,   308,     0,   806,   309,   807,   808,     0,
        0,   809,   701,   702,   703,  1201,   704,  1202,   705,  1203,
        0,   706,  1204,   605,  1205,   604,  1209,   707,     0,     0,
        0,     0,   708,  1206,   709,  1207,   601,     0,  1208,  1107,
     1101,     0,     0,  1108,   602,   606,   603,     0,   607,   608,
      609,     0,   209,   208,   207,  1109,   206,  1102,   205,     0,
        0,  1103,   204,  1104,   203,  1105,   202,     0,  1106,   503,
        0
};

/* A +2 PIN SHIFT (with
 * wraparound) against `xact/X/XACT/DATA/MODEL.CHP`'s own `73108100`
 * column -- the SAME shape as the already-fixed `pc84_54_mc`
 * "+1 pin shift" bug, applied to this table too. Testing every integer
 * shift -10..+10 finds exactly one bijective, zero-residual match:
 * shift=+2 reproduces MODEL.CHP's 72 nonzero entries at 100/100
 * positions. Currently UNREACHABLE in practice: `decode` refuses even
 * a trivial 1-macrocell XC73108/PQ100 design for an unrelated reason
 * (the "NOT ONE product term could be resolved", still
 * open) -- so this fix carries zero regression risk to anything this
 * toolchain can currently decode, and is applied on data grounds
 * alone (a mechanical shift, not a live-fit judgement call). */
static const int pq100_108_mc[101] = {   /* XC73108 73108100, 72 bonded */
        0,   504,   201,   505,   506,     0,     0,   507,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,  1008,
        0,     0,  1003,  1004,   101,  1009,     0,  1005,   102,   103,
      104,     0,   105,   106,   407,   107,   108,   109,     0,   901,
      902,   903,   906,   408,     0,   907,   409,   908,   909,   801,
      802,     0,   803,   904,   905,   804,   805,     0,   806,   309,
      807,   808,     0,   809,   701,   702,   703,   704,   705,     0,
      706,   605,   604,  1209,   707,     0,   708,   709,   601,  1107,
     1108,   602,   606,   603,     0,   607,   608,   609,     0,   209,
      208,   207,  1109,   206,   205,   204,   203,   202,     0,   503,
        0
};

static const int pc68_108_mc[69] = {   /* XC73108 7310868, 47 bonded */
        0,     0,     0,     0,     0,     0,     0,     0,  1003,  1004,
     1005,   102,   103,   104,     0,   105,   106,   107,   108,   109,
        0,   906,   907,   908,   909,   801,   802,   803,   804,   805,
        0,   806,   807,   808,     0,   809,   701,   702,   703,   704,
      705,     0,   706,   707,   708,   709,   601,   602,   603,     0,
        0,   209,   208,   207,   206,   205,   204,   203,   202,     0,
      503,   504,   505,     0,     0,     0,     0,     0,     0
};

/* These were "26 bonded", missing 10 of 36 -- read
 * the XC7336 datasheet's own printed "XC7336 Pinouts" table (verified
 * against its own Figure 1 first) and found FB1_MC1 (pin1), FB3_MC3..8
 * (pins42-37), FB4_MC6..8 (pins14-12) all zero where the datasheet shows
 * A bonded macrocell. Confirmed NOT bitmap-tested at all when built:
 * these four tables came from XACT6's own `model6.chp`-family
 * vendor extractor, package coverage only -- "vendor tables are tried
 * only for pairs the MEASURED tables do not cover", never round-cycled).
 *
 * This item ran the real fitter (`-p XC7336-5PQ44`, an explicit
 * PARTITION-forced OUTPUTPIN on each of the 10 target macrocells,
 * a test design) and read the resulting
 * `.MAP`/`.PIN`: 9 of the 10 place cleanly onto a real package pin,
 * confirmed pin-for-pin against the SAME datasheet table (FB1_MC1->1,
 * FB3_MC3/4/5->42/41/40, FB3_MC7/MC8->38/37, FB4_MC6/7/8->14/13/12).
 * **The 10th, FB3_MC6 (PQ44 pin 39, the MR-shared pin), does NOT** --
 * see `pc44_mc`'s own comment above for the `.PAR` evidence (identical
 * refusal on `-p XC7336-5PC44`, `-p 7336`, and `-p XC7336-5PQ44` alike:
 * "Associated chip pin supports only nodes"). Wiring 9, not 10 -- FB3_MC6
 * stays 0, now for a MEASURED reason instead of an unmeasured gap.
 * 35 bonded (was 26). */
static const int pq44_36_mc[45] = {   /* XC7336 733644PQ, 35 bonded */
        0,   101,   102,   103,     0,   104,   105,   106,   107,   108,
      109,   409,   408,   407,   406,     0,   405,     0,   404,   403,
      402,   401,     0,   209,   208,     0,     0,   207,   206,   205,
      204,   203,   202,   201,   309,     0,     0,   308,   307,     0,
      305,   304,   303,   302,   301
};

/* CONFIRMED UNREACHABLE: device_pin_macrocell()'s
 * own `vt[]` dispatch table has no ("XC7336","PC44") entry -- that pair
 * resolves through `pc44_mc` instead (this file's final fallback
 * chain). No code path, no test, ever reads this array; the compiler's
 * own `-Wunused-const-variable` already flags it at every build,
 * standing-close. Its name also collides with the *convention*
 * `pq44_36_mc` uses for a table that IS the live source -- a hazard to
 * a future reader, not merely dead weight. CORRECTION: this comment
 * originally also named `pc68_54_mc` as an
 * example of a live table by the same convention -- WRONG, found
 * while fixing that exact table's own 3-entry gap: `pc68_54_mc` is
 * ALSO unreachable (same `vt[]` gap, no `("XC7354","PC68")` entry --
 * that pair resolves through the older, independently-measured
 * `pc68_mc[6][9]` 2D table instead, and is now
 * DELETED for the identical reason, below. Deleted rather
 * than fixed: nothing wires it, and fixing dead code just to leave it
 * dead is not "not introducing errors", it is spending review effort on
 * something with zero live effect. */

static const int vq44_36q_mc[45] = {   /* XC7336Q 733644QVQ, 35 bonded --
                                          same fix as pq44_36_mc; the
                                          XC7336Q datasheet text states
                                          "timing parameters ... are
                                          identical" to XC7336 and prints
                                          no separate VQ44 pinout table,
                                          so this array's own byte-for-
                                          byte-identical-to-pq44_36_mc
                                          convention (pre-existing, not
                                          established here) is
                                          carried forward unchanged --
                                          NOT independently re-fit under
                                          `-p XC7336Q...`
                                          (disclosed, not chased). */
        0,   101,   102,   103,     0,   104,   105,   106,   107,   108,
      109,   409,   408,   407,   406,     0,   405,     0,   404,   403,
      402,   401,     0,   209,   208,     0,     0,   207,   206,   205,
      204,   203,   202,   201,   309,     0,     0,   308,   307,     0,
      305,   304,   303,   302,   301
};

static const int pq44_36q_mc[45] = {   /* XC7336Q 733644QPQ, 35 bonded --
                                          same fix and same caveat as
                                          vq44_36q_mc above. */
        0,   101,   102,   103,     0,   104,   105,   106,   107,   108,
      109,   409,   408,   407,   406,     0,   405,     0,   404,   403,
      402,   401,     0,   209,   208,     0,     0,   207,   206,   205,
      204,   203,   202,   201,   309,     0,     0,   308,   307,     0,
      305,   304,   303,   302,   301
};

static const int pc44_36q_mc[45] = {   /* XC7336Q 733644QPC, 35 bonded --
                                          PC44 numbering (matches pc44_mc
                                          exactly, FB3_MC6 included, since
                                          this table already had FB3_MC6
                                          at 0 before this change touched
                                          it -- the ORIGINAL vendor
                                          extractor got that one entry
                                          right by omission, same as
                                          pq44_36_mc's own pre-fix state;
                                          only the other 9 needed fixing). */
        0,     0,   305,   304,   303,   302,   301,   101,   102,   103,
        0,   104,   105,   106,   107,   108,   109,   409,   408,   407,
      406,     0,   405,     0,   404,   403,   402,   401,     0,   209,
      208,     0,     0,   207,   206,   205,   204,   203,   202,   201,
      309,     0,     0,   308,   307
};


static const device_t devices[] = {
    /* name      fbs  mcs  ffb_mcs  qf       qf_measured */
    { "XC7318",    2,  18,   9,     128960, 1 },   /* MEASURED -- SAME as XC7336 */
    { "XC7336",    4,  36,   9,     128960, 1 },   /* MEASURED, XC7336-5PC44 */
    { "XC7354",    6,  54,   9,     243648, 1 },   /* MEASURED, XC7354-10PC68 */
    { "XC7372",    8,  72,   9,     260032, 1 },   /* MEASURED */
    { "XC73108",  12, 108,   9,     779136, 1 },   /* MEASURED */
    { "XC73144",  16, 144,   9,          0, 0 },   /* no accepted -p spelling */
    /* both appear in XACT 6.01's PARTLIST.XCT. XC7336Q has its own
     * CHIP record in `model6.chp` -- FFB 4, FB 0, COLUMNS 7 -- so it is
     * XC7336's shape with a different column count and a VQ44 package it
     * does not share. XC7236A has NO chip record anywhere and only a PC44
     * package and timing grades, so its geometry is UNKNOWN and its entry
     * carries zeros rather than a guess. */
    { "XC7336Q",   4,  36,   9,          0, 0 },   /* QF NOT measured */
    /* MEASURED BY FITTING ONE. The die has no chip record in
     * `model6.chp`, no entry in any manual, and measurement showed it as
     * "geometry UNKNOWN" -- but `PARTLIST.XCT` lists `7236APC44` with a
     * `7236a.die` and `7236A.SPD`, both present, and the XACT 5 toolchain
     * fits it. `.PIN` says `Target Device: XC7236A-16PC44`, `.MAP` reports
     * FOUR function blocks with 3 fast inputs each, `.RES` reports 36
     * macrocells, and MAKEJED writes QF128400. */
    { "XC7236A",   4,  36,   9,     128400, 1 },   /* MEASURED, XC7236A-16PC44 */
};

#define NDEV ((int)(sizeof(devices) / sizeof(devices[0])))

const device_t *device_find(const char *name) {
    int i;
    if (!name) return NULL;
    for (i = 0; i < NDEV; i++) {
        size_t n = strlen(devices[i].name);
        if (strncasecmp(name, devices[i].name, n) == 0) {
            /* longest match wins: XC73108 must not match XC7318's prefix */
            const device_t *best = &devices[i];
            size_t bn = n;
            int j;
            for (j = i + 1; j < NDEV; j++) {
                size_t m = strlen(devices[j].name);
                if (m > n && strncasecmp(name, devices[j].name, m) == 0
                           && m > bn) { best = &devices[j]; bn = m; }
            }
            /* The matched name is only a
             * PREFIX of `name` -- legitimate when what follows is a speed
             * grade / package (`XC7336-5PC44`, `-` or a digit), but a
             * device string like `XC73108A` or `XC7318B` silently matched
             * its own base part here with NO warning, for a suffix letter
             * that names no registered device at all (`XC73108A` is not in
             * `devices[]`; `XC7336Q` IS, and the loop above already picks
             * it over `XC7336`'s own shorter match -- this warns only when
             * NO longer registered name explains the extra letter). */
            if (name[bn] >= 'A' && name[bn] <= 'Z')
                fprintf(stderr,
                    "NOTE: device string '%s' is not a registered part -- "
                    "reading it as its prefix '%s' (no `%.*sX...` device is "
                    "known for any letter X). If '%s' is a genuinely "
                    "different die, this is WRONG.\n",
                    name, best->name, (int)bn, name, name);
            return best;
        }
    }
    return NULL;
}

const device_t *device_nth(int i) {
    if (i < 0 || i >= NDEV) return NULL;
    return &devices[i];
}

int device_count(void) { return NDEV; }

/* A BGA position is a STRING. */
int device_ball_macrocell(const device_t *dev, const char *package,
                          const char *ball, int *fb, int *mc) {
    const ball_t *t = NULL;
    if (!dev || !package || !ball) return 0;
    /* PG144 is a PIN grid and BG225 a BALL grid, but the vendor
     * table gives IDENTICAL designators on all 144 XC73108 rows -- the two
     * packages share a footprint, so one table serves both. */
    if (strcasecmp(package, "BG225") != 0 &&
        strcasecmp(package, "PG144") != 0) return 0;
    if      (strcasecmp(dev->name, "XC73108") == 0) t = bg225_108_mc;
    else if (strcasecmp(dev->name, "XC73144") == 0) t = bg225_144_mc;
    if (!t) return 0;
    for (; t->ball; t++)
        if (strcasecmp(t->ball, ball) == 0) {
            *fb = t->fbmc / 100; *mc = t->fbmc % 100; return 1;
        }
    return 0;
}

/* The reverse of device_ball_macrocell -- same table,
 * same gating, queried by (fb,mc) instead of by ball name. Needed because
 * PG144/BG225 have no numeric pin at all; decode.c's ordinary
 * pin_for_macrocell()/device_pin_macrocell() path can never resolve a
 * package position for these two, and every macrocell was silently
 * treated as unbonded (buried) as a result -- confirmed by direct test,
 * not assumed. This table is a COMPLETE
 * 108-of-108 enumeration for XC73108 (every FBn_MCm, n=1..12, m=1..9,
 * appears exactly once) so there is no partial-coverage risk the way
 * PC84/PQ160's numeric tables have for special (MR/GND/VCC) pins -- every
 * entry here is, by the table's own provenance, a real
 * macrocell ball and nothing else. */
int device_macrocell_ball(const device_t *dev, const char *package,
                          int fb, int mc, char *out, size_t outsz) {
    const ball_t *t = NULL;
    short want;
    if (!dev || !package || !out || outsz == 0) return 0;
    if (strcasecmp(package, "BG225") != 0 &&
        strcasecmp(package, "PG144") != 0) return 0;
    if      (strcasecmp(dev->name, "XC73108") == 0) t = bg225_108_mc;
    else if (strcasecmp(dev->name, "XC73144") == 0) t = bg225_144_mc;
    if (!t) return 0;
    want = (short)(fb * 100 + mc);
    for (; t->ball; t++)
        if (t->fbmc == want) {
            size_t n = strlen(t->ball);
            if (n >= outsz) return 0;
            memcpy(out, t->ball, n + 1);
            return 1;
        }
    return 0;
}

int device_pin_macrocell(const device_t *dev, const char *package, int pin,
                         int *fb, int *mc) {
    if (!dev || !package) return 0;
    /* PC68 from the fitter's own.MAP -- 49
     * of 54 macrocells, the other five having no package pin -- and the table
     * has sat in this file marked __attribute__((unused)) ever since, while
     * this resolver refused every device but XC7336/PC44. then
     * CONFIRMED the whole FB6 row against nine fits. Wired in. */
    /* vendor-sourced package tables. Tried BEFORE the measured
     * paths only for (device, package) pairs the measured tables do not
     * cover -- a fit outranks a vendor file wherever both exist. */
    {
        struct { const char *dev, *pkg; const int *t; int n; } vt[] = {
            { "XC7354",  "PC84",  pc84_54_mc,   85 },
            { "XC7372",  "PQ100", pq100_72_mc, 101 },
            { "XC7372",  "PC68",  pc68_72_mc,   69 },
            { "XC73108", "PQ160", pq160_108_mc,161 },
            { "XC73108", "PQ100", pq100_108_mc,101 },
            { "XC73108", "PC68",  pc68_108_mc,  69 },
            { "XC7336",  "PQ44",  pq44_36_mc,   45 },
            { "XC7336Q", "VQ44",  vq44_36q_mc,  45 },
            { "XC7336Q", "PQ44",  pq44_36q_mc,  45 },
            { "XC7336Q", "PC44",  pc44_36q_mc,  45 },
            { "XC7236A", "PC44",  pc44_36a_mc,  45 },
        };
        size_t k;
        for (k = 0; k < sizeof vt / sizeof vt[0]; k++)
            if (strcasecmp(dev->name, vt[k].dev) == 0 &&
                strcasecmp(package, vt[k].pkg) == 0) {
                if (pin < 1 || pin >= vt[k].n || !vt[k].t[pin]) return 0;
                *fb = vt[k].t[pin] / 100; *mc = vt[k].t[pin] % 100;
                return 1;
            }
    }
    if (strcasecmp(package, "PC68") == 0) {
        int f, m;
        if (strcasecmp(dev->name, "XC7354") != 0) return 0;
        if (pin < 1 || pin > 68) return 0;
        for (f = 0; f < 6; f++)
            for (m = 0; m < 9; m++)
                if (pc68_mc[f][m] == pin) { *fb = f + 1; *mc = m + 1; return 1; }
        return 0;
    }
    if (strcasecmp(dev->name, "XC73108") == 0) {
        if (strcasecmp(package, "PC84") != 0) return 0;
        if (pin < 1 || pin > 84 || !pc84_108_mc[pin]) return 0;
        *fb = pc84_108_mc[pin] / 100; *mc = pc84_108_mc[pin] % 100; return 1;
    }
    if (strcasecmp(dev->name, "XC7372") == 0) {
        if (strcasecmp(package, "PC84") != 0) return 0;
        if (pin < 1 || pin > 84 || !pc84_72_mc[pin]) return 0;
        *fb = pc84_72_mc[pin] / 100; *mc = pc84_72_mc[pin] % 100; return 1;
    }
    if (strcasecmp(package, "PC44") != 0) return 0;
    if (pin < 1 || pin > 44) return 0;
    if (strcasecmp(dev->name, "XC7354") == 0) {
        int v = pc44_54_mc[pin];
        if (pin == 40) { *fb = 3; *mc = 5; return 1; }
        if (!v) return 0;
        *fb = v / 100; *mc = v % 100; return 1;
    }
    if (strcasecmp(dev->name, "XC7318") == 0) {
        if (!pc44_18_mc[pin]) return 0;
        *fb = pc44_18_mc[pin] / 100;
        *mc = pc44_18_mc[pin] % 100;
        return 1;
    }
    if (strcasecmp(dev->name, "XC7336") != 0) return 0;
    if (!pc44_mc[pin]) return 0;
    *fb = pc44_mc[pin] / 100;
    *mc = pc44_mc[pin] % 100;
    return 1;
}

const char *device_pin_function(const device_t *dev, const char *package,
                                int pin) {
    if (!dev || !package) return NULL;
    if (strcasecmp(package, "PC68") == 0)
        return (strcasecmp(dev->name, "XC7354") == 0) ? pc68_special(pin) : NULL;
    if (strcasecmp(package, "PC44") != 0) return NULL;
    if (strcasecmp(dev->name, "XC7318") == 0) return pc44_18_special(pin);
    if (strcasecmp(dev->name, "XC7336") != 0) return NULL;
    return pc44_special(pin);
}

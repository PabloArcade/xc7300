# xc7300dump

A JEDEC/bitmap decompiler for Xilinx XC7300-family EPLDs, written for arcade
PCB preservation.

Given the fuse map of a programmed XC7300 part (a `.jed` file, or a raw
programmer readback), `xc7300dump decode` reconstructs a design that the
original Xilinx XACT/XEPLD toolchain can compile back into an equivalent
bitmap. The goal is functional equivalence at the pins: the rebuilt part
behaves like the original on every output pin, so a failed or unreadable
chip on a board can be replaced.

## Supported devices and packages

| Device  | Packages                         | Notes |
|---------|----------------------------------|-------|
| XC7318  | PC44                             | |
| XC7336  | PC44                             | |
| XC7354  | PC44, PC68, PC84                 | |
| XC7372  | PC68, PC84                       | PQ100 has a pin table but is unverified |
| XC73108 | PC84, PQ160, PG144, BG225        | PQ100: designs using the dedicated input pads are refused |
| XC7236A | PC44                             | recognised; designs using its UIM crosspoint are refused (see Limitations) |
| XC73144 | -                                | recognised and refused: no available toolchain produces its true fuse map |

The device and package are taken from the JEDEC header. A bitmap that does
not state its package is refused rather than decoded on an assumption.

## What it outputs

`decode` writes two files for the XACT XEPLD flow:

- `<outbase>.PLD`: the design as XEPLD PLUSASM source (pin declarations,
  `PARTITION` statements that pin every macrocell to its original location,
  and the equations).
- `<outbase>.VMF`: the pin assignment, used with `FITEQN -f` to freeze every
  signal to the package pin it occupied on the original part.

Signal names are not stored in a bitmap and are not recovered. Inputs are
named `I_<pin>` (the signal on package pin `<pin>`) and macrocell outputs
`N_<fb>_<mc>`. Names will differ from the original design; pin behaviour
will not.

## Building

Linux (or any C99 compiler):

    make

This produces `xc7300dump`.

Windows, cross-compiled with mingw-w64 (statically linked, no DLLs needed):

    make windows

This produces `xc7300dump.exe`. The Windows build embeds a build timestamp
in its PE header, so two builds of the same source never have the same
checksum; the Linux build is deterministic.

Check the build:

    ./xc7300dump selftest           # must report no failures, exit 0
    ./xc7300dump selftest --fail    # deliberately breaks a known value; must FAIL, exit 1

## Usage

Every command prints the tool's version line first. Examples use a real
XC7336 bitmap, `board.jed`.

**Identify a bitmap**

    xc7300dump info board.jed

Prints the device, fuse count (QF), the JEDEC record structure and a fuse
census.

**Decompile**

    xc7300dump decode board.jed BOARD

    JEDEC recognized
    target            : XC7336-PC44
    board.jed -> BOARD.PLD and BOARD.VMF

      Recompile with:
         PLUSASM -e BOARD -p 7336
         FITEQN  -e BOARD -p 7336 -f
         MAKEJED -n BOARD -s <signature>

The resulting `BOARD.PLD` for a small design:

    TITLE BOARD
    CHIP BOARD XEPLD
    MINIMIZE OFF
    LOGIC_OPT OFF
    OPTIONS OFF UIM_OPT REG_OPT CLOCK_OPT FOE_OPT
    INPUTPIN I_8 I_11 I_42
    OUTPUTPIN N_1_1
    PARTITION FB1_1 N_1_1
    EQUATIONS
    N_1_1 = (I_8*/I_11 + I_42*/I_8);

When a bitmap uses a structure the tool cannot decode with certainty,
`decode` refuses to write a `.PLD` and names the reason, rather than emit a
design that could be wrong.

**Read the MAKEJED signature**

    xc7300dump signature board.jed

Use the same signature with `MAKEJED -s` when rebuilding, so a bit-level
comparison is not polluted by a signature difference.

**Compare two bitmaps**

    xc7300dump rdiff board.jed rebuilt.jed

Record-by-record comparison that names the function block, macrocell, row
and slot of every difference. Prefer it to the flat `diff`, which lists raw
fuse addresses only:

    xc7300dump diff board.jed rebuilt.jed --max 50

**Convert a programmer readback to JEDEC**

    xc7300dump readback chip.prg chip.jed template.jed

`chip.prg` is an Intel HEX dump of the part as read by a device programmer.
`template.jed` is any MAKEJED-produced bitmap for the same device; its record
set tells `readback` which addresses hold fuse data. The output decodes like
any other `.jed`. `readback` reports how much of the image it left out.

**Inspect one macrocell**

    xc7300dump used board.jed            # macrocells in use
    xc7300dump config board.jed 1 1      # FB1 MC1 configuration bits
    xc7300dump rows board.jed 1 1        # FB1 MC1 product-term rows
    xc7300dump uim board.jed             # UIM (interconnect) crosspoints

**Raw access**

    xc7300dump dump board.jed 0 64       # 64 fuse values from address 0
    xc7300dump col board.jed 1 1         # one macrocell's column across blocks

**Device information**

    xc7300dump devices                   # supported devices and fuse counts
    xc7300dump pins XC7354 PC68          # package pin -> macrocell map

**Fitter database diagnostic**

    xc7300dump vmhroutes design.vmh

Counts the framed `bits`/`routes` records in an XEPLD `.VMH` design
database. It is a diagnostic, not a route decoder. Its `--selftest` is
skipped in this release (exit 0) because its `.VMH` fixtures are not
included.

## The round cycle

The tool is checked by rebuilding a decoded design with the original vendor
fitter and confirming that the rebuilt part behaves identically.

1. Decode the original bitmap:

       xc7300dump decode board.jed BOARD
       xc7300dump signature board.jed           # note the signature, e.g. TEST

2. Rebuild with XACT (5.0.0 runs under DOSBox). Run in an empty directory
   that contains only `BOARD.PLD` and `BOARD.VMF`:

       PLUSASM -e BOARD -p 7336
       FITEQN  -e BOARD -p 7336 -f
       MAKEJED -n BOARD -s TEST

   `-f` freezes the pin assignment from `BOARD.VMF`. Without it the fitter
   may move inputs to other package pins, and the rebuilt part is not a
   drop-in replacement. `FITEQN -f` requires `BOARD.VMF` to exist before
   `PLUSASM` runs.

3. Compare:

       xc7300dump rdiff board.jed BOARD.JED

   Fuse-level differences are expected and are not by themselves a fault.
   The fitter is free to order product terms or allocate shared resources
   differently for the same logic. What must match is behaviour at the
   pins.

4. Confirm behaviour by decoding the rebuild and comparing the two designs
   pin by pin:

       xc7300dump decode BOARD.JED BOARD2

   `BOARD.PLD` and `BOARD2.PLD` must describe the same function on every
   output pin (the same equations, possibly with terms in a different
   order). For registered designs, compare by simulation over random input
   sequences rather than by reading the text.

Judge a fitter run by whether it produced a `.JED`, not by its exit code;
some failures exit cleanly and write nothing.

## Limitations

- XC7236A: designs that use the device's UIM crosspoint are not decoded; they are refused.
- XC73108: a minority of very dense designs are refused (a 7th or later shared product term on certain slots, or three or more UIM wired-AND claimants on one line).
- FASTINPUT declarations are refused on every device.
- XC73108 in PQ100: designs that use the dedicated input pads are refused.
- XC73144 cannot be decoded; no available toolchain produces its true fuse map.
- Signal names are not recovered; only pin-level behaviour is.

In every refused case the tool exits with an error and names the reason. It
does not write a partial or guessed design.

## Credit

xc7300dump by DARKSOFT.

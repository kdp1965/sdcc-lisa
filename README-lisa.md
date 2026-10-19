# SDCC for LISA (Little ISA)

This tree is upstream SDCC (github.com/swegener/sdcc mirror) plus a `lisa`
port: `sdcc -mlisa`, the assembler `sdaslisa`, the linker `sdldlisa` and the
runtime library `device/lib/lisa`.  Design notes: `src/lisa/PLAN.md`.

## Building (macOS, this machine)

The tree was regenerated with autoconf 2.73, which makes `AC_PROG_CC` pick
`-std=gnu23`; the asxxxx linker sources are K&R C, so force an older
standard.  Keep the x86_64 Homebrew `gnubin` coreutils out of `PATH` (an
x86_64 `env`/`sh` in the chain makes every `xcrun` shim fail on this arm64
box), and point bison at an arm64 m4.

```sh
cd lisa-tools/sdcc-lisa && autoconf # only after editing configure.ac
mkdir -p build && cd build
export PATH=$HOME/.local/bin:/opt/local/bin:/usr/local/opt/bison@3.8/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin
export M4=/opt/local/bin/gm4
ac_cv_prog_cc_c23=no ../configure \
    --disable-mcs51-port --disable-z80-port --disable-z180-port --disable-r2k-port \
    --disable-r2ka-port --disable-r3ka-port --disable-sm83-port --disable-tlcs90-port \
    --disable-ez80_z80-port --disable-z80n-port --disable-r800-port --disable-ds390-port \
    --disable-ds400-port --disable-pic14-port --disable-pic16-port --disable-hc08-port \
    --disable-s08-port --disable-stm8-port --disable-mos6502-port --disable-mos65c02-port \
    --disable-f8-port --disable-pdk13-port --disable-pdk14-port --disable-pdk15-port \
    --disable-ucsim --disable-sdcdb --disable-packihx --disable-sdbinutils --disable-non-free
# sdcpp needs libiberty even with sdbinutils disabled; build just that, and
# turn off pipe2() (the macOS SDK declares it, the libc does not have it):
mkdir -p support/sdbinutils/libiberty && (cd support/sdbinutils/libiberty && \
    ../../../../support/sdbinutils/libiberty/configure && make)
sed -i '' 's|^#define HAVE_PIPE2 1|/* #undef HAVE_PIPE2 */|' support/sdbinutils/libiberty/config.h
make                                # serial: the gcc-based sdcpp configure races under -j
# make the uninstalled compiler find its libraries and headers (lisa-cache is
# the library compiled with --tt07-cache, which --tt07-cache programs link,
# lisa-rev2 the one compiled with --rev2)
mkdir -p share/sdcc/lib && ln -sfn ../../../device/lib/build/lisa share/sdcc/lib/lisa
ln -sfn ../../../device/lib/build/lisa-cache share/sdcc/lib/lisa-cache
ln -sfn ../../../device/lib/build/lisa-rev2 share/sdcc/lib/lisa-rev2
ln -sfn $PWD/../device/include share/sdcc/include
```

Incremental rebuilds: `make -C src` (then `cp src/sdcc bin/sdcc`),
`make -C sdas/aslisa`, `make -C sdas/linksrc sdcc-ldlisa`,
`make -C device/lib/lisa`, `make -C device/lib/lisa-cache` and
`make -C device/lib/lisa-rev2` (the three libraries: after a compiler
change, `make clean` in each first).

## Using it

```sh
build/bin/sdcc -mlisa -o prog.ihx prog.c          # Intel HEX, byte addresses = 2 x PC
../lisa_sim/lisa_sim -b -c 500000 prog.ihx        # batch run, UART1 output on stdout
```

* `__sfr __at(0x210) UART_TX;` declares a peripheral register (direct
  addresses 0x200..0x3ff are the peripheral space).
* The first parameter of a function travels in A when it is a byte; the
  rest are on the stack, first parameter at the lowest address.  A byte
  result comes back in A, a two-byte one (int, a pointer) in IX, anything
  wider or a struct in a slot the caller reserves above the return
  address.  A function whose inline asm reads its parameters from the
  stack or writes a return slot is declared `__sdcccall(0)` to keep the
  old all-on-the-stack convention (variadic functions use it too).
* `printf` needs `int putchar(int c)` from the application (see
  `device/lib/lisa` tests in the project scratch area for an example).
* `--debug` writes `prog.cdb` beside the `.ihx` (the code is unchanged):
  the source lines behind the addresses, the functions, the globals and
  the stack locals with their offsets from the entry SP, the struct
  layouts.  `lisa_sim` loads it with the firmware and then has `break
  file:line`, `list`, `next`, `into`, `finish`, `print`, `locals` and `bt`
  (the reader is `lisa-tools/lisa_cdb`, meant for lisa_ide too).
* Default stack: SP starts at 0x7f (`--stack-loc`), i.e. the 128-byte RAM
  of the TT07 board with the cache disabled; data starts at 0.
* The library is a plain object list (`lisa.lib` + `.rel` files), because
  `sdar` lives in the disabled sdbinutils.
* Memory: the direct `lda`/`sta` forms reach data 0..0x1ff, so small
  globals live in `DATA`/`INITIALIZED` there; uninitialized objects of 64
  bytes or more (and anything declared `__xdata`) go to `FDATA` behind them
  and are addressed through IX. An `__at` object beyond 0x1ff is addressed
  through IX too. `n(sp)` reaches 511 bytes; in a bigger frame the small
  objects are laid out nearest SP and bytes beyond the reach are addressed
  through an IX window (`spix`, chained `adx`, `n(ix)`; SP never moves, so
  interrupts are safe), so frames are limited only by RAM.
* Division: unsigned 8- and 16-bit `/` and `%` use the hardware divider
  (`lddiv`, `div`, `rem`); the signed helpers in the library work on
  magnitudes with it.  On the TT07 silicon the result's high byte is not
  stored but every division leaves the 16-bit result in RA (the compiler
  reads it from there, and counts every division as an RA clobber), the
  forms with the divisor's high byte at `1(sp)` or the one-word `div 1`
  write the low byte to a stray address (only `div 3` and the slot at
  `2(sp)` are emitted), and RA is 15 bits with no high byte at all for a
  quotient by 1 - so a 16-bit quotient or remainder by a variable divisor
  is the library's `__divuint` / `__moduint`, which check - see the
  silicon notes in `lisa_isa.md`.  16-bit shifts use `shl16` / `shr16`
  on the `{n(sp), A}` pair.  `unsigned long` `/` and `%` (and the signed
  ones on top of them) are `device/lib/lisa/divul.s`: a divisor below
  256 is four 16/8 hardware divisions, anything else Knuth's algorithm D
  in base 256 with the digit estimates on the divider - about 100 and
  500-600 instructions a call against 3000-6000 for the bit-serial C
  routines.  `unsigned long long` `/` and `%` in the second chip's
  libraries (`lisa-rev2`, `lisa-rev2k`) are `device/lib/lisa/divull.s`
  on the chained divide step (ttsky26d RESPIN 27: `div` leaves the
  remainder in IX, `divc` divides {IX, A} by the latched divisor): a
  divisor below 65536 - printf's bases, a clock's 1000 - is one `divc`
  per byte of the dividend, about 60 instructions with interrupts off
  for the chain (the latched divisor is state any division replaces);
  a wider divisor stays the bit-serial loop (`divullb.c`, `modullb.c`:
  SDCC's generic routines renamed), 20K instructions.  The signed ones
  go through them.  TT07's libraries keep the generic routines.
* The bfloat16 unit: `#include <lisa/bf16.h>` gives `bf16_t` (the bit
  pattern) with `bf16_mul`/`bf16_div`/`bf16_gt`/`bf16_eq`/`bf16_cmp`/
  `bf16_from_uint`/`bf16_to_uint` on the hardware, `bf16_add`/`bf16_sub`
  in software (the TT07 silicon's adder is defective, see the BF16 notes
  in `lisa_isa.md`; `--rev2` puts them on the second chip's adder, below),
  and `bf16_from_float`/`bf16_to_float` to and from the 32-bit `float`.  The C type `float` itself stays the software one -
  unless compiled with `--bf16-float`: then `__fsadd`/`__fssub`/
  `__fsmul`/`__fsdiv` and the 8- and 16-bit integer conversions come from
  `device/lib/lisa/bf16fs.c` (linked ahead of `lisa.lib`; it defines
  `__SDCC_BF16_FLOAT`), every result is rounded to bf16 (8 significant
  bits, about 2.4 decimal digits) and stored as a float with a zero low
  half, and the comparisons and `long` conversions stay exact.  On the
  simulator an addition takes 240 cycles against 1080 for the software
  float library, a multiplication 100 against 2000, a division 100
  against 2850, the integer conversions 150 against 700-870.  Code built
  with and without the option must not be mixed in one link.
* Registers: one-byte temporaries are kept in A where the register
  allocator (`src/lisa/ralloc2.cc`, SDCC's tree-decomposition allocator
  with a dry-run cost model) finds it cheaper; everything else lives on
  the stack.  `LISA_NO_RALLOC=1` in the environment spills everything,
  for comparing code.
* Code-space constants: `const` globals live in code space as `ldi/ret`
  pairs (two words per byte); a pointer to them is the pair index with bit
  15 set, which is what the generic-pointer code tests at run time.
  Constant data initializers that point into code space are scaled
  accordingly (`src/SDCCval.c`, `lisaCodeScale`).  A read through a
  generic or `__code` pointer is a call to `__gptrget` / `__gptrnext`
  (`device/lib/lisa/gptrget.s`, 2 words per byte), with a constant
  offset (`p->member`, `p[k]`: SDCC folds offsets of 0..255 into the
  access for lisa) in A for `__gptrgeto` / `__gptrcodeo`, and a struct
  passed by value through such a pointer is pushed last byte first with
  `__gptrprev`; `--opt-code-speed` puts the space test and the `call ix`
  loop inline instead (12 words for the first byte, but a few cycles
  faster per byte - string loops care).  `memcpy` (`__memcpy`), `memset`,
  `strcpy`, `strlen` and `strcmp` are assembly (`device/lib/lisa/{memcpy,
  memset,strcpy,strlen,strcmp}.s`): they test each source's space once
  and run a loop for it, RAM or `call ix`, the pointers swapped through
  IX with `ldxx` / `stxx` (2-15 times the speed of the C on the chip).
  An unqualified pointer is generic; a `__near` pointer is RAM only and
  reads with `ldxx; ldax 0(ix)`, no helper and no space test - a
  `while (*p)` byte walk on the chip takes 54 ms through a `__near`
  pointer against 88 through a generic one (23 bytes x 32).  Use it for
  buffers that are never in code space.
* The data cache (the 32K data space on a SPI RAM, `lisa_isa.md` "TT07
  silicon data cache note"): tested on the chip with the RP2040 of the
  demo board emulating the RAM (`mbell_micropython` branch
  `lisa_spi_ram`, `tt07-um-lisa-ttlc/lisa-test/test/hw_test.mjs
  spiram=<file>`).  Every `sdcc_test` suite passes through it, with the
  silicon's quirks worked around in the generated code: no `swap n(sp)`
  (it addresses sp + n - 512) and a `cmp` of the byte before every
  `inx`/`dcx` (a read-modify-write that misses the cache works on stale
  data).  The cache's own bug - data addresses X and X ^ 0x4000 share a
  RAM location - means a program must not use both halves of the data
  space at addresses that differ only in bit 14.  `--tt07-cache` lays
  a program out for it: the stack at 0x7fff (upper half, cache lines
  4..7, so it never evicts data), `--stack-size` bytes reserved (2K
  unless given), and the data areas - DATA, INITIALIZED, FDATA with the
  1K heap, FINITIALIZED - limited to 16K minus that reserve (the limit
  reaches the linker as `-X`, and `sdldlisa` refuses a layout beyond
  it).  Splitting data across the halves gains nothing: whatever is
  used in one half is unusable in the other.
* `--rev2`: a program for the second chip (`ttsky26c-um-lisa-ttlc`, whose
  `RESPIN.md` lists what its silicon fixes).  Two things change.  The
  stack: on that chip a push decrements SP before it writes and a pop
  reads before it increments, so SP points at the byte on top of the
  stack and every frame offset is one less than on TT07 (`0(sp)` is the
  top; `gen.c` counts every offset from `spTop ()`, 1 or 0, and the
  library's `.s` sources write theirs as `T+n(sp)` with `T` from
  `lisamodel.inc`, 1 or 0 - an `spix; adx #T+n` for a computed address).
  And the library: it links `lib/lisa-rev2`, compiled with the option, in
  which `bf16_add` - and so `bf16_sub` and `--bf16-float`'s
  `__fsadd`/`__fssub` - is the chip's adder (`fadd`: correct there,
  nearest even, 15 instructions against the software addition's few
  hundred; `bf16add.s` under `LISA_REV2`).  `__SDCC_LISA_REV2` is defined
  for the source.  The code sequences the compiler generates are otherwise
  still the TT07 ones: `ret #imm8` for the constants, the plain
  `adc`/`sub`/`cmp` and the divider's result in RA are to come.  `--rev2`
  and `--tt07-cache` exclude each other (that layout is for the TT07
  cache's bit-14 alias, which the second chip's cache does not have).
  `lisa_sim --rev2` models the chip (the stack, the adder, the divider
  without its stray write, `swap n(sp)`, the interrupt semantics of
  `--fixed-irq`); `make check REV2=1` in `sdcc_test` and `make test-lisa
  LISA_REV2=1` in `build/support/regression` run the suites that way.  The
  debuggers follow the convention through `lisa_cdb`'s `Cdb::set_sp_top`
  (lisa_sim sets it from `--rev2`).
* Code size: `gen.c` tracks what A holds and skips reloads, compares
  against 0 are sign-bit tests or `bnz` chains, and the peephole rules
  (`src/lisa/peeph.def`) thread branches and drop dead code; `--no-peep`
  turns the rules off.  `ldxs #lit; push ix` pushes a 16-bit literal in 3
  words, but only one whose low byte has bit 7 set: `ldx` sets ix_cond
  and `push ix` writes it as bit 7 of the high half.

## Peripherals and interrupts

`#include <tt07.h>` (`device/include/lisa/tt07.h`, found automatically for
`-mlisa`) declares the TT07 peripheral registers as `__sfr __at(0x2xx)`
(single `lda`/`sta` instructions), their bits, the interrupt sources
(`INT_TIMER1`...) and vector numbers (`LISA_INT_TIMER1`...), and
`lisa_ei()` / `lisa_di()`:

```c
#include <tt07.h>
volatile unsigned char ticks;
void timer1_isr (void) __interrupt (LISA_INT_TIMER1)
{
  ticks++;
  INT_STATUS = INT_TIMER1;        /* acknowledge */
}
...
  TIMER1_PREDIV_LO = 0x4f; TIMER1_PREDIV_HI = 0xc3;   /* 50 MHz / 50000 = 1 ms */
  TIMER1_DIV_LO = 5; TIMER1_DIV_HI = 0;               /* every 5 ms */
  INT_ENABLE = INT_TIMER1;
  TIMER1_CTRL = TIMER_CTRL_ENABLE;
  lisa_ei ();
```

A handler is a normal function with a prologue that saves A, IX, RA (the
vector `jal` leaves RA alone, the core's `isr_jump`) and `cflag_save` (the
`savec`/`restc` shadow, which the hardware does not shadow), and ends with
`rets`; the live C and Z come back from the hardware's shadows. Generated
code never changes `amode` (it is not readable or shadowed), so a handler
can use everything.

### An interrupt-driven receiver (the second chip)

The console UART status (0x211) has bit 0 "data available", bit 1 "TX
buffer empty" and, on the second chip, bit 2 "overrun" (a byte arrived
with the 8-byte receive FIFO full; it clears once the FIFO has been read
empty).  The UART interrupt (`__interrupt(LISA_INT_UART1)`, enable bit 0 of
0x214 and `INT_UART1` in `INT_ENABLE`) is raised on the rising edge of
"data available" (and of "TX empty" if bit 1 of 0x214 is set), so a
receive handler acknowledges first and then drains:

```c
void uart_isr(void) __interrupt(LISA_INT_UART1)
{
  INT_STATUS = INT_UART1;                 /* ack before the drain: an edge during it is not lost */
  while (UART_STATUS & 1) {
    unsigned char c = UART_TX;
    ... ring buffer ...
  }
  if (UART_STATUS & 4) overruns++;        /* bytes were lost before this handler ran */
}
```

The second chip takes interrupts only between whole instructions (never
inside `ldx`, a two-stage opcode or a predicate), so this is safe in C;
on TT07 it is not (below).

### Interrupts on TT07

The TT07 silicon samples an interrupt in the same cycle that executes an
`if`/`ifte`/`iftt` or the first word of an `ldx` (two single-stage passes),
and saves neither the predicate nor the pending literal. After `rets` the
predicated instruction runs unconditionally, and an `ldx` gets the vector
word as its value while its literal is later executed as an opcode
(always a `jal`). `sdcc_test/test_irqhaz.c` shows both on the chip: in two
20 ms runs with a 1 ms timer it counted 2 lost predicates and 1 corrupted
`ldx`. There is no software workaround at acceptable cost — `ldx` and
predication are in almost every generated sequence — so **on TT07,
interrupts must stay disabled while compiled code runs**: a handler can
be used only around code written in assembly without `if*`/`ldx`, as
`test_irq2.c` does (its wait loop is `cpi`/`bnz`). The live
`signed_inversion` is also lost on return (an interrupt between a signed
`cmp` and its `if slt` branches wrong).

`lisa_sim` models this exactly; `lisa_sim --fixed-irq` models the
semantics a respin should have, under which the C handler support is
correct (`test_irq.c` passes there with interrupts landing anywhere in
32-bit arithmetic, signed multiplies and shifts). The RTL changes for that
are listed in `lisa_isa.md` ("TT07 interrupt notes").

## Code banks (the second chip, `--rev2`)

The second chip (ttsky26d-um-lisa-ttlc, RESPIN.md item 22) makes the upper
16K words of the code space a window on one of 64 banks, chosen by the
code bank register `[p:0x15]` (reset 1 = the plain 32K layout; `lisa_isa.md`
"Bank registers").  The port uses it for functions declared `__banked`:

```c
/* bank2.c */
#pragma bank 2                      /* this file's code goes to the area BANK2 */
unsigned int sum(unsigned int a, unsigned int b) __banked { return a + b; }

/* main.c */
unsigned int sum(unsigned int a, unsigned int b) __banked;   /* the same attribute in every declaration */
...   sum(1, 2) ...
```

    sdcc -mlisa --rev2 -c bank2.c                                 # one source per run
    sdcc -mlisa --rev2 -o prog.ihx main.c bank2.rel -Wl-bBANK2=0x10000

* `#pragma bank n` (1..63; 0 or BASE = the common area CODE again) names the
  area `BANKn`; the link option `-bBANKn=<byte n * 0x8000>` puts it at word
  0x4000 of bank n (the Intel HEX then has type-04 records, which lisa_sim,
  lisa_pydb, the testbench's ihx2hex.py and ttlc_io.py read).  The linker
  turns every code address at or above word 0x4000 into its window address,
  so `jal`, `ldx #_f` and function pointers are right from any bank.
* A `__banked` call pushes the callee's bank (`b_f`, an equate the
  definition emits), loads IX with the function and runs `___sdcc_bcall`
  (device/lib/lisa/bcall.s, in HOME): it saves RA and the caller's bank
  under the callee's frame, switches the bank, calls, switches back and
  returns; the result stays in A / IX.  The callee's stack parameters and
  return slot are 3 bytes deeper than a plain function's (the port's
  `banked_overhead`).  Pointers to `__banked` functions are 3 bytes
  (address, bank) and are called the same way.
* The common code must stay below word 0x4000: HOME (the vectors, the
  trampoline), CODE (`main`, every function without `#pragma bank`, the
  library), GSINIT, CONST.  Interrupt handlers belong there (the hardware
  does not switch banks on an interrupt).  Watch the `.map`: the linker does
  not check it yet.  Constants (`const`, string literals) live in CONST
  whichever file declares them, so a bank's functions may use them.
* The linker checks the layout: a `BANKn` area must lie inside bank n,
  and once there is one, every other code area must end below word 0x4000.
* `sdcc_test/test_bank.c` (+ `test_bank2.c`, `test_bank3.c`; `make check
  REV2=1`) exercises calls from the common area and between banks, byte /
  int / long results, A and stack parameters, 3-byte function pointers and
  recursion across banks.

### The huge model

A program too large to annotate function by function (an operating system
and its applications) is built with `--model-huge`: every function is then
a banked call unless it is declared `__nonbanked`, and a source goes to a
code bank with `--codeseg BANKn` instead of `#pragma bank` - nothing in
the sources says `__banked`:

    sdcc -mlisa --rev2 --model-huge --codeseg BANK5 -c lib.c
    sdcc -mlisa --rev2 --model-huge -o prog.ihx main.c lib.rel -Wl-bBANK5=0x28000

* The functions of a file compiled without `--codeseg` go to CODE (the
  common area; their equate `b_f` is 1, the bank that the window shows by
  default - a call to one below word 0x4000 switches the bank for nothing,
  that is all).  All function pointers are 3 bytes unless their type says
  `__nonbanked`; `sizeof` tells.
* Its data is far: a program that size has more small globals than the
  512 bytes the direct `lda`/`sta` reach, so every global goes to FDATA /
  FINITIALIZED through IX unless declared `__data` (the small model keeps
  objects under 64 bytes in DATA, and the linker now refuses a DATA,
  INITIALIZED or OSEG area that ends beyond 0x200 - a silent truncation
  before).
* `sdldlisa` takes a library that is a plain list of object files, and a
  line that is an absolute path is the module itself (the NuttX port's
  `lisa-ar` writes them that way so that a library can be moved).  With
  `-y` the `.adb` of such a module goes into the `.cdb` like a file named
  on the command line (the stock linker merged only those), so lisa_sim
  and lisa_ide see the library's functions and lines too.
* The library is the small-model one: the compiler's support routines
  (`__mullong`, the divides, the BF16 helpers) are always plain calls, and
  a program that calls the library's C functions (`printf`, `malloc`, ...)
  must declare them `__nonbanked` (an operating system brings its own
  libc, so it does not).  Interrupt handlers and `main` need nothing: the
  vectors and crt0 jump into them directly, which works for a function
  without parameters whatever its calling convention.
* A `static inline` function is emitted only if the file still refers to
  it after inlining (an address taken, a call the inliner left alone):
  the helpers of a header that a file never calls cost it nothing.  SDCC
  used to emit every one of them (NuttX's headers put 10 KB of dead code
  in each module that way, and their calls dragged the whole kernel into
  every link).  This holds in every model; `.cdb` entries of the dropped
  functions remain.
* A parameter declared with a function type (`void f(fn_t handle)`,
  `handle(x)`) is a 3-byte pointer on the stack and the call goes
  through it.
* `--model-huge` defines `__SDCC_MODEL_HUGE`.  `sdcc_test/test_huge.c`
  (+ `test_huge5.c`, `test_huge6.c`; `make check REV2=1`) is `test_bank.c`
  again without an attribute in sight, plus `__nonbanked` functions and
  pointers.

### Packed constants (`--pack-const`, the second chip)

LISA reads its code memory as data only by executing it: a byte of
constant data has been an `ldi #b; ret` pair that `call ix` returns
through, four bytes of flash per byte, and it had to sit in the lower 16K
words, which every bank sees, because the reader does not switch banks.
NuttX's image filled that common area with them.  The second chip's
`lda (ix++)` (RESPIN 26) reads byte IX[0] of the code word at {KB,
IX[14:1]} and moves IX on, KB being the constant bank register at 0x21F.

`--pack-const` (implies `--rev2`) puts the constant areas - CONST,
INITIALIZER, FINITIALIZER, CABS - in the assembler's `KDATA` form, bytes
packed two to a word, places them wherever the link script says
(`-b CONST=0x8000` puts them in code bank 1; each area has to stay within
one bank, the linker checks), and reads them with the instruction: a
pointer to a constant is its byte offset in the bank with the code-space
tag in bit 15, `__gptrget` and its kin (lib/lisa-rev2k, gptrgetk.s and the
string helpers' k variants) read with `lda (ix++)`, and the startup writes
KB from the linker's `k_INITIALIZER` / `k_FINITIALIZER` for its copy loops
and leaves it on `k_CONST`.  KB is set once per image, so no interrupt
frame carries it and generic pointers stay two bytes.  The C side sees
`__SDCC_LISA_PACK_CONST`.  The suite runs in this form with `make check
PACK=1` (`*_rev2k.ihx`, `lisa_sim --rev2`).

### Data banks

The data space of the second chip has three regions, by address (RESPIN
28; before it the upper 16K was one window chosen by the form of the
access): below 0x2000 the common 8K, 0x2000-0x3fff the data window on one
of 32 granules of 8K chosen by `[p:0x16]`, 0x4000-0x7fff the stack window
on one of 16 banks of 16K chosen by `[p:0x17]`.  Switching the data bank
never moves a stack above 0x4000.  The port uses SDCC's named address
spaces for the data window: a space is a switch function plus a name, and
the compiler calls the function before every access to an object of that
space.

```c
__sfr __at(0x216) DATA_BANK;
static void dbank2(void) { DATA_BANK = 2; }
__addressmod dbank2 DBANK2;          /* the name is also the object's area */

DBANK2 unsigned char buf[4096];      /* in data bank 2 */
```

    sdcc -mlisa --rev2 -o prog.ihx prog.c -Wl-bDBANK2=0x2000

* Name the spaces `DBANKn`: the area of a space is its name, linked at
  0x2000 (`-bDBANKn=0x2000`; several spaces share the window address, each
  in its own granule; a space holds 8K at most).  Their objects are always
  addressed through IX.
* The switch function leaves the bank where it put it, so ordinary data
  (DATA, FDATA, the initialized areas) must stay below 0x2000, in the
  common 8K, once there are data banks; the linker checks it (`--data-loc`
  / `--xram-loc` keep it low).  The stack is exempt: above 0x4000 it is in
  the stack bank whatever the data bank; a stack in the data window moves
  with it.
* The startup neither initializes nor clears objects in a data bank: give
  them their values at run time.
* A pointer to a space-qualified type (`DBANK2 unsigned char *p`) is a
  16-bit window address, and its dereference switches to the space.  A
  plain pointer does not: it reaches whichever bank was switched in last.
* The compiler remembers which space it last switched to and skips the
  switch for the next access to the same space, so code that also writes
  `[p:0x16]` by hand defeats it (seen in a test: `DATA_BANK = 3; *p`
  read bank 3).  Switch banks through the spaces only.
* An interrupt handler that touches a data bank must save and restore
  `[p:0x16]`: the hardware does not.
* `sdcc_test/test_dbank.c` (`make check REV2=1`): two spaces in banks 2
  and 3 at one window address, byte and int objects, a loop, a
  read-modify-write across the two, ordinary data alongside.

## Regression testing

SDCC's own suite (`support/regression`, ~1650 test files, ~6000 generated
cases, ~100k test points) runs on `lisa_sim`:

```sh
cd build/support/regression
make test-lisa                      # whole suite, ~6 minutes with -j8
make test-lisa TEST_PREFIX=bitfields # one family
cat results/lisa.sum                # summary; results/lisa/<test>.out has the details
```

`ports/lisa/spec.mk` points at `../lisa_sim/lisa_sim` (override with
`LISA_SIM=`), runs with `--stack-loc 0x7fff` (the simulator has 32K of data)
and `_exitEmu()` executes `brk`, which halts the simulator. The Makefile only
tracks the test sources: after changing the compiler or the library,
`rm -rf gen/lisa results/lisa` first. Tests that hard-code addresses outside
the 32K data space have LISA-specific addresses.

The smaller, chip-sized suite is `../sdcc_test` (`make check`); its
`test_regress.c` collects the bugs the big suite found.

## Layout of the port

| what | where |
|---|---|
| compiler back end | `src/lisa/{main.c,gen.c,ralloc.c,ralloc2.cc,peeph.def}` (`ralloc2.cc`: A for one-byte temporaries, tree-decomposition allocator with dry-run costs) |
| assembler | `sdas/aslisa/{lisa.h,lisaadr.c,lisamch.c,lisapst.c}`, data-in-code expansion in `sdas/asxxsrc/asout.c`, `.p` suffix in `asmain.c` |
| linker | `sdas/linksrc/lkrloc3.c` (LISA relocation rules), `lkarea.c` (code/data spaces, CDATA alignment, `s_<area>` symbols) |
| library | `device/lib/lisa/{Makefile.in,setjmp.s,atomic_flag_test_and_set.s,heap.s,divu.s,divul.s,divull.s,divullb.c,modullb.c,gptrget.s,memcpy.s,memset.s,strcpy.s,strlen.s,strcmp.s,bf16.s,bf16add.s,bf16c.c,bf16fs.s,bf16fsc.c}`, `device/include/lisa/tt07.h` (peripherals, interrupts), `device/include/lisa/bf16.h` (the FPU), `device/include/stdarg.h`, `setjmp.h`, `stdatomic.h` |
| regression port | `support/regression/ports/lisa/{spec.mk,support.c}`, `fwk/include/testfwk.h` (`__SDCC_lisa`), LISA addresses in `tests/bitfields-*.c.in`, `tests/absolute.c.in` |
| generic hooks | `src/SDCCglue.c` (program startup jumps, sfr table, FDATA), `src/SDCCsymt.c` (8-bit div/mod helpers return int; `__at` objects stay in data), `src/SDCCmem.c` (big objects to FDATA), `src/SDCCval.c` (code-pointer offset scaling), `src/port.h`, `src/SDCCmain.c`, `configure.ac`, `Makefile.in`, `device/lib/Makefile.in`, `device/lib/malloc.c` (lazy heap init) |

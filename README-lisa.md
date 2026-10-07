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
# make the uninstalled compiler find its library and headers
mkdir -p share/sdcc/lib && ln -sfn ../../../device/lib/build/lisa share/sdcc/lib/lisa
ln -sfn $PWD/../device/include share/sdcc/include
```

Incremental rebuilds: `make -C src`, `make -C sdas/aslisa`,
`make -C sdas/linksrc sdcc-ldlisa`, `make -C device/lib/lisa`.

## Using it

```sh
build/bin/sdcc -mlisa -o prog.ihx prog.c          # Intel HEX, byte addresses = 2 x PC
../lisa_sim/lisa_sim -b -c 500000 prog.ihx        # batch run, UART1 output on stdout
```

* `__sfr __at(0x210) UART_TX;` declares a peripheral register (direct
  addresses 0x200..0x3ff are the peripheral space).
* `printf` needs `int putchar(int c)` from the application (see
  `device/lib/lisa` tests in the project scratch area for an example).
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
  routines.
* The bfloat16 unit: `#include <lisa/bf16.h>` gives `bf16_t` (the bit
  pattern) with `bf16_mul`/`bf16_div`/`bf16_gt`/`bf16_eq`/`bf16_cmp`/
  `bf16_from_uint`/`bf16_to_uint` on the hardware, `bf16_add`/`bf16_sub`
  in software (the silicon's adder is defective, see the BF16 notes in
  `lisa_isa.md`), and `bf16_from_float`/`bf16_to_float` to and from the
  32-bit `float`.  The C type `float` itself stays the software one -
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
  (`device/lib/lisa/gptrget.s`, 2 words per byte); `--opt-code-speed`
  puts the space test and the `call ix` loop inline instead (12 words
  for the first byte, but a few cycles faster per byte - string loops
  care).
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
| library | `device/lib/lisa/{Makefile.in,setjmp.s,atomic_flag_test_and_set.s,heap.s,divu.s,bf16.s,bf16add.s,bf16c.c,bf16fs.s,bf16fsc.c}`, `device/include/lisa/tt07.h` (peripherals, interrupts), `device/include/lisa/bf16.h` (the FPU), `device/include/stdarg.h`, `setjmp.h`, `stdatomic.h` |
| regression port | `support/regression/ports/lisa/{spec.mk,support.c}`, `fwk/include/testfwk.h` (`__SDCC_lisa`), LISA addresses in `tests/bitfields-*.c.in`, `tests/absolute.c.in` |
| generic hooks | `src/SDCCglue.c` (program startup jumps, sfr table, FDATA), `src/SDCCsymt.c` (8-bit div/mod helpers return int; `__at` objects stay in data), `src/SDCCmem.c` (big objects to FDATA), `src/SDCCval.c` (code-pointer offset scaling), `src/port.h`, `src/SDCCmain.c`, `configure.ac`, `Makefile.in`, `device/lib/Makefile.in`, `device/lib/malloc.c` (lazy heap init) |

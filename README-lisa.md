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

## Layout of the port

| what | where |
|---|---|
| compiler back end | `src/lisa/{main.c,gen.c,ralloc.c,peeph.def}` |
| assembler | `sdas/aslisa/{lisa.h,lisaadr.c,lisamch.c,lisapst.c}`, data-in-code expansion in `sdas/asxxsrc/asout.c`, `.p` suffix in `asmain.c` |
| linker | `sdas/linksrc/lkrloc3.c` (LISA relocation rules), `lkarea.c` (code/data spaces, CDATA alignment, `s_<area>` symbols) |
| library | `device/lib/lisa/Makefile.in`, `device/include/stdarg.h` |
| generic hooks | `src/SDCCglue.c` (program startup jumps, sfr table), `src/SDCCsymt.c` (8-bit div/mod helpers return int), `src/port.h`, `src/SDCCmain.c`, `configure.ac`, `Makefile.in`, `device/lib/Makefile.in` |

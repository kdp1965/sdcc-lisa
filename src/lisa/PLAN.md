# SDCC port for LISA — design notes

Target: the LISA 8-bit core as taped out on TT07 (`tt07-um-lisa-ttlc/src/lisa_core.v`),
tool names `sdcc -mlisa`, `sdaslisa`, `sdldlisa`.  The port is modelled on the pdk
port (accumulator machine, 16-bit instruction words, word-addressed code, Harvard)
with the stack handling of the stm8/z80 ports.

## Machine facts that shape the port

* Code: 32K x 16-bit words, PC is a word address.  Data: 32K x 8-bit (the TT07
  board runs with the cache disabled, i.e. 128 bytes of RAM at 0..0x7f;
  peripherals at `[p:abs9]`).  The assembler/linker count code in *bytes*
  (2 per word, little endian); every code address that ends up in an
  instruction or a data word is divided by two by the linker (see "Relocations").
* Registers: `A` (8), `IX` (15 + cond bit), `SP`, `RA` (15 + cond), `Z`, `C`.
  `IX` bit 15 is the condition tag, so IX never carries a C value — it is a
  pointer temp only.  Nothing survives a call except the stack.
* Stack grows down.  `push a` writes `[SP]` then `SP--`; `pop a` does `SP++` then
  reads.  SP points at the next free byte; the top byte in use is `1(sp)`.
* `jal` saves the return address in `RA` (not on the stack).  Non-leaf functions
  spill it with `sra`/`lra` (2 bytes).  `ret` returns through `RA`.
* `ret #imm8` is broken on TT07 (A gets the low byte of the instruction at the
  return address), so a byte of constant data in code space costs two words:
  `ldi #v ; ret`, read with `call ix` (IX post-increments) followed by `adx 1`.
  A fixed core could use `ret #imm8` and stride 1; that is a compile-time option
  (`--reti`) for later, everything below assumes stride 2.
* `add`/`sub` always include the carry (`A+M+C`, `A-M-C`); `ldi` clears C, loads and
  stores leave it alone.  `cmp`/`cpi` set Z and C (C = borrow).  Signed ordering
  needs `amode[1]=1` during the compare and `if` with the `s` bit.
  Baseline `amode` is the reset value 1 (shift through carry): clear C before a
  logical shift; signed compares/mul/div are bracketed with `amode 3` .. `amode 1`.
* Most instructions are predicated by `cond[0]`, written by `if/iftt/ifte`.  Any
  taken branch resets cond to 3.  That gives cheap conditional stores/increments
  (`inx lo ; if c ; inx hi`).

## ABI

* `char`/`bool` returned in `A`.  Anything wider is returned in a slot the caller
  reserves on the stack directly above the return address (`ads -size` just before
  `jal`), so the callee finds it at a static offset even with varargs.
* Parameters are pushed right to left, little endian (high byte pushed first), so
  the first parameter is at the lowest address.  The caller pops them.
  Callee frame, seen from the callee's SP after the prologue (`sra` if non-leaf,
  `ads -L` for L bytes of locals):

      1(sp) .. L(sp)            locals / spilled temporaries
      L+1, L+2                  saved RA (non-leaf only)   S = 2 or 0
      L+S+1 ..                  return slot (if the return type is wider than 1 byte)
      L+S+R+1 ..                parameters, first parameter first

  Every stack operand is `ldax n(sp)` / `stax n(sp)` with n <= 511, so no frame
  pointer.  Frames larger than that are not supported for now (there is 128
  bytes of RAM).
* Caller-saved: everything (`A`, `IX`, flags, `amode`).  Callee-saved: `SP`.
* Interrupt handlers: vectors are words 1..8 (`jal _isr`), word 0 is `jal
  __sdcc_gsinit_startup`... (`genIVT`).  An ISR saves `A` (`push a`) and `IX`
  (`push ix`) if it uses them, and returns with `rets` (restores Z/C shadows and
  re-enables interrupts).
* Pointers are 16 bits.  Generic pointers (unqualified `char *`) carry a tag in
  bit 15: set = code space (constant data), clear = data space.  A pointer to
  constant data counts in `ldi/ret` pairs (its value is the word address / 2,
  so stride 1 per byte like a RAM pointer, and generic pointer arithmetic
  needs no special case).  Constant data areas are flagged `CDATA` and
  4-byte aligned by the linker.  Dereferencing doubles IX
  (`txau; andi #0x7f; addaxu; txa; addax`) then `call ix` (+ `adx 1`) per
  byte.  `ldx #sym` always yields the word address (for direct reads and
  function pointers).

## Assembler `sdaslisa` (sdas/aslisa)

asxxxx target with lisa_as-compatible syntax so existing `.S` sources port easily:

    ldax 3(sp)    stax 0(ix)    lda 0x20c    sta PORTB     ldi 5     adc #5 / adc 5
    jal _f        br L          bz L         bnz L         ldx _buf  ldx #_buf
    if lt         iftt ge       ifte ne      (optional `s` prefix: if slt)
    call ix / call_ix   jmp ix   xchg sp   cpx ra   push a   pop ix   sra   lra
    shl16 2(sp)   div 1   rem 0   amode 3   btst 7   .db/.dw/.ascii in code areas

* Operand forms in `lisaadr.c`: immediate (`#n` or bare `n`), `n(sp)` / `n(ix)`,
  bare symbol/number (direct `[p:abs9]` for lda/sta/swapi; abs for `jal`/`ldx`).
* Branches to a label in the same area are resolved by the assembler (rel11 in
  words).  Anything else is an error (the compiler never does it).
* `.db`, `.dw`, `.ascii`, `.str` inside a code area (`.area CODE`, `CONST`,
  `HOME`, `GSINIT`, ...) emit `ldi #v ; ret` word pairs per byte.  `.dw sym` becomes
  two `ldi` words with byte relocations (`R_BYTE`, `R_BYTE|R_MSB`).
* Relocations.  Code is byte-counted.  The assembler emits ordinary R_WORD/R_BYTE
  relocations and the linker (`TARGET_ID_LISA` in `lkrloc3.c`) converts any value
  that resolves to a symbol in a CODE-flagged area to a word address (`v >> 1`)
  and, for data words/bytes (not the `jal` field), ORs in the 0x8000 tag.
  `jal` fields mask to 15 bits.  The `ldx` literal word ignores bit 15 in hardware.

## Compiler `src/lisa`

* `main.c`: PORT `lisa_port` (pdk15 as template): sizes char 1 / int 2 / long 4 /
  ptr 2 / gptr 2 / fptr 2, tags `{0,0,0,0x80}`, unqualified pointers generic,
  little endian, `code_ro`, stack grows down, `sp points to next free`,
  `genIVT`, `genInitStartup` (copies `INITIALIZER` -> `INITIALIZED` with
  `call ix` pairs, zeroes nothing else: SDCC zeroes DATA in `__sdcc_init`... use
  the stm8 pattern: `___sdcc_init_data` loop).
* `ralloc.c` / `ralloc2.cc`: `A` is the one allocatable register, for
  one-byte temporaries, assigned by the tree-decomposition allocator of
  `SDCCralloc.hpp` with the pdk-style dry-run cost model (`dryLisaiCode`
  generates an iCode without emitting and returns its cost).  Everything
  else is spilled to the stack (`AOP_STK`) or lives in DATA.  The
  generators load their first operand with `loadA` and store the result
  with `storeA`, both no-ops for a byte in A; `genNativeA` says when that
  placement is enough, otherwise `genLisaiCode` parks A on the stack
  around the generator (`push a` / `pop a`, the operand becomes that stack
  byte).  Only branches, calls and pushes cannot be wrapped that way, so
  `Ainst_ok` in `ralloc2.cc` rules out a live A across them unless the
  generator keeps it (`bz`/`bnz` on A, `cpi`, the push-and-swap of
  `genIpush`).  A byte that is in A already is tested with `cpi #0` before
  a `bz`: the last flag write need not have been its load.
  `LISA_NO_RALLOC=1` in the environment spills everything (for comparing).
* `gen.c`: asmop types LIT, STK (`n(sp)`), DIR (`sta/lda abs9` for low data and
  sfrs), IMMD (symbol address), CODE (const data), A.  Byte-wise codegen for
  everything wider than 8 bits, `C` chained through `add`/`sub`/`adc`.
  Key sequences:
  - compare/branch: `cmp`/`cpi` then `bz`/`bnz`, or `if cc ; br` (signed: `amode 3`
    around the compare, `if s..`).
  - pointer get/put: `ldxx n(sp)` (16-bit load of IX from the stack) then
    `ldax k(ix)` / `stax k(ix)`; code pointers via `call ix`.
  - mul: hardware `mul`/`mulu` (8x8 -> 16); div/rem: library first, hardware later.
  - shifts: `shl`/`shr` (after `ldc 0`), `shl16/shr16 n(sp)` for 16-bit operands at
    0..3(sp).
* `peeph.def`: `stax n ; ldax n` -> `stax n`; `br` to next; `ldi 0 ; stax x` chains;
  `if ne ; br L` -> `bnz L` etc.
* `device/lib/lisa`: `crt0.s` (vectors, `__sdcc_gsinit_startup`, call `_main`),
  generic C library built with the port, `__gptrget`/`__gptrput` helpers.
* `device/include/lisa`: `tt07.h` with the peripheral map.

## Status (2026-10-06)

Done: assembler, linker, port, runtime library (`device/lib/lisa`: int/long
div-mod, string functions, printf/sprintf, `__sdcc_external_startup`).
Verified on the C++ simulator: 44 self-checking cases (8/16/32-bit
arithmetic, signed/unsigned compares, shifts, mul, div/mod via the library,
pointers into RAM and into constant data, generic pointers, function
pointers, switch jump tables, initialized globals, unions, varargs,
printf with %d %u %x %ld %c %s and widths).  Everything but one-byte
temporaries that the allocator keeps in A lives on the stack;
frames are packed by block scope and spill slots are shared
between non-overlapping temporaries.  Branches out of the +-1024-word range
are relaxed after the peephole pass.  Instructions after `if/iftt/ifte`
are emitted with a `.p` suffix so peephole rules leave them alone.

Verified on the TT07 chip (LISA Commander loads Intel HEX; `hw_test.mjs
ihx=<file>` runs a suite): test_core 25, test_signed 15, negate 6, varargs 5,
digits 4, union 4, strs 4, plus the hello/printf/owl output tests.  Running
on silicon found the ALU flag bugs (`lisa_core.v` 643-653: the adder is
`acc + 8-bit operand` with the carry-in folded into the operand).  `lisa_sim`
now models them and the generator works around them:
* `add M` has no carry-in: multi-byte add is `adc #0; add M`, with the two
  carries merged by `savec` / `if nc; restc` (`emitAddByteMem`).
* `adc #k` adds `(k+C)&0xff`: subtract-literal is `ldc 1; adc #~k`, and a
  byte whose operand is `0xff` gets `if nc; adc.p #0xff` so a wrapped
  operand does not lose the carry.
* `sub M` / `cmp M` report a borrow when `M == 0`: the memory operand is
  tested with `cpi #1` first and a zero operand takes `ldc 0` / skips the
  subtract (`ifte nc; sub.p M; ldc.p #0`, branchy for middle bytes).
* `cpi` never sets `signed_inversion`: signed compares against a literal
  bias both sides (`ldc 0; adc #0x80; cpi #(k^0x80)`) and branch unsigned;
  signed compares against memory use `cmp` with the zero guard and a
  `btst 7` path for a zero top byte.

SDCC's own regression suite (`support/regression`, `make test-lisa`,
~6000 cases, ~31k test points on lisa_sim) passes on the port as of
2026-10-06 (tests that hard-code addresses outside the 32K data space got
LISA addresses).  Getting there fixed: ldx hoisted out of predicated pairs
(ldx is two words - a skipped ldx executes its literal), the borrow
convention of zero-extension bytes in subtractions, struct arguments
(IPUSH_VALUE_AT_ADDRESS), bit fields (read, write, _BitInt padding), the
result/operand slot sharing that SDCC's allocator allows (shift count read
after the result was written; aopSame beyond an operand's size), literal
generic pointers (ldx cannot carry bit 15: the space is decided at compile
time), float negation (sign bit, not two's complement) and float truth
tests (-0.0), signed literal bytes beyond the literal's size, symbol
addresses in subtractions (no ~sym relocation), code-pointer offsets in
initializers (scaled by 4), setjmp/longjmp, atomic_flag, malloc's heap,
__critical, calls through constants, lldiv and the char multiply helpers,
and a peephole that dropped a reload whose Z flag was still needed
(`lisaNotUsed` now tracks Z/C liveness for the rules).  Memory layout: big
objects (>= 64 bytes, or `__xdata`) go to FDATA / FINITIALIZED behind the
directly addressed data and are reached through IX.  Frames bigger than
the 511-byte `n(sp)` reach are laid out with the small objects nearest SP
(`lisaFarFrameLayout`), and far bytes go through an IX window (`spix`,
chained `adx`, `n(ix)`; the IX tracker keeps the base, loads and stores
that must keep IX wrap it in `push ix`/`pop ix`); SP never moves, so this
is interrupt-safe, unlike an `ads` window would be.

The A allocator (2026-10-06): one-byte temporaries go to A where the
tree-decomposition allocator finds it cheaper (about 4% less code on the
library and the test programs; most `stax t ; ldax t` pairs are gone).
Bugs it shook out: a call returning int into a one-byte result (the 8-bit
division helpers) loaded the high byte of the return slot after the low
byte, and an ifx on a byte in A took Z from an earlier `cpi` on it.

The divider and the 16-bit shifts (2026-10-06): `shl16`/`shr16` shift
`{n(sp), A}` as a pair for 16-bit shifts by 2 or more bits (in place when
the high byte sits at 1..3(sp), else pushed to 1(sp)) and for variable
counts (loop of `dcx`/`if c`/`shl16`); the fill is C, so the sign of an
arithmetic shift is put in C once (kept in the save shadow across the
loop's `dcx`).  Unsigned `/` and `%` on the hardware divider inline
(`_hasNativeMulFor`), the signed helpers on top of `__divuint`/
`__moduint` in `device/lib/lisa/divu.s`.  What the TT07 silicon does
(probed with `test_divprobe.c`, `lisa_isa.md` divider notes): the
result's high byte is not stored; the forms with offset bit 1 clear (or
`div 1`) write the low byte to a stray address (only `div 3` and the
slot at 2(sp) are emitted); every division leaves IX = 0 and RA = the
16-bit result (`xchg ra; txau; andi #0x7f` gives the high byte; every
division is an RA clobber for the prologue), 15 bits of it: a quotient
by 1 has no high byte there and a remainder of 0x8000 or more loses its
top bit (the regression's arith-rand found that one) - so a 16-bit
quotient or remainder by a variable divisor is `__divuint`/`__moduint`,
which check (q = a for b == 1; r = a - q*b with q 0 or 1 for b >= 0x8000).

Not done / next:
* Code quality: more peepholes
  (`ldax x; stax x`, `ldi 0; stax a; ldi 0; stax b`, compare-then-branch
  fusion), 32-bit division on the 16-bit divider.
* Bit fields, `ROT`, `GETWORD`, `IPUSH_VALUE_AT_ADDRESS` (struct
  arguments), `__critical` beyond eidi, floats in the library.
* Interrupt handlers: `__interrupt(n)` works on a core with sane interrupt
  semantics (`lisa_sim --fixed-irq`, `test_irq.c`); on the TT07 silicon an
  interrupt after an `if*` or inside an `ldx` corrupts execution
  (`test_irqhaz.c` shows it on the chip), so compiled code must run with
  interrupts off there (README-lisa.md "Interrupts on TT07").
  `device/include/lisa/tt07.h` declares the peripherals.
* A `--reti` option for a fixed core (1 word per constant byte).

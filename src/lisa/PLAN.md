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

* `char`/`bool` returned in `A`.  A two-byte result that is not a struct (int,
  pointers, bf16) comes back in IX - `ldxx` / `stxx` move it in one word, a
  pointer's tag riding along in `ix_cond` - unless the function is
  `__sdcccall(0)`.  Anything wider, or a struct, is returned in a slot the caller
  reserves on the stack directly above the return address (`ads -size` just before
  `jal`), so the callee finds it at a static offset even with varargs.
* Parameters are pushed right to left, little endian (high byte pushed first), so
  the first parameter is at the lowest address.  The caller pops them.
  Except the first one when it is a byte: it travels in A (`lisa_reg_parm`,
  SDCC's SEND right before the call, RECEIVE first in the callee, which stores
  it to a slot in its locals if it needs it in memory).  Variadic, unprototyped
  and `__sdcccall(0)` functions keep everything on the stack - inline asm that
  reads parameters from the stack wants `__sdcccall(0)`.  The library's byte
  division helpers follow the convention (a in A).
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
  `if ne ; br L` -> `bnz L` etc.; branch threading and inversion, dead
  code and labels (2026-10-06).
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

The BF16 unit (2026-10-06): `<lisa/bf16.h>` + `device/lib/lisa/bf16.s`
and `bf16c.c` expose it as `bf16_t` with explicit functions (the C
`float` stays the 32-bit software one).  Probing it on the chip found
the adder defective (the carry-out and the rounding-overflow cases;
`lisa_isa.md` BF16 notes), so add/sub are software (round to nearest
even), and `lisa_sim` got bit-exact ports of fadd/fmul/fdiv/fcmp/itof/
ftoi from the RTL.  `--bf16-float` (main.c: `lisa_options`) puts the C
float through them: `device/lib/lisa/bf16fs.s` (`__fsadd`/`__fssub`/
`__fsmul`/`__fsdiv`: the operands rounded to bf16 by `fs_round`, pushed
through IX, the bf16_t routine called) and `bf16fsc.c` (the 8/16-bit
conversions), linked as objects ahead of the libraries (`relFilesSet`,
found in `libDirsSet`; a `-l bf16float` would make the linker warn about
the generic definitions in lisa.lib), rounding every result to bf16;
`test_bf16f.c`.  `bf16_add` is assembly (`bf16add.s`: the significand
of the larger operand as {0x80|f, 0x00}, the smaller one shifted into
the pair with shr16, so a byte of guard bits and one-byte rounding) at
160-230 cycles; a float add is 240 cycles all in, a multiply 100.

Code size (2026-10-06, a 19.5K-word sample of library and test code
-6.5%): the A tracker in `gen.c` (`G.a`: what byte A holds - a stack
slot, a direct symbol or a literal - and whether Z still reflects it;
`loadA` skips the reload, `loadAZ` adds a `cpi #0` when Z is needed;
volatile objects are never tracked, labels, calls, predicated code and
stores through IX forget).  Compares against the literal 0 (`genCmp`):
signed `x < 0` is `btst 7` of the top byte, unsigned `x > 0` an or of
the bytes, and a zero literal byte in a chain is `ldax; bnz`.  Reads
through generic and `__code` pointers go out of line unless
`--opt-code-speed` (`device/lib/lisa/gptrget.s`: `__gptrget`,
`__gptrgeto` with an offset in A, `__gptrcode`, and `__gptrnext` for the
following bytes - the raw address stays in IX and the space in C, since
no IX arithmetic keeps ix_cond): 2 words per byte instead of 12 for the
first and 2 for the rest.  Peepholes: branch threading
(`labelIsUncondJump` knows `br`), branches around a `br` inverted,
unreachable code after a `br` and unused labels dropped; `condInverse`
must know every code the rules can leave (`ge`, `le`: the relaxer
inverts it again).  A 16-bit literal is pushed as `ldxs #lit; push ix`
(3 words) only when its low byte has bit 7 set: `ldx` sets ix_cond, and
`push ix` puts that out as bit 7 of the high half - so never for a
symbol address.

32-bit division (2026-10-06): `__divulong`/`__modulong` are
`device/lib/lisa/divul.s`, one body with a flag.  A divisor below 256:
four 16/8 divisions (`lddiv`, `div 0, 2(sp)` / `rem 0, 2(sp)` with a
zero byte kept at 2(sp), the result always a byte so RA is never read).
Otherwise Knuth D in base 256: VN = 2..4 divisor digits normalized by
2^s (mul/mulu by 2^s, no shift loops), the dividend into 5 digits, 5-VN
quotient digits, each estimated as {top two digits} / VT on the divider
(clamped to 255 when the top digit equals VT), refined with VT2, the
product subtracted with the borrow folded into the next product byte so
the silicon's `sub M` is always guarded by `cpi #1`, one add-back on a
borrow; the remainder un-normalized by multiplying with 2^(8-s).  The
frame is 30 bytes (the quotient digits go straight into the return
slot; the test `test_divl.c` runs on the 128-byte default stack).
~105 instructions for a byte divisor, 500-600 for 16/32-bit ones, 439
words for both.  `test_divl.c` (table, signed, 200 LCG cases against a
bit-serial reference) passes on the chip.

No `swap n(sp)` (2026-10-06): on the TT07 silicon it addresses sp + n -
512 (lisa_isa.md), which only the 128-byte RAM hides, so `emitAluA` puts
A on the stack and the literal / loaded byte in A for the commutative
ops, pushes both for sub / cmp against an SFR / CODE / STL byte, and
genIpush parks a live A in IX's low byte (tax / txa) or swaps through
`spix; adx #1; swap 0(ix)`; bf16add.s tests the hidden bit through a
push.  lisa_sim models the silicon address.

The data cache on the chip (2026-10-06, the RP2040 as a 32K SPI RAM on
CS1: `mbell_micropython` branch `lisa_spi_ram`, `hw_test.mjs spiram=`):
every sdcc_test suite passes through it.  Besides `swap n(sp)`, the
silicon's RMW instructions work on stale data when the byte misses the
cache, so `genAddSub`'s inx/dcx, the shift loops' dcx, the startup
copy loops (`lisa_genInitStartup`) and divul.s / bf16add.s touch each
byte's line with a `cmp` first.  And the cache maps bit 14 of the data
address to nothing (data_cache8.v uses cache_map[1] for qspi_addr[14]),
so X and X ^ 0x4000 alias; `--tt07-cache` (main.c) puts the stack at
0x7fff with `--stack-size` (2K default) reserved and hands the linker
the data limit 0x4000 - reserve as -X, which lkarea.c enforces for
LISA (lkmain.c consumes -S).  lisa_isa.md has the three notes.

`x >= 0` as a value (2026-10-06): SDCC hands it over as `t = x < 0; r =
!t`; `notForOp` (like ifxForOp) finds the `!` that consumes a compare's
temporary, and genCmp / genCmpEQorNE produce the inverted truth value
straight into its result - `btst 7; ldac ne` for a signed `>= 0`, the
chain's constants swapped otherwise, `eq`/`ne` flipped for `!(a == b)`.

Pointer offsets and signed division (2026-10-07): SDCCopt's
`offsetFoldGet` / `offsetFoldUse` run for lisa too (a folded offset of
0..255 - the unsigned 9-bit field of `ldax n(ix)`, a byte for the
helpers - and never negative, since a wrap below a stack object could
set the code-space tag), so `p->member`, `p[k]` and `&local[k]` are one
access carrying the offset instead of a 16-bit pointer add first (the
`rd_scc` of test_ptroff: 6 words gone).  gptrget.s has `__gptrcodeo` (a
code pointer plus the offset in A, without reading byte 0 first) and
`__gptrprev` (the byte before the last one read), and
IPUSH_VALUE_AT_ADDRESS - a struct through a generic or code pointer -
goes through the helpers too, last byte first (`push a` leaves C, the
space tag between the calls, alone).  `__divslong` / `__modslong`
(divul.s) and `__divsint` / `__modsint` (divu.s) take the operands'
absolute values in place (`neg4` / `neg2` through IX, the genUminus
sequence), run the unsigned code and negate the result once; frame slot
28 and the flag byte pushed at 1(sp) hold the sign.  The four C wrappers
were 468 words and called the unsigned routine with everything pushed
again; the entries are 107 words.  test_ptroff (built `--tt07-cache`),
test_signed 16-20 and test_divl 55-60 cover it, on the chip as well.

Byte division (2026-10-07): the eight byte helpers (`__divschar`,
`__modschar`, the mixed `__divsuchar` / `__divuschar` / `__modsuchar` /
`__moduschar`, and the unsigned pair for a `_BitInt`) are in divu.s: the
operands' magnitudes in place, `div 3` / `rem 3`, the result - at most
255 - negated as 16 bits when the signed operand's sign says so.  They
keep SDCC's int-returning type for the byte helpers (SDCCsymt.c): the
quotient of two chars is an int in C, SDCC still hands char operands to
the byte helper, and -128 / -1 is 128, 255 / -1 is -255 - a byte-returning
version broke `xa / xb == -14` in test_divsh (the high byte of the compare
was never written).  89 words for the 272 of the C files, which called
the 16-bit routines with everything pushed again; test_signed 21-27.

The return slot and the pointer copies (2026-10-07): a temporary that
exists only to be returned - one definition, in the iCode right before
the RETURN that is its one use, of the return slot's size
(`lisaRetSlotTemp`, ralloc.c) - gets no spill location, and
`aopForRetSlot` (gen.c) makes the slot (entry-SP-relative 1..ret_size)
its asmop: the defining iCode writes there and genReturn's `cheapMove`
finds nothing to copy.  The adjacency is what makes two returned
temporaries unable to clobber each other.  And for lisa `offsetFoldUse`
(SDCCopt.c) no longer leaves `t = p` behind the folded add: with the get
right after it, the get reads the pointer itself, and so on back through
a chain of adjacent single-use copies - the one SDCC makes of a
parameter included (nothing coalesces temporaries on a one-register
target).  `rd_scc` in test_ptroff: 29 words to 9; `(unsigned int)(l >>
16)`: 18 to 5.

Byte division with a byte result (2026-10-07): `_hasNativeMulFor` keeps
a signed or mixed byte pair with a byte result, and genDivMod calls the
byte-returning `__divschar8` family (divu.s: a byte back in A, no return
slot) - `push b; push a; jal; ads #2`, 6 words for the 9 of the
int-returning call; a dividend already in A is parked in IX's low byte
while b is loaded.  And `lisaNarrowByteDiv` (SDCCopt.c, before
convertToFcall) turns `(T8)((int)a / k)` and `%` - the C promotion
around a char divided by a literal that fits a byte - back into the byte
operation: the int quotient of two bytes fits a byte, bar 128 and -255,
which the cast truncates exactly as the helpers do; a literal of the
other signedness (a signed char by 200, an unsigned one by -3) is the
mixed helper.  `a / 7` on a signed char: 28 words (sign extension, four
pushes, `__divsint`, the slot copies) to 9.  test_signed 28-29.

The first byte parameter in A (2026-10-07): `lisa_reg_parm` claims the
first parameter when it is a byte (SDCC's register-parameter machinery:
`options.sdcccall = 1`, the SEND emitted right before the call since
SDCC walks the parameters last to first, the RECEIVE first in the
callee), `genSend` loads A, `genReceive` stores it to the parameter's
slot in the locals unless the allocator keeps it in A or never reads
it, and `genCall` keeps A across the IX load of a function-pointer
call.  Variadic, unprototyped and `__sdcccall(0)` functions stay on the
stack (inline asm that reads parameters from the stack wants
`__sdcccall(0)`: test_aluprobe).  The library's byte division helpers
take a in A, and genDivMod's own calls of the byte-returning ones push
b only.  `putc('x')`: `ldi; jal` for `ldi; push; jal; ads #1`; the test
corpus (15 programs with the library) 34214 to 33521 words, -2.0%.
Peephole 28 merges adjacent `ads` (`ads` takes a 10-bit immediate:
`S_SIMM10` in sdaslisa, the ISA doc said 8).

Copies share a slot (2026-10-07): a temporary born as a copy (an
assignment, or a cast between types of one size - genCast is a plain
move then) takes the place of what it copies (`copyHome`, ralloc.c,
from `createStackSpil`): of a spilled temporary that dies at the copy,
its spill location, when the clash sets allow (SDCC does not count live
ranges that only touch at the copy as a clash - SDCClrange.c); of a
never-written parameter, the parameter's own slot - unconditionally
when the temporary is never written after the copy either (the two stay
equal, whatever else reads the parameter), and when it is written (a
loop variable: the copy is its first definition in sequence, the
increments follow), only if nothing but the copy reads the parameter
and the copy is outside any loop.  The temporaries homed in a parameter
are tracked like a spill location's; other locals are left alone
because redoStackOffsets lets disjoint blocks share their space.  The
loop-variable case is what SDCC's loop passes leave: the parameters a
loop modifies, copied into temporaries first - `zero2(p, n)` in the
scan had a 3-byte frame and a 6-word prologue for it, now neither (`inx
4(sp)` on the parameter itself); `__memcpy`'s prologue went from five
copies to one.  cheapMove finds the same slot on both sides of the copy
and emits nothing.  The corpus: 33521 to 33263 words.  The copy runs
that remain are mostly call results leaving the return slot (the slot's
ABI, see the IX idea below).

A two-byte result in IX (2026-10-07): `lisaRetInIX` (ralloc.c) - a
two-byte result that is not a struct, unless the function is
`__sdcccall(0)`.  genReturn loads IX (`ixLoadValue`: `ldxx` from the
stack, `ldx` for a literal with bit 15 set, else through A with tax /
taxu, the high byte parked on the stack when its own load needs IX);
genCall stores it with `stxx` into the result's slot - and notes that
IX still holds it, so a deref right after needs no `ldxx` - or through
A for a global; `return f(x)` passes IX straight through
(`lisaRetIXTemp`: the call's result, used by the RETURN right after it,
gets no spill slot, no store, no load).  The library followed: divu.s
reserves the old slot's pair itself (`ads #-2` before `sra`, so its
bodies keep their frames) and ends with `ldxx 1(sp)`, bf16.s returns
facc with tax / taxu, bf16add.s keeps its result pair in its own frame,
setjmp.s hands 0 / the longjmp value over in IX, bf16fs.s takes the
bf16 results from IX.  A leaf that computes its result pays an `ads
#-2 .. ldxx 1(sp); ads #2` where the slot mapping used to be free (gw16
5 to 8 words), every call site saves the 4 words of copying and its
`ads`: the corpus 33263 to 32693 words.

Predicated compare chains, measured and not adopted (2026-10-07): the
scan's commonest short branch is the second byte of a multi-byte
equality compare (`bnz L; ldax hi; cmp/cpi; L:`, ~240 sites), and
`iftt z` fits it exactly - same size, no taken branch.  Timed on the
chip with TIMER1 (1 ms steps, `LISA_NO_IFTT` for the A/B): a 16-bit
`while (i != n) i++` 3000 times, 143 ms with the branch, 144 predicated;
a 32-bit one 1200 times, 76 ms with the branches, 115 predicated.  The
4-word instruction cache explains it: a short forward branch whose
target lies in the current line costs nothing, while a predicated chain
streams every one of its words through the cache - 12 words for the long
compare, three line fetches where the branch form takes two.  So on the
TT07 predication only pays where it removes words (an if/else join
folded into `ifte`, ~20 sites in the scan); the generator keeps the
branches.

The read-modify-write workaround only under --tt07-cache (2026-10-07):
the `cmp` of each byte before an `inx` / `dcx` (genAddSub, the shift
loops, the startup copy loops) is for a data-cache miss; a program for
the direct 128-byte RAM has no cache, so `lisa_tt07_cache` now gates
it.  The library comes in two: `lib/lisa` and `lib/lisa-cache`, the
latter compiled with `--tt07-cache` - the one Makefile.in, written by
configure into `device/lib/lisa/Makefile` and
`device/lib/lisa-cache/Makefile` (the directory name sets PORTDIR and
the flag; `device/lib/lisa-cache` exists in the source tree for the
`../lisa` sources to be found) - and `get_model` returns `lisa-cache`
under the option, which is where SDCC takes the library directory from.
The hand-written .s keep their `cmp`s in both (a few words).  The corpus
32693 to 32092 words.

The string routines in assembly (2026-10-07): `memcpy` (with
`__memcpy`), `memset`, `strcpy` and `strlen` are
`device/lib/lisa/{memcpy,memset,strcpy,strlen}.s` (the Makefile's `.s`
rule goes before incl.mk so that they win over `../memcpy.c` and
`../strlen.c`; `_memset.c` is filtered out of COMMON_SDCC).  A first
cut that walked the source with `__gptrnext` a byte at a time, as the C
does, was no faster on the chip: it is fetch-bound at roughly 4.5 us
per executed word (a 4-word line from the flash every four words and
at every taken branch), and that loop executes as many words per byte
as the compiler's.  So each tests the source's space once (`txau; btst
7`) and runs one of two loops, RAM read in IX or the ldi/ret pairs
called directly (`call ix`, RA saved once by the `sra`), the cursors
swapped through IX with `ldxx` / `stxx` and bumped by `adx`, the count
brought down in place by `dcx` (its low byte, then 256 per high byte,
one more when the low byte's pass is partial; `dcx` sets Z).  `memset`
keeps the fill byte in A (`dcx` leaves it) and the pointer in IX, 4
words per byte.  `strlen` of RAM walks IX alone and subtracts s
(`subax` / `subaxu`; every IX arithmetic sets ix_cond, and the caller
stores IX with the tag as bit 15, so the result goes through `txau;
andi #0x7f; taxu`); of code space it halves the words walked with
`shr16` on the stack (tag masked and C cleared first, so neither amode
shifts anything in).  A generated `lisamodel.inc` (TT07_CACHE, from
the Makefile) gives the lisa-cache build its `cmp` before each `inx` /
`dcx`; the older .s keep theirs in both.  On the chip (TIMER1, 23
bytes x 32, ms): memset 41 to 7, memcpy from RAM 101 to 42, from code
136 to 66, strcpy 77 to 42, strlen of RAM 77 to 5, of code 113 to 43.
Sizes in words: memcpy 49 (54 under the cache), memset 16, strcpy 39,
strlen 47; the second loop costs - the 22 checked sdcc_test programs
plus the monitor went 47564 to 47710 words (test_strs +48, test_printf
+42, test_pfu and test_owl +26, test_regress +16, test_bigframe -12).
test_strs 5-14: sources in code space and in RAM, empty strings, n ==
0, the returns.

A shared loop, measured and not adopted (2026-10-07): one loop for
both spaces, the space kept in C by `savec` and `restc; ifte c; call
ix; ldax 0(ix)` picking the read (the pair's `ret` brings cond[0] = 0
back from ra_cond, so the ldax is skipped after a call), saves 7 words
of memcpy (9 under the cache) and 5 of strcpy - the corpus 47710 to
47686 - for the two words per byte: on the chip memcpy from RAM 42 to
54 ms and strcpy 42 to 54, copies from code space unchanged (66 to
67).  The two loops stay.

strcmp in assembly (2026-10-07, `device/lib/lisa/strcmp.s`): both
arguments are generic, so four loops, one per pair of spaces, the
arguments themselves the cursors; with s2 in RAM *s1 is compared
against it in place (`cmp 0(ix)`), with s2 in code space *s2 is read
first into a pushed byte.  The TT07's `cmp` / `sub` subtract the
carry-in too, so C is 0 going into each (`ldc` once, then the loop's
`cpi #0`), and a 0 operand reports a borrow it did not make, so the
sign of the result (the unsigned difference, in IX) is forced positive
when *s2 is the NUL.  On the chip (23 bytes x 32, ms): RAM/RAM 175 to
43, RAM/code and code/RAM 212 to 69, code/code 249 to 96; 102 words
against the C's 40 (test_regress and test_pfu +62 each; the monitor
does not link it).  test_strs 15-19.

The compiled-loop scan (2026-10-07): 303 loops in the corpus's .asm
(the startup copies left out), 15.6K body words, loads and stores of
stack bytes 37% of them.  Timed on the chip (23 iterations x 32): a
`while (*p)` byte walk through a generic pointer 88 ms, the same loop
inverted by hand 111 (SDCC reads *p again for the bottom test; the
top-tested form reuses the one load), and a counted `while (i < n)` 87
ms top- or bottom-tested alike - so loop inversion gains nothing on
this chip and is not done (the back-branch is a word, not a line
fetch, as the predication measurement also found).  What does pay: a
`__near` pointer (unqualified pointers are generic), whose read is
`ldxx; ldax 0(ix)` with no space test - the same walk 54 ms - and that
found a bug: a near pointer kept in IX for the body's read was bumped
in place by `inx` for p++ and the loop's test read through the stale
IX, one iteration too many; aTrack now invalidates IX on any write into
the slot it mirrors (stax, inx / dcx, stxx, swap, shl16 / shr16;
test_regress 24; no other program's code changed).  Still open from
the scan: a byte-indexed array access `a[i]` is 10 words (a 16-bit add
into a temporary, then ldxx / ldax) where `ldx #_a; ldax i(sp); addax;
ldax 0(ix)` is 5, and a variable shift of a long is 14-17 words per
bit (ldax / shl / stax per byte, the count tested at the top).

The indexed address (2026-10-07, genIndexedAddr in genAddSub): the
'+' of `a[i]` or `p + i` - a data symbol, the address of a stack
object or a near pointer in a stack slot, plus a one-byte unsigned or
two-byte index in a stack slot or in A - forms the address in IX (`ldx
#_a; ldax i(sp); addax`, then `ldax hi(sp); addaxu` for two bytes), stores
it with stxx and leaves IX holding it, which ixLoadPtr finds for the
access that follows: 6 words for `a[i]` where the 16-bit add into the
temporary and its ldxx were 10.  addax sets ix_cond, so the stored
temporary carries bit 15 - fine for `ldax` / `stax n(ix)` and nothing
else, so only a temporary of a non-generic pointer type whose every use
(OP_USES, iCodehTab) is a GET_VALUE_AT_ADDRESS or SET_VALUE_AT_ADDRESS
through it takes this path; `&a[i]` that escapes, and `p[i]` through a
generic pointer, keep the plain add.  On the chip the counted `while (i
< n) s += a[i]` loop went 87 to 75 ms; the corpus 48518 to 48448, the
monitor alone -46 (its `line[n]`), the test programs index through
pointers and mostly do not change.  test_index.

Shifts by a variable count (2026-10-07, genShift): the loops test the
count once (`ldax; bz`, which also brings its line into the TT07 data
cache) and count it down after each step - `dcx` sets Z, so `dcx; bnz`
closes the loop - with the bit shifted in from C: 0, or the sign kept
in the save shadow (`dcx` and the cache's `cmp` clobber C).  A byte
stays in A throughout (`ldc #0; shl; dcx; bnz`, 4 words a bit, were 8);
a word is `shl16` / `shr16` on {1(sp), A}, 4 words a bit (were 6); a
long goes by whole bytes first - while 8 or more remain the bytes move
up or down one (the sign filled in from the top byte) and the count
loses 8 (`ldc #0; adc #0xf8`: the TT07 adc adds (k + C) & 0xff) - then
by bits, the sign computed once (15 words a bit, were 20 for every
bit).  `shl16` / `shr16` update neither C nor Z, so two cannot be
chained for a long.  On the chip (x32): `ulong >> 20` 54 to 16 ms,
`long >> 20` 55 to 18, `ulong << 9` 27 to 8, `uint >> 7` 11 to 5,
`uchar << 7` 10 to 5; the corpus 48448 to 48482 (the byte loop is
static code at each variable long-shift site: test_divl +18, test_divsh
+12).  test_vshift.  This is what the software float library's `mant
>>= expd` and `l <<= exp` run.

Two bugs the regression suite found the same day (`make test-lisa`,
not run since the parameter in A and the copy homing went in; it is
clean again, 32223 tests): a function with a frame past 511 bytes and
its byte parameter in A homed that parameter at entry offset 0 - RA's
low byte once sra has run - because lisaFarFrameLayout took only
non-parameter objects from the frame, and a register parameter is
allocated as a local (SDCC's bigstack; test_bigframe 7).  And copyHome
let a temporary written later share a parameter's slot through an
intermediate never-written copy of it without the parameter being dead:
memmove's `d`, a copy of a copy of `dst`, was incremented in dst's slot
and `return dst` read it (SDCC's memory suite; test_strs 20).

Not done / next:
* `__critical` is just eidi (no interrupt state to save: `ie` cannot be
  read).

ROT and GETWORD (2026-10-07): `hasExtBitOp` claims GETWORD (two byte
moves, genGetWord: `(unsigned int)(l >> 16)` was 18 words of copies) and
ROT for a byte by any count, a word by 1, 8 or 15 and a long by 16
(genRot).  `shl` / `shr` rotate through C under the port's amode 1, so a
byte rotated left by s is `ldc #0` and s times `shl; adc #0` (the adc
never carries, bit 0 is clear), right by r is r times `push a; shr; pop
a; shr` (the first shr only fetches bit 0 into C), whichever is shorter;
a word by 1 chains the two shl with the carry added to the low byte, by
15 fetches the low byte's bit 0 into C and shifts the high then the low
byte right through it; by half the width the halves are swapped (through
the stack when the result is over the source).  `(x << 1) | (x >> 7)` on
a byte went from 17 words to 3.  test_rot.
* Interrupt handlers: `__interrupt(n)` works on a core with sane interrupt
  semantics (`lisa_sim --fixed-irq`, `test_irq.c`); on the TT07 silicon an
  interrupt after an `if*` or inside an `ldx` corrupts execution
  (`test_irqhaz.c` shows it on the chip), so compiled code must run with
  interrupts off there (README-lisa.md "Interrupts on TT07").
  `device/include/lisa/tt07.h` declares the peripherals.
* A `--reti` option for a fixed core (1 word per constant byte).

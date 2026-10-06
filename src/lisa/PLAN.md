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
* `ralloc.c`: no allocatable registers in the first cut — every variable and
  iTemp is spilled to the stack (`AOP_STK`) or lives in DATA.  Second cut: keep
  an iTemp in `A` when its only use is the next iCode (the z80 "surviving in A"
  idea), which removes most `stax t ; ldax t` pairs.
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
printf with %d %u %x %ld %c %s and widths).  Everything lives on the stack;
A is scratch; frames are packed by block scope and spill slots are shared
between non-overlapping temporaries.  Branches out of the +-1024-word range
are relaxed after the peephole pass.  Instructions after `if/iftt/ifte`
are emitted with a `.p` suffix so peephole rules leave them alone.

Not done / next:
* Run on the TT07 chip (LISA Commander needs an Intel HEX loader; the
  simulator mirrors the RTL but the silicon quirks are real).
* Code quality: keep short-lived temporaries in A (the pdk-style
  tree-decomposition allocator with a dry-run cost model is the plan;
  `emit2`/`cost` are already structured for it), more peepholes
  (`ldax x; stax x`, `ldi 0; stax a; ldi 0; stax b`, compare-then-branch
  fusion), `shl16/shr16` for 16-bit shifts at 0..3(sp), hardware `div`/`rem`.
* Bit fields, `ROT`, `GETWORD`, `IPUSH_VALUE_AT_ADDRESS` (struct
  arguments), `__critical` beyond eidi, floats in the library.
* Interrupt handlers: generated (push a / push ix / sra ... rets) but not
  tested.
* A `--reti` option for a fixed core (1 word per constant byte).

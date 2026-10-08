/*-------------------------------------------------------------------------
  gen.c - code generator for LISA (Little ISA).

  Modelled on the Padauk port by Philipp Klaus Krause.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
-------------------------------------------------------------------------*/

/*
  Machine model used here (see PLAN.md for the ABI):

  * A is the only register; everything else lives on the stack (n(sp)),
    in data memory (lda/sta, reachable directly up to address 0x1ff),
    in peripheral registers (0x200..0x3ff, lda/sta only) or in code
    space (ldi/ret pairs read with call ix).
  * IX is a transient pointer.  G.ix remembers what it holds so that
    "ldx #sym" is not repeated.
  * add/sub always include C.  ldi clears C, loads/stores leave it alone,
    but every load of A sets Z.
  * Stack offsets: an AOP_STK byte stores its address relative to SP at
    function entry; the emitted offset adds G.stack.pushed (bytes the
    SP has moved down since: saved RA, locals, pushes).
*/

#include "ralloc.h"
#include "gen.h"
#include "dbuf_string.h"

#define D(x) do if (options.verboseAsm) { x; } while (0)

#define UNIMPLEMENTED do {wassertl (regalloc_dry_run, "Unimplemented"); cost (500, 500);} while(0)

static bool regalloc_dry_run;
static unsigned int regalloc_dry_run_cost_words;
static float regalloc_dry_run_cost_cycles;

static struct genState
{
  struct
    {
      int pushed;         /* bytes the SP is below its value at function entry */
      int size;           /* local frame size (locals + spill slots) */
      int param_offset;   /* entry-SP-relative offset of the first parameter byte, minus sym->stack: 1 + return slot */
      int locals_base;    /* entry-SP-relative offset of the byte above the locals: -(saved RA, saved ISR registers) */
      int ret_size;       /* size of the return slot (0 if the function returns <= 1 byte, or in IX) */
      bool ret_ix;        /* the result comes back in IX (lisaRetInIX) */
    } stack;
  bool ra_saved;          /* the prologue did sra */
  /* Track content of IX */
  struct
   {
     AOP_TYPE type;       /* AOP_INVALID: unknown; AOP_IMMD: #sym+off; AOP_STL: SP + off (entry-relative) */
     const char *base;
     int offset;
   } ix;
  /* Track content of A: the byte it holds, so that loading it again is
     skipped (see aTrack), and whether Z reflects it */
  struct
   {
     enum { A_UNKNOWN, A_STK, A_DIR, A_LIT } kind;
     int stk;             /* A_STK: entry-SP-relative offset */
     char text[48];       /* A_DIR: the lda operand, A_LIT: the ldi operand */
     bool zvalid;         /* Z == (A == 0) */
   } a;
}
G;

static void adjustStack (int n);
static void ixLoadValue (const asmop *aop);
static unsigned char litByte (const asmop *aop, int offset);
static bool loadA (const asmop *aop, int offset);

static struct asmop asmop_a, asmop_zero, asmop_one, asmop_mone;
static struct asmop *const ASMOP_A = &asmop_a;
static struct asmop *const ASMOP_ZERO = &asmop_zero;
static struct asmop *const ASMOP_ONE = &asmop_one;
static struct asmop *const ASMOP_MONE = &asmop_mone;

void
lisa_init_asmops (void)
{
  asmop_a.type = AOP_REG;
  asmop_a.size = 1;
  asmop_a.aopu.bytes[0].in_reg = true;
  asmop_a.aopu.bytes[0].byteu.reg = lisa_regs + A_IDX;
  asmop_a.valinfo.anything = true;

  asmop_zero.type = AOP_LIT;
  asmop_zero.size = 1;
  asmop_zero.aopu.aop_lit = constVal ("0");
  asmop_zero.valinfo.anything = false;
  asmop_zero.valinfo.nothing = true;
  asmop_zero.valinfo.nonnull = false;
  asmop_zero.valinfo.min = 0;
  asmop_zero.valinfo.max = 0;
  asmop_zero.valinfo.knownbitsmask = 0xffffffffffffffffull;
  asmop_zero.valinfo.knownbits = 0;

  asmop_one.type = AOP_LIT;
  asmop_one.size = 1;
  asmop_one.aopu.aop_lit = constVal ("1");
  asmop_one.valinfo.anything = true;

  asmop_mone.type = AOP_LIT;
  asmop_mone.size = 8; // Maximum size for asmop.
  asmop_mone.aopu.aop_lit = constVal ("-1");
  asmop_mone.valinfo.anything = true;
}

/* Instructions following an if / iftt / ifte are predicated.  They are
   emitted with a ".p" suffix (ldi.p) which the assembler ignores, so that
   no peephole rule can merge or delete them. */
static int predicated = 0;

static void
aInvalidate (void)
{
  G.a.kind = A_UNKNOWN;
  G.a.zvalid = false;
}

static void ixInvalidate (void);

/* The A tracker: every emitted instruction passes through here.  A load
   of a near stack byte, a direct byte or a literal is remembered; a
   store makes A known as the stored byte when it was not; anything else
   that writes A, a write that may hit the remembered byte, a label and
   a call forget it.  Z reflects A after a load, not after a compare or
   test.  A predicated instruction may not execute: whatever it would
   have done to A is unknown. */
static void
aTrack (const char *inst, const char *opnd, bool pred)
{
  int n;
  char junk;

  if (!inst[0] || inst[0] == ';')
    return;
  /* IX mirrors a stack slot (G.ix of type AOP_STK, ixLoadValue): a write
     into either of its bytes - the inx of a pointer kept there - makes
     IX stale */
  if (G.ix.type == AOP_STK &&
      (!strcmp (inst, "stax") || !strcmp (inst, "inx") || !strcmp (inst, "dcx") || !strcmp (inst, "stxx") ||
       !strcmp (inst, "swap") || !strcmp (inst, "swapi") || !strcmp (inst, "shl16") || !strcmp (inst, "shr16")) &&
      sscanf (opnd, "%d(sp%c", &n, &junk) == 2 && junk == ')')
    {
      int lo = n - G.stack.pushed, bytes = !strcmp (inst, "stxx") ? 2 : 1;
      if (lo + bytes > G.ix.offset && lo <= G.ix.offset + 1)
        ixInvalidate ();
    }
  /* a remembered stack byte that has been dropped (at or below SP) may be
     overwritten by a push with something else */
  if (G.a.kind == A_STK && G.a.stk + G.stack.pushed <= 0)
    aInvalidate ();
  /* what writes A */
  if (!strcmp (inst, "ldax") || !strcmp (inst, "lda") || !strcmp (inst, "ldi") ||
      !strcmp (inst, "pop") && !strcmp (opnd, "a") ||
      !strcmp (inst, "swap") || !strcmp (inst, "swapi") || !strcmp (inst, "txa") || !strcmp (inst, "txau") ||
      !strcmp (inst, "add") || !strcmp (inst, "adc") || !strcmp (inst, "sub") || !strcmp (inst, "and") ||
      !strcmp (inst, "andi") || !strcmp (inst, "or") || !strcmp (inst, "xor") || !strcmp (inst, "mul") ||
      !strcmp (inst, "mulu") || !strcmp (inst, "shl") || !strcmp (inst, "shr") || !strcmp (inst, "ldac") ||
      !strcmp (inst, "ldirq") || !strcmp (inst, "tfa") || !strcmp (inst, "call") || !strcmp (inst, "jal") ||
      !strcmp (inst, "div") || !strcmp (inst, "rem") || !strcmp (inst, "shl16") || !strcmp (inst, "shr16") ||
      !strcmp (inst, "jmp") || !strcmp (inst, "ret") || !strcmp (inst, "rets"))
    {
      aInvalidate ();
      if (pred)
        return;
      if (!strcmp (inst, "ldax") && sscanf (opnd, "%d(sp%c", &n, &junk) == 2 && junk == ')')
        {
          G.a.kind = A_STK;
          G.a.stk = n - G.stack.pushed;
          G.a.zvalid = true;
        }
      else if (!strcmp (inst, "lda") || !strcmp (inst, "ldi"))
        {
          G.a.kind = !strcmp (inst, "lda") ? A_DIR : A_LIT;
          strncpy (G.a.text, opnd, sizeof (G.a.text) - 1);
          G.a.text[sizeof (G.a.text) - 1] = 0;
          G.a.zvalid = true;
        }
      else if (!strcmp (inst, "ldax") || !strcmp (inst, "pop") || !strcmp (inst, "add") || !strcmp (inst, "adc") ||
               !strcmp (inst, "sub") || !strcmp (inst, "and") || !strcmp (inst, "andi") || !strcmp (inst, "or") ||
               !strcmp (inst, "xor") || !strcmp (inst, "mul") || !strcmp (inst, "mulu") || !strcmp (inst, "swap") ||
               !strcmp (inst, "swapi"))
        G.a.zvalid = true;
      return;
    }
  /* what writes memory A may be known as, or Z */
  if (!strcmp (inst, "stax") || !strcmp (inst, "sta"))
    {
      if (pred)
        return;
      if (G.a.kind == A_UNKNOWN)
        {
          if (!strcmp (inst, "stax") && sscanf (opnd, "%d(sp%c", &n, &junk) == 2 && junk == ')')
            {
              G.a.kind = A_STK;
              G.a.stk = n - G.stack.pushed;
            }
          else if (!strcmp (inst, "sta"))
            {
              G.a.kind = A_DIR;
              strncpy (G.a.text, opnd, sizeof (G.a.text) - 1);
              G.a.text[sizeof (G.a.text) - 1] = 0;
            }
        }
      else if (!strcmp (inst, "stax") && (G.a.kind == A_STK || G.a.kind == A_DIR) && !strstr (opnd, "(sp)"))
        aInvalidate ();                 /* through IX: may hit anything */
      return;
    }
  if (!strcmp (inst, "inx") || !strcmp (inst, "dcx") || !strcmp (inst, "stxx"))
    {
      G.a.zvalid = false;
      if (G.a.kind == A_STK && sscanf (opnd, "%d(sp%c", &n, &junk) == 2 && junk == ')')
        {
          if (n - G.stack.pushed == G.a.stk || !strcmp (inst, "stxx") && n + 1 - G.stack.pushed == G.a.stk)
            aInvalidate ();
        }
      else if (G.a.kind == A_STK || G.a.kind == A_DIR)
        aInvalidate ();                 /* through IX */
      return;
    }
  if (!strcmp (inst, "cpi") || !strcmp (inst, "cmp") || !strcmp (inst, "btst") || !strcmp (inst, "ldz") ||
      !strcmp (inst, "notz") || !strcmp (inst, "cpx") || !strcmp (inst, "fcmp") || !strcmp (inst, "lddiv"))
    G.a.zvalid = false;
  /* the rest leaves A and its memory alone: push, ads, adx, ldx, spix, tax,
     taxu, sra, lra, push ix, pop ix, ldxx, ldc, savec, restc, if, br, bz,
     bnz, eidi, amode, nop, the fpu register moves */
}

static void
emit2 (const char *inst, const char *fmt, ...)
{
  if (!regalloc_dry_run)
    {
      va_list ap, ap2;
      char buf[32], opnd[64];
      bool pred = predicated > 0 && inst[0] && inst[0] != ';';

      va_start (ap, fmt);
      va_copy (ap2, ap);
      vsnprintf (opnd, sizeof (opnd), fmt, ap2);
      va_end (ap2);
      aTrack (inst, opnd, pred);

      if (pred)
        {
          predicated--;
          SNPRINTF (buf, sizeof (buf), "%s.p", inst);
          inst = buf;
        }
      if (!strcmp (inst, "if"))
        predicated = 1;
      else if (!strcmp (inst, "iftt") || !strcmp (inst, "ifte"))
        predicated = 2;

      va_emitcode (inst, fmt, ap);
      va_end (ap);
    }
}

static void
cost (unsigned int words, float cycles)
{
  regalloc_dry_run_cost_words += words;
  regalloc_dry_run_cost_cycles += cycles;
}

static void
ixInvalidate (void)
{
  G.ix.type = AOP_INVALID;
}

/* Is the register free after this iCode (its content neither used later
   nor the result)?  From the allocator's rSurv. */
static bool
regDead (int idx, const iCode *ic)
{
  wassert (idx == A_IDX);
  return (!bitVectBitValue (ic->rSurv, idx));
}

/* Is this operand a one-byte temporary that the allocator put in A? */
static bool
opInA (const operand *op)
{
  if (!op || !IS_SYMOP (op) || !IS_ITEMP (op))
    return (false);
  const symbol *sym = OP_SYMBOL_CONST (op);
  return (getSize (sym->type) == 1 && sym->regs[0] == lisa_regs + A_IDX);
}

static void
emitBranch (const char *inst, const symbol *target)
{
  if (!regalloc_dry_run)
    emit2 (inst, "!tlabel", labelKey2num (target->key));
  cost (1, 2);
}

/* Any label is a control flow merge: forget IX. */
static void
emitLbl (const symbol *lbl)
{
  if (!regalloc_dry_run)
    emitLabel (lbl);
  ixInvalidate ();
  aInvalidate ();
}

/*---------------------------------------------------------------------*/
/* lisa_emitDebuggerSymbol - associate the current code location       */
/*   with a debugger symbol                                            */
/*---------------------------------------------------------------------*/
void
lisa_emitDebuggerSymbol (const char *debugSym)
{
  genLine.lineElement.isDebug = 1;
  emit2 ("", "%s ==.", debugSym);
  genLine.lineElement.isDebug = 0;
}

/*-----------------------------------------------------------------*/
/* aopInReg - asmop from offset in the register                    */
/*-----------------------------------------------------------------*/
static bool
aopInReg (const asmop *aop, int offset, short rIdx)
{
  if (aop->type != AOP_REG)
    return (false);
  return (aop->aopu.bytes[offset].in_reg && aop->aopu.bytes[offset].byteu.reg->rIdx == rIdx);
}

static bool
aopOnStack (const asmop *aop)
{
  return (aop->type == AOP_STK);
}

/* Both bytes at the same place? */
static bool
aopSame (const asmop *aop1, int offset1, const asmop *aop2, int offset2, int size)
{
  for(; size; size--, offset1++, offset2++)
    {
      /* a byte beyond an operand is its zero extension, never "the same" */
      if (offset1 >= aop1->size || offset2 >= aop2->size)
        return (false);
      if (aop1->type == AOP_REG && aop2->type == AOP_REG &&
        aop1->aopu.bytes[offset1].in_reg && aop2->aopu.bytes[offset2].in_reg &&
        aop1->aopu.bytes[offset1].byteu.reg == aop2->aopu.bytes[offset2].byteu.reg)
        continue;
      if (aop1->type == AOP_LIT && aop2->type == AOP_LIT &&
        byteOfVal (aop1->aopu.aop_lit, offset1) == byteOfVal (aop2->aopu.aop_lit, offset2))
        continue;
      if ((aop1->type == AOP_DIR || aop1->type == AOP_SFR) && aop1->type == aop2->type &&
        aop1->aopu.immd_off + offset1 == aop2->aopu.immd_off + offset2 &&
        !strcmp (aop1->aopu.immd, aop2->aopu.immd))
        continue;
      if (aop1->type == AOP_STK && aop2->type == AOP_STK &&
        offset1 < 8 && offset2 < 8 && aop1->aopu.bytes[offset1].byteu.stk == aop2->aopu.bytes[offset2].byteu.stk)
        continue;
      return (false);
    }
  return (true);
}

/*-----------------------------------------------------------------*/
/* aopIsLitVal - asmop from offset is val                          */
/*-----------------------------------------------------------------*/
static bool
aopIsLitVal (const asmop *aop, int offset, int size, unsigned long long int val)
{
  wassert_bt (size <= sizeof (unsigned long long int));

  for(; size; size--, offset++)
    {
      unsigned char b = val & 0xff;
      val >>= 8;

      // Bytes beyond a literal's size: its sign extension
      if (aop->size <= offset && aop->type == AOP_LIT)
        {
          if (litByte (aop, offset) != b)
            return (false);
          continue;
        }
      // Leading zeroes
      if (aop->size <= offset && !b)
        continue;

      // Information from generalized constant propagation analysis
      if (!aop->valinfo.anything &&
        ((aop->valinfo.knownbitsmask >> (offset * 8)) & 0xff) == 0xff &&
        ((aop->valinfo.knownbits >> (offset * 8)) & 0xff) == b)
        continue;

      if (aop->size <= offset)
        return (false);
      if (aop->type != AOP_LIT)
        return (false);
      if (byteOfVal (aop->aopu.aop_lit, offset) != b)
        return (false);
    }

  return (true);
}

/* Stack offset of a byte as the instruction wants it: relative to the current SP. */
static int
stkOffset (const asmop *aop, int offset)
{
  int stk;

  wassert_bt (aop->type == AOP_STK);
  stk = (offset < 8 ? aop->aopu.bytes[offset].byteu.stk : aop->aopu.bytes[0].byteu.stk + offset) + G.stack.pushed;
  wassertl_bt (regalloc_dry_run || stk >= 0, "Stack offset below SP");
  return (stk);
}

/* Operand text for n(sp); the caller has checked that the byte is in reach
   (stkIsFar). */
static const char *
stkArg (const asmop *aop, int offset)
{
  static char buffer[32];
  int stk = stkOffset (aop, offset);
  wassertl_bt (regalloc_dry_run || stk <= 511, "Far stack byte in n(sp)");
  SNPRINTF (buffer, sizeof (buffer), "%d(sp)", stk);
  return (buffer);
}

/* Far stack bytes.  n(sp) reaches 511 bytes above SP; a byte further away
   is addressed through IX, pointed at a base near it with spix and a chain
   of adx (A untouched, SP untouched - an interrupt can hit at any point).
   The IX tracker (AOP_STL, offset relative to the SP at function entry)
   remembers the base, so neighbouring bytes reuse it. */
#define FAR_STK_REM 256         /* the base sits this far below the first byte asked for */

/* entry-SP-relative offset of a stack byte */
static int
stkEntryOffset (const asmop *aop, int offset)
{
  return (offset < 8 ? aop->aopu.bytes[offset].byteu.stk : aop->aopu.bytes[0].byteu.stk + offset);
}

static bool
stkIsFar (const asmop *aop, int offset)
{
  return (aop->type == AOP_STK && stkEntryOffset (aop, offset) + G.stack.pushed > 511);
}

static void ixLoadStackAddr (int stk_off);
static void ixInvalidate (void);
static void ixLoadPtr (const asmop *aop);
static bool ixLoadPtrClobbersA (const asmop *aop);

/* The tracked base reaches the byte: its rem(ix) offset, else -1. */
static int
farStkTracked (const asmop *aop, int offset)
{
  int rem = stkEntryOffset (aop, offset) - G.ix.offset;
  if (G.ix.type == AOP_STL && rem >= 0 && rem <= 511)
    return (rem);
  return (-1);
}

/* Point IX at a base for the byte (clobbers IX, tracked) and return the
   rem(ix) text - the memory operand form, like memArg for data memory. */
static const char *
farStkArg (const asmop *aop, int offset)
{
  static char buffer[32];
  int rem = farStkTracked (aop, offset);
  if (rem < 0)
    {
      ixLoadStackAddr (stkEntryOffset (aop, offset) - FAR_STK_REM);
      rem = FAR_STK_REM;
    }
  SNPRINTF (buffer, sizeof (buffer), "%d(ix)", rem);
  return (buffer);
}

/* ldax / stax of a far byte with IX preserved: through the tracked base
   when it reaches, else around a saved IX. */
static void
farStkAccess (const char *op, const asmop *aop, int offset)
{
  int rem = farStkTracked (aop, offset);
  if (rem >= 0)
    {
      emit2 (op, "%d(ix)", rem);
      cost (1, 1);
      return;
    }
  emit2 ("push", "ix");
  cost (1, 2);
  G.stack.pushed += 2;
  {
    int n = stkEntryOffset (aop, offset) + G.stack.pushed;
    emit2 ("spix", "");
    cost (1, 1);
    while (n > 511)
      {
        emit2 ("adx", "#511");
        cost (1, 1);
        n -= 511;
      }
    emit2 ("adx", "#%d", n);
    emit2 (op, "0(ix)");
    cost (2, 2);
  }
  emit2 ("pop", "ix");
  cost (1, 2);
  G.stack.pushed -= 2;
}

/* Operand text for a direct data / peripheral address. */
static const char *
dirArg (const asmop *aop, int offset)
{
  static char buffer[256];
  wassert_bt (aop->type == AOP_DIR || aop->type == AOP_SFR);
  if (aop->aopu.immd_off + offset)
    SNPRINTF (buffer, sizeof (buffer), "%s+%d", aop->aopu.immd, aop->aopu.immd_off + offset);
  else
    SNPRINTF (buffer, sizeof (buffer), "%s", aop->aopu.immd);
  return (buffer);
}

/* A byte of a literal operand; beyond its size byteOfVal gives the sign
   extension (a narrow signed literal added to a wider operand). */
static unsigned char
litByte (const asmop *aop, int offset)
{
  wassert_bt (aop->type == AOP_LIT);
  return (byteOfVal (aop->aopu.aop_lit, offset));
}

/* Immediate byte text: literal, or a byte of a symbol address. */
static const char *
immArg (const asmop *aop, int offset)
{
  static char buffer[256];

  if (aop->type == AOP_LIT)
    {
      SNPRINTF (buffer, sizeof (buffer), "#0x%02x", litByte (aop, offset));
      return (buffer);
    }
  if (offset >= aop->size)
    return ("#0x00");
  if (aop->type == AOP_LIT)
    {
      SNPRINTF (buffer, sizeof (buffer), "#0x%02x", byteOfVal (aop->aopu.aop_lit, offset));
      return (buffer);
    }
  wassert_bt (aop->type == AOP_IMMD);
  /* code space objects are two words per byte: a byte offset o is 4*o in the
     assembler's byte-counted code addresses */
  int off = aop->aopu.code && !aop->aopu.func ? 4 * aop->aopu.immd_off : aop->aopu.immd_off;
  if (offset == 0)
    SNPRINTF (buffer, sizeof (buffer), "#<(%s + %d)", aop->aopu.immd, off);
  else if (offset == 1)
    SNPRINTF (buffer, sizeof (buffer), "#>(%s + %d)", aop->aopu.immd, off);
  else
    SNPRINTF (buffer, sizeof (buffer), "#0x00");
  return (buffer);
}

/*-----------------------------------------------------------------*/
/* newAsmop - creates a new asmOp                                  */
/*-----------------------------------------------------------------*/
static asmop *
newAsmop (short type)
{
  asmop *aop;

  aop = Safe_calloc (1, sizeof (asmop));
  aop->type = type;
  aop->valinfo.anything = true;

  return (aop);
}

/*-----------------------------------------------------------------*/
/* freeAsmop - free up the asmop given to an operand               */
/*----------------------------------------------------------------*/
static void
freeAsmop (operand *op)
{
  asmop *aop;

  wassert_bt (op);

  aop = op->aop;

  if (!aop)
    return;

  Safe_free (aop);

  op->aop = 0;
  if (IS_SYMOP (op) && SPIL_LOC (op))
    SPIL_LOC (op)->aop = 0;
}

/*-----------------------------------------------------------------*/
/* aopForSym - for a true symbol                                   */
/*-----------------------------------------------------------------*/
static asmop *
aopForSym (const iCode *ic, symbol *sym)
{
  asmop *aop;

  wassert_bt (ic);
  wassert_bt (regalloc_dry_run || sym);
  wassert_bt (regalloc_dry_run || sym->etype);

  // Unlike some other backends we really free asmops; to avoid a double-free, we need to support multiple asmops for the same symbol.

  if (sym && IS_FUNC (sym->type))
    {
      aop = newAsmop (AOP_IMMD);
      aop->aopu.immd = sym->rname;
      aop->aopu.immd_off = 0;
      aop->aopu.code = true;
      aop->aopu.func = true;
      aop->size = getSize (sym->type);
    }
  /* Assign depending on the storage class */
  else if (sym && sym->onStack || sym && sym->iaccess)
    {
      aop = newAsmop (AOP_STK);
      aop->vol = IS_VOLATILE (sym->type) || IS_VOLATILE (sym->etype);
      aop->size = getSize (sym->type);
      int base = sym->stack >= 0 ? sym->stack + G.stack.param_offset : sym->stack + 1 + G.stack.locals_base;
      for (int offset = 0; offset < aop->size && offset < 8; offset++)
        aop->aopu.bytes[offset].byteu.stk = base + offset;
    }
  /* sfr */
  else if (sym && IN_REGSP (SPEC_OCLS (sym->etype)))
    {
      aop = newAsmop (AOP_SFR);
      aop->aopu.immd = sym->rname;
      aop->aopu.immd_off = 0;
      aop->size = getSize (sym->type);
    }
  else
    {
      aop = newAsmop (sym && IN_CODESPACE (SPEC_OCLS (sym->etype)) ? AOP_CODE : AOP_DIR);
      if (sym)
        {
          aop->vol = IS_VOLATILE (sym->type) || IS_VOLATILE (sym->etype);
          aop->aopu.immd = sym->rname;
          aop->aopu.immd_off = 0;
          aop->aopu.code = (aop->type == AOP_CODE);
          aop->size = getSize (sym->type);
          /* lda/sta reach 0..0x1ff; FDATA (big objects, __xdata) and an
             __at object beyond that go through IX */
          aop->aopu.far = (aop->type == AOP_DIR &&
                           (IN_FARSPACE (SPEC_OCLS (sym->etype)) ||
                            SPEC_ABSA (sym->etype) && SPEC_ADDR (sym->etype) + aop->size > 0x200));
        }
    }

  return (aop);
}

/*-----------------------------------------------------------------*/
/* aopForRemat - rematerializes an object                          */
/*-----------------------------------------------------------------*/
static asmop *
aopForRemat (symbol *sym)
{
  iCode *ic = sym->rematiCode;
  asmop *aop;
  int val = 0;

  wassert_bt (ic);

  for (;;)
    {
      if (ic->op == '+')
        {
          if (isOperandLiteral (IC_RIGHT (ic)))
            {
              val += (int) operandLitValue (IC_RIGHT (ic));
              ic = OP_SYMBOL (IC_LEFT (ic))->rematiCode;
            }
          else
            {
              val += (int) operandLitValue (IC_LEFT (ic));
              ic = OP_SYMBOL (IC_RIGHT (ic))->rematiCode;
            }
        }
      else if (ic->op == '-')
        {
          val -= (int) operandLitValue (IC_RIGHT (ic));
          ic = OP_SYMBOL (IC_LEFT (ic))->rematiCode;
        }
      else if (ic->op == CAST)
        {
          ic = OP_SYMBOL (IC_RIGHT (ic))->rematiCode;
        }
      else if (ic->op == ADDRESS_OF)
        {
          val += (int) operandLitValue (IC_RIGHT (ic));
          break;
        }
      else
        wassert_bt (0);
    }

  if (OP_SYMBOL (IC_LEFT (ic))->onStack)
    {
      aop = newAsmop (AOP_STL);
      symbol *ssym = OP_SYMBOL (IC_LEFT (ic));
      aop->aopu.stk_off = (ssym->stack >= 0 ? ssym->stack + G.stack.param_offset : ssym->stack + 1 + G.stack.locals_base) + val;
    }
  else
    {
      aop = newAsmop (AOP_IMMD);
      aop->aopu.immd = OP_SYMBOL (IC_LEFT (ic))->rname;
      aop->aopu.immd_off = val;
      aop->aopu.code = IN_CODESPACE (SPEC_OCLS (OP_SYMBOL (IC_LEFT (ic))->etype));
      aop->aopu.func = IS_FUNC (OP_SYMBOL (IC_LEFT (ic))->type);
      /* the address of FDATA or of an __at object beyond the direct range */
      aop->aopu.far = !aop->aopu.code &&
                      (IN_FARSPACE (SPEC_OCLS (OP_SYMBOL (IC_LEFT (ic))->etype)) ||
                       SPEC_ABSA (OP_SYMBOL (IC_LEFT (ic))->etype) && SPEC_ADDR (OP_SYMBOL (IC_LEFT (ic))->etype) + val >= 0x200);
    }

  aop->size = getSize (sym->type);

  return aop;
}

/*-----------------------------------------------------------------*/
/* aopForRetSlot - the return slot (entry-SP-relative 1..ret_size,  */
/* reserved by the caller above the saved RA) as the asmop of a    */
/* temporary that exists only to be returned (lisaRetSlotTemp).    */
/* NULL otherwise.                                                 */
/*-----------------------------------------------------------------*/
static asmop *
aopForRetSlot (const symbol *sym)
{
  /* the same test the allocator made when it gave the temporary no spill
     location (ralloc.c); a byte the allocator put in A is ignored, both
     accesses go to the slot */
  if (!G.stack.ret_size || !lisaRetSlotTemp (sym) || getSize (sym->type) != G.stack.ret_size)
    return (NULL);

  asmop *aop = newAsmop (AOP_STK);
  aop->size = G.stack.ret_size;
  for (int i = 0; i < aop->size && i < 8; i++)
    aop->aopu.bytes[i].byteu.stk = 1 + i;
  return (aop);
}

/*-----------------------------------------------------------------*/
/* aopOp - allocates an asmop for an operand  :                    */
/*-----------------------------------------------------------------*/
static void
aopOp (operand *op, const iCode *ic)
{
  wassert_bt (op);

  /* if already has an asmop */
  if (op->aop)
    return;

  /* if this a literal */
  if (IS_OP_LITERAL (op))
    {
      asmop *aop = newAsmop (AOP_LIT);
      aop->aopu.aop_lit = OP_VALUE (op);
      aop->size = getSize (operandType (op));
      aop->valinfo = getOperandValinfo (ic, op);
      op->aop = aop;
      return;
    }

  symbol *sym = OP_SYMBOL (op);

  /* if this is a true symbol */
  if (IS_TRUE_SYMOP (op))
    {
      op->aop = aopForSym (ic, sym);
      op->aop->valinfo = getOperandValinfo (ic, op);
      return;
    }

  /* Rematerialize symbols where all bytes are spilt. */
  if (sym->remat && (sym->isspilt || regalloc_dry_run))
    {
      bool completely_spilt = TRUE;
      for (int i = 0; i < getSize (sym->type); i++)
        if (sym->regs[i])
          completely_spilt = FALSE;
      if (completely_spilt)
        {
          op->aop = aopForRemat (sym);
          op->aop->valinfo = getOperandValinfo (ic, op);
          return;
        }
    }

  /* if the type is a conditional */
  if (sym->regType == REG_CND)
    {
      asmop *aop = newAsmop (AOP_CND);
      op->aop = aop;
      sym->aop = sym->aop;
      return;
    }

  /* A temporary made only to be returned is the return slot itself: the
     iCode defining it writes there and genReturn has nothing to copy. */
  asmop *slot = aopForRetSlot (sym);
  if (slot)
    {
      op->aop = slot;
      op->aop->valinfo = getOperandValinfo (ic, op);
      return;
    }

  /* Spilt temporaries live in their spill location. */
  if ((sym->isspilt || sym->nRegs == 0) && sym->usl.spillLoc)
    {
      sym->aop = op->aop = aopForSym (ic, sym->usl.spillLoc);
      op->aop->size = getSize (sym->type);
      op->aop->valinfo = getOperandValinfo (ic, op);
      return;
    }

  /* The rest: temporaries in A, or dummies. */
  {
    asmop *aop = newAsmop (AOP_REG);
    aop->size = getSize (operandType (op));
    op->aop = aop;
    aop->valinfo = getOperandValinfo (ic, op);
    for (int i = 0; i < aop->size; i++)
      {
        aop->aopu.bytes[i].in_reg = !!sym->regs[i];
        if (sym->regs[i])
          aop->aopu.bytes[i].byteu.reg = sym->regs[i];
        else if (regalloc_dry_run && sym->nRegs)
          {
            /* not (yet) in a register: it will get a stack slot */
            aop->type = AOP_STK;
            for (int j = 0; j < aop->size && j < 8; j++)
              {
                aop->aopu.bytes[j].in_reg = false;
                aop->aopu.bytes[j].byteu.stk = 1 + j;
              }
            return;
          }
        else
          {
            aop->type = AOP_DUMMY;
            return;
          }
      }
  }
}

/*-----------------------------------------------------------------*/
/* The IX tracker                                                  */
/*-----------------------------------------------------------------*/

/* IX <- #sym + off (for a DIR symbol byte at offset). */
static void
ixLoadSym (const asmop *aop, int offset)
{
  wassert_bt (aop->type == AOP_DIR || aop->type == AOP_IMMD || aop->type == AOP_CODE);
  /* constant data in code space: 4 bytes (two words) per byte */
  int off = (aop->aopu.code && !aop->aopu.func) ? 4 * (aop->aopu.immd_off + offset) : aop->aopu.immd_off + offset;
  if (G.ix.type == AOP_IMMD && G.ix.base && !strcmp (G.ix.base, aop->aopu.immd) && G.ix.offset == off)
    return;
  if (off)
    emit2 ("ldx", "#(%s + %d)", aop->aopu.immd, off);
  else
    emit2 ("ldx", "#%s", aop->aopu.immd);
  cost (2, 2);
  G.ix.type = AOP_IMMD;
  G.ix.base = aop->aopu.immd;
  G.ix.offset = off;
}

/* IX <- SP + n, i.e. the address of a stack object (AOP_STL stk_off, entry-SP relative). Clobbers A. */
static void
ixLoadStackAddr (int stk_off)
{
  int n = stk_off + G.stack.pushed;
  if (G.ix.type == AOP_STL && G.ix.offset == stk_off)
    return;
  emit2 ("spix", "");
  cost (1, 1);
  /* adx takes -512..511; chain it for more rather than touch A */
  while (n > 511)
    {
      emit2 ("adx", "#511");
      cost (1, 1);
      n -= 511;
    }
  while (n < -512)
    {
      emit2 ("adx", "#-512");
      cost (1, 1);
      n += 512;
    }
  if (n)
    {
      emit2 ("adx", "#%d", n);
      cost (1, 1);
    }
  G.ix.type = AOP_STL;
  G.ix.offset = stk_off;
}

/*-----------------------------------------------------------------*/
/* Loads and stores through A                                      */
/*-----------------------------------------------------------------*/

/* Does A hold this byte already (the A tracker)?  Never for a volatile
   object or a peripheral register, and not known in a dry run. */
static bool
aHolds (const asmop *aop, int offset)
{
  /* not in a dry run, not under a predicate (the load must be there to
     be skipped), not a volatile object or a peripheral register */
  if (regalloc_dry_run || predicated > 0 || aop->vol || G.a.kind == A_UNKNOWN)
    return (false);
  if (G.a.kind == A_STK && G.a.stk + G.stack.pushed <= 0)
    return (false);
  if (offset >= aop->size && aop->type != AOP_STL && aop->type != AOP_LIT)
    return (G.a.kind == A_LIT && !strcmp (G.a.text, "#0x00"));
  switch (aop->type)
    {
    case AOP_STK:
      return (G.a.kind == A_STK && !stkIsFar (aop, offset) && G.a.stk == stkEntryOffset (aop, offset));
    case AOP_DIR:
      return (G.a.kind == A_DIR && !aop->aopu.far && !strcmp (G.a.text, dirArg (aop, offset)));
    case AOP_LIT:
    case AOP_IMMD:
      return (G.a.kind == A_LIT && !strcmp (G.a.text, immArg (aop, offset)));
    default:
      return (false);
    }
}

/* A <- aop[offset].  Note: ldi clears C, every load sets Z. Code space reads
   clobber IX and RA.  Returns true when nothing was emitted because A
   holds the byte already (then Z need not reflect it: see loadOrBytes). */
static bool
loadA (const asmop *aop, int offset)
{
  if (aHolds (aop, offset))
    return (true);
  if (offset >= aop->size && aop->type != AOP_STL && aop->type != AOP_LIT)
    {
      emit2 ("ldi", "#0x00");
      cost (1, 1);
      return (false);
    }
  switch (aop->type)
    {
    case AOP_REG:
      wassert_bt (aopInReg (aop, offset, A_IDX));
      return (true);
    case AOP_LIT:
    case AOP_IMMD:
      emit2 ("ldi", "%s", immArg (aop, offset));
      cost (1, 1);
      break;
    case AOP_STK:
      if (stkIsFar (aop, offset))
        {
          farStkAccess ("ldax", aop, offset);
          break;
        }
      emit2 ("ldax", "%s", stkArg (aop, offset));
      cost (1, 1);
      break;
    case AOP_DIR:
    case AOP_SFR:
      if (aop->aopu.far)
        {
          /* through IX, which may be in use: save it around the access */
          emit2 ("push", "ix");
          emit2 ("ldx", "#%s", aop->aopu.immd);
          emit2 ("ldax", "%d(ix)", aop->aopu.immd_off + offset);
          emit2 ("pop", "ix");
          cost (5, 7);
          break;
        }
      emit2 ("lda", "%s", dirArg (aop, offset));
      cost (1, 1);
      break;
    case AOP_STL:
      /* the address of a stack object */
      {
        int n = aop->aopu.stk_off + G.stack.pushed;
        if (offset == 0)
          {
            emit2 ("spix", "");
            emit2 ("txa", "");
            emit2 ("ldc", "#0");
            emit2 ("adc", "#0x%02x", n & 0xff);
            cost (4, 4);
          }
        else if (offset == 1)
          {
            emit2 ("spix", "");
            emit2 ("txa", "");
            emit2 ("ldc", "#0");
            emit2 ("adc", "#0x%02x", n & 0xff);
            emit2 ("txau", "");
            emit2 ("andi", "#0x7f");
            emit2 ("adc", "#0x%02x", (n >> 8) & 0xff);
            cost (7, 7);
          }
        else
          {
            emit2 ("ldi", "#0x00");
            cost (1, 1);
          }
        ixInvalidate ();
      }
      break;
    case AOP_CODE:
      ixLoadSym (aop, offset);
      emit2 ("call", "ix");
      cost (1, 4);
      ixInvalidate ();
      break;
    case AOP_DUMMY:
      break;
    default:
      wassertl_bt (0, "Unknown aop type in loadA.");
    }
  return (false);
}

static void ixNoteStore (const asmop *aop, int offset);

/* aop[offset] <- A */
static void
storeA (const asmop *aop, int offset)
{
  if (offset >= aop->size)
    return;
  /* the store may hit the bytes IX was loaded from */
  ixNoteStore (aop, offset);
  switch (aop->type)
    {
    case AOP_REG:
      wassert_bt (aopInReg (aop, offset, A_IDX));
      break;
    case AOP_STK:
      if (stkIsFar (aop, offset))
        {
          farStkAccess ("stax", aop, offset);
          break;
        }
      emit2 ("stax", "%s", stkArg (aop, offset));
      cost (1, 1);
      break;
    case AOP_DIR:
    case AOP_SFR:
      if (aop->aopu.far)
        {
          emit2 ("push", "ix");
          emit2 ("ldx", "#%s", aop->aopu.immd);
          emit2 ("stax", "%d(ix)", aop->aopu.immd_off + offset);
          emit2 ("pop", "ix");
          cost (5, 7);
          break;
        }
      emit2 ("sta", "%s", dirArg (aop, offset));
      cost (1, 1);
      break;
    case AOP_DUMMY:
      break;
    default:
      wassertl_bt (0, "Invalid aop type in storeA.");
    }
}

/* Can this operand byte be used as the memory operand of an ALU instruction
   (add/sub/cmp/and/or/xor/mul/swap/inx/dcx)?  Stack: yes; data memory: through IX. */
static bool
aopIsMem (const asmop *aop, int offset)
{
  return (offset < aop->size && (aop->type == AOP_STK || aop->type == AOP_DIR));
}

/* Memory operand text for an ALU instruction; loads IX for data memory. */
static const char *
memArg (const asmop *aop, int offset)
{
  static char buffer[32];
  if (aop->type == AOP_STK)
    return (stkIsFar (aop, offset) ? farStkArg (aop, offset) : stkArg (aop, offset));
  wassert_bt (aop->type == AOP_DIR);
  ixLoadSym (aop, 0);
  SNPRINTF (buffer, sizeof (buffer), "%d(ix)", offset);
  return (buffer);
}

/* Load IX for a data-memory operand ahead of a predicated instruction, so
   the ldx is not emitted inside the predicated pair (ldx leaves Z and C). */
static void
prepareMem (const asmop *aop, int offset)
{
  if (aop->type == AOP_DIR && offset < aop->size)
    ixLoadSym (aop, 0);
  else if (offset < aop->size && stkIsFar (aop, offset))
    farStkArg (aop, offset);
}

static bool
aluCommutative (const char *op)
{
  return (!strcmp (op, "add") || !strcmp (op, "and") || !strcmp (op, "or") || !strcmp (op, "xor") ||
          !strcmp (op, "mul") || !strcmp (op, "mulu"));
}

/* A <- A op aop[offset] for op in add/sub/and/or/xor/cmp (memory forms).
   Literal operands: add -> adc #, sub -> adc #~ (caller sets the carry
   convention), and -> andi, or -> andi/adc trick, cmp -> cpi.  Operands
   that are neither go through the stack. */
static void
emitAluA (const char *op, const asmop *aop, int offset)
{
  if (aopIsMem (aop, offset))
    {
      emit2 (op, "%s", memArg (aop, offset));
      cost (1, 1);
      return;
    }
  if (aop->type == AOP_LIT || aop->type == AOP_IMMD || offset >= aop->size)
    {
      const char *imm = immArg (aop, offset);
      if (!strcmp (op, "add"))
        emit2 ("adc", "%s", imm);
      else if (!strcmp (op, "sub"))
        {
          /* A - imm - borrow == A + ~imm + !borrow; the chain keeps C = !borrow */
          unsigned char b = (aop->type == AOP_LIT) ? litByte (aop, offset) : 0;
          if (aop->type == AOP_LIT || offset >= aop->size)
            emit2 ("adc", "#0x%02x", (~b) & 0xff);
          else
            emit2 ("adc", "#(~%s)", imm + 1);
        }
      else if (!strcmp (op, "cmp"))
        emit2 ("cpi", "%s", imm);
      else if (!strcmp (op, "and"))
        emit2 ("andi", "%s", imm);
      else if (!strcmp (op, "or") && aop->type == AOP_LIT)
        {
          /* A | k == (A & ~k) + k */
          unsigned char b = litByte (aop, offset);
          emit2 ("andi", "#0x%02x", (~b) & 0xff);
          emit2 ("ldc", "#0");
          emit2 ("adc", "#0x%02x", b);
          cost (2, 2);
        }
      else
        {
          /* xor with a literal, or with a symbol address byte, a multiply
             by a literal: all commutative, so A goes onto the stack and
             the literal into A.  (No swap n(sp): on the TT07 silicon it
             addresses sp + n - 512, see lisa_isa.md.) */
          wassertl_bt (aluCommutative (op), "non-commutative ALU op with an immediate operand");
          emit2 ("push", "a");
          G.stack.pushed++;
          emit2 ("ldi", "%s", imm);
          emit2 (op, "1(sp)");
          cost (3, 3);
          adjustStack (1);
        }
      cost (1, 1);
      return;
    }
  /* a byte in A cannot be an operand of itself: the generators order the
     loads so that A is read first, or park it on the stack */
  wassertl_bt (aop->type != AOP_REG, "A-resident operand as the memory operand of an ALU instruction");
  /* SFR, CODE, STL: A onto the stack, the byte into A - and for sub / cmp
     (which their callers only use here unpredicated) the byte onto the
     stack too and the old A back */
  emit2 ("push", "a");
  G.stack.pushed++;
  cost (1, 1);
  loadA (aop, offset);
  if (aluCommutative (op))
    {
      emit2 (op, "1(sp)");
      cost (1, 1);
      adjustStack (1);
      return;
    }
  emit2 ("push", "a");
  G.stack.pushed++;
  emit2 ("ldax", "2(sp)");
  emit2 (op, "1(sp)");
  cost (3, 3);
  adjustStack (2);
}

/*-----------------------------------------------------------------*/
/* cheapMove - Copy a byte                                         */
/*-----------------------------------------------------------------*/
static void
cheapMove (const asmop *result, int roffset, const asmop *source, int soffset)
{
  if (aopSame (result, roffset, source, soffset, 1))
    return;
  if (result->type == AOP_DUMMY)
    return;
  loadA (source, soffset);
  storeA (result, roffset);
}

/*-----------------------------------------------------------------*/
/* genMove_o - Copy part of one asmop to another                   */
/*-----------------------------------------------------------------*/
static void
genMove_o (asmop *result, int roffset, asmop *source, int soffset, int size)
{
  for (int i = 0; i < size; i++)
    {
      /* a byte of the result overlapping a later byte of the source is not a problem
         since we copy low to high and stack/dir objects never partially overlap */
      if (source->type == AOP_LIT && aopIsLitVal (source, soffset + i, 1, 0) && i > 0 &&
          aopIsLitVal (source, soffset + i - 1, 1, 0) && result->type != AOP_REG)
        {
          /* A is still 0 */
          storeA (result, roffset + i);
          continue;
        }
      cheapMove (result, roffset + i, source, soffset + i);
    }
}

/*-----------------------------------------------------------------*/
/* genMove - Copy the value from one asmop to another              */
/*-----------------------------------------------------------------*/
static void
genMove (asmop *result, asmop *source)
{
  genMove_o (result, 0, source, 0, result->size);
}

/*-----------------------------------------------------------------*/
/* push / pop helpers                                              */
/*-----------------------------------------------------------------*/
static void
pushA (void)
{
  emit2 ("push", "a");
  cost (1, 1);
  G.stack.pushed++;
}

static void
popA (void)
{
  emit2 ("pop", "a");
  cost (1, 1);
  G.stack.pushed--;
}

static void
adjustStack (int n)
{
  while (n)
    {
      int step = n > 511 ? 511 : (n < -512 ? -512 : n);
      emit2 ("ads", "#%d", step);
      cost (1, 1);
      G.stack.pushed -= step;
      n -= step;
    }
  if (G.ix.type == AOP_STL)
    ixInvalidate ();  /* STL tracking is relative to SP */
}

/* Does the iCode's result survive the instruction (i.e. is it worth storing)? */
static bool
resultUsed (const iCode *ic)
{
  return (IC_RESULT (ic) && (!IS_ITEMP (IC_RESULT (ic)) || OP_SYMBOL (IC_RESULT (ic))->nRegs || OP_SYMBOL (IC_RESULT (ic))->usl.spillLoc || OP_SYMBOL (IC_RESULT (ic))->isspilt));
}

static bool
isUnsignedOp (const operand *op)
{
  sym_link *t = operandType (op);
  return (IS_PTR (t) || !IS_SPEC (t) || SPEC_USIGN (t));
}

/* A <- aop[offset] with Z == (A == 0): a load sets Z, but one that
   was skipped may have had it clobbered since. */
static void
loadAZ (const asmop *aop, int offset)
{
  if (loadA (aop, offset) && !G.a.zvalid)
    {
      emit2 ("cpi", "#0x00");
      cost (1, 1);
    }
}

/* Set A to the OR of all bytes of aop (Z then tells if it is zero).  A
   byte that is in A already needs an explicit test: whatever set the
   flags last (a compare, a swap) need not have been its load. */
static void
loadOrBytes (const asmop *aop, int size)
{
  if (size <= 1)
    loadAZ (aop, 0);
  else
    loadA (aop, 0);
  for (int i = 1; i < size; i++)
    emitAluA ("or", aop, i);
}

/* The truth value of an operand into Z: like loadOrBytes, but a float
   ignores its sign bit so that -0.0 is false too. */
static void
loadTruth (const operand *op)
{
  const asmop *aop = op->aop;
  if (IS_FLOAT (operandType (op)) && aop->size == 4)
    {
      loadA (aop, 3);
      emit2 ("andi", "#0x7f");
      cost (1, 1);
      for (int i = 0; i < 3; i++)
        emitAluA ("or", aop, i);
    }
  else
    loadOrBytes (aop, aop->size);
}

/*-----------------------------------------------------------------*/
/* Function entry / exit                                           */
/*-----------------------------------------------------------------*/

/* Does the body of this function clobber RA?  jal (calls, helpers) and
   call ix (code space reads) do.  Scanned up front so that the prologue
   knows whether to sra. */
static bool
functionClobbersRA (const iCode *ic)
{
  for (ic = ic->next; ic && ic->op != ENDFUNCTION; ic = ic->next)
    {
      if (ic->op == CALL || ic->op == PCALL)
        return (true);
      /* division: a library call, or the hardware divider, which leaves
         its result in RA */
      if (ic->op == '/' || ic->op == '%')
        return (true);
      if (ic->op == GET_VALUE_AT_ADDRESS || ic->op == IPUSH_VALUE_AT_ADDRESS)
        {
          sym_link *type = operandType (IC_LEFT (ic));
          int ptype = (IS_PTR (type) && !IS_FUNC (type->next)) ? DCL_TYPE (type) : PTR_TYPE (SPEC_OCLS (getSpec (type)));
          if (ptype == CPOINTER || ptype == GPOINTER)
            return (true);
        }
      if (ic->op == SET_VALUE_AT_ADDRESS)
        {
          sym_link *type = operandType (IC_LEFT (ic));
          int ptype = (IS_PTR (type) && !IS_FUNC (type->next)) ? DCL_TYPE (type) : PTR_TYPE (SPEC_OCLS (getSpec (type)));
          if (ptype == GPOINTER)
            return (true);
        }
      /* a true symbol in code space is read with call ix */
      if (IC_LEFT (ic) && IS_SYMOP (IC_LEFT (ic)) && IS_TRUE_SYMOP (IC_LEFT (ic)) && !IS_FUNC (operandType (IC_LEFT (ic))) &&
          ic->op != ADDRESS_OF && IN_CODESPACE (SPEC_OCLS (OP_SYMBOL (IC_LEFT (ic))->etype)))
        return (true);
      if (IC_RIGHT (ic) && IS_SYMOP (IC_RIGHT (ic)) && IS_TRUE_SYMOP (IC_RIGHT (ic)) && !IS_FUNC (operandType (IC_RIGHT (ic))) &&
          IN_CODESPACE (SPEC_OCLS (OP_SYMBOL (IC_RIGHT (ic))->etype)))
        return (true);
      if (ic->op == IFX && IC_COND (ic) && IS_SYMOP (IC_COND (ic)) && IS_TRUE_SYMOP (IC_COND (ic)) &&
          IN_CODESPACE (SPEC_OCLS (OP_SYMBOL (IC_COND (ic))->etype)))
        return (true);
      if (ic->op == JUMPTABLE)
        return (true);
      if (ic->op == INLINEASM)
        return (true);
    }
  return (false);
}

/* The frame of a function as the prologue leaves it (G.stack, G.ra_saved). */
static void
setupFrame (const iCode *ic)
{
  const symbol *sym = OP_SYMBOL_CONST (IC_LEFT (ic));
  sym_link *ftype = operandType (IC_LEFT (ic));
  int retsize = getSize (ftype->next);

  G.stack.pushed = 0;
  G.stack.size = sym->stack;
  G.stack.ret_ix = lisaRetInIX (ftype);
  G.stack.ret_size = (retsize > 1 || IS_STRUCT (ftype->next)) && !G.stack.ret_ix ? retsize : 0;
  G.stack.param_offset = 1 + G.stack.ret_size;
  G.stack.locals_base = 0;
  G.ra_saved = false;
  ixInvalidate ();
  aInvalidate ();
  if (IFFUNC_ISNAKED (ftype))
    return;
  G.ra_saved = functionClobbersRA (ic) || IFFUNC_ISISR (ftype);
  G.stack.pushed = IFFUNC_ISISR (ftype) ? 6 : G.ra_saved ? 2 : 0;
  G.stack.locals_base = -G.stack.pushed;
  G.stack.pushed += sym->stack;
}

/* The register allocator's dry runs happen before the prologue is
   generated: give them the frame of the function the iCodes belong to. */
void
lisaDryRunInit (iCode *ic)
{
  for (; ic && ic->op != FUNCTION; ic = ic->next);
  if (ic)
    setupFrame (ic);
}

static void
genFunction (iCode *ic)
{
  const symbol *sym = OP_SYMBOL_CONST (IC_LEFT (ic));
  sym_link *ftype = operandType (IC_LEFT (ic));

  setupFrame (ic);
  /* the pushes below count up to what setupFrame computed */
  G.stack.pushed = 0;
  G.stack.locals_base = 0;
  predicated = 0;

  /* create the function header */
  emit2 (";", "---------------------------------");
  emit2 (";", " Function %s", sym->name);
  emit2 (";", "---------------------------------");

  emit2 ("", "%s:", sym->rname);
  if (!regalloc_dry_run)
    genLine.lineCurr->isLabel = 1;
  if (IFFUNC_ISNAKED (ftype))
    return;

  /* An interrupt handler saves what the hardware does not: A, IX, RA (the
     vector jal leaves it alone, isr_jump) and cflag_save, the shadow that
     every savec ... restc window relies on - read through restc / ldac c
     (the live flags are shadowed by the hardware and restored by rets).
     The save shadow's signed inversion makes the round trip through the
     ISR shadow by itself; the live signed_inversion is lost on TT07. */
  if (IFFUNC_ISISR (ftype))
    {
      emit2 ("push", "a");
      emit2 ("push", "ix");
      emit2 ("sra", "");
      emit2 ("restc", "");
      emit2 ("ldac", "c");
      emit2 ("push", "a");
      cost (6, 9);
      G.stack.pushed += 6;
    }
  else if (G.ra_saved)
    {
      emit2 ("sra", "");
      cost (1, 2);
      G.stack.pushed += 2;
    }
  G.stack.locals_base = -G.stack.pushed;

  if (sym->stack)
    adjustStack (-sym->stack);
  wassert_bt (G.stack.pushed == sym->stack + (IFFUNC_ISISR (ftype) ? 6 : G.ra_saved ? 2 : 0));
}

static void
genEndFunction (iCode *ic)
{
  symbol *sym = OP_SYMBOL (IC_LEFT (ic));
  sym_link *ftype = operandType (IC_LEFT (ic));

  D (emit2 ("; genEndFunction", ""));

  if (IFFUNC_ISNAKED (ftype))
    {
      D (emit2 (";", "naked function: no epilogue."));
      if (options.debug && currFunc && !regalloc_dry_run)
        debugFile->writeEndFunction (currFunc, ic, 0);
      return;
    }

  if (sym->stack)
    adjustStack (sym->stack);

  if (IFFUNC_ISISR (ftype))
    {
      /* cflag_save back (shr puts bit 0 in C), then RA, IX, A; rets restores
         the live flags and re-enables interrupts */
      emit2 ("pop", "a");
      emit2 ("shr", "");
      emit2 ("savec", "");
      emit2 ("lra", "");
      emit2 ("pop", "ix");
      emit2 ("pop", "a");
      emit2 ("rets", "");
      cost (7, 11);
      G.stack.pushed -= 6;
    }
  else
    {
      if (G.ra_saved)
        {
          emit2 ("lra", "");
          cost (1, 2);
          G.stack.pushed -= 2;
        }
      emit2 ("ret", "");
      cost (1, 2);
    }

  if (options.debug && currFunc && !regalloc_dry_run)
    debugFile->writeEndFunction (currFunc, ic, 1);
  if (G.stack.pushed != 0)
    {
      fprintf (stderr, "%s: stack imbalance of %d bytes at end of function\n", sym->name, G.stack.pushed);
      if (!getenv ("SDCC_LISA_NOASSERT"))
        wassert_bt (0);
      G.stack.pushed = 0;
    }
}

/*-----------------------------------------------------------------*/
/* genReturn - generate code for return statement                  */
/*-----------------------------------------------------------------*/
static void
genReturn (const iCode *ic)
{
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genReturn", ""));

  if (left && G.stack.ret_ix && IS_SYMOP (left) && lisaRetIXTemp (OP_SYMBOL (left)))
    {
      /* the call right before left it in IX */
      D (emit2 (";", "the result stays in IX from the call"));
    }
  else if (left)
    {
      aopOp (left, ic);
      wassertl (currFunc, "return iCode outside of function");

      if (G.stack.ret_ix)
        {
          ixLoadValue (left->aop);
        }
      else if (!G.stack.ret_size)
        {
          loadA (left->aop, 0);
        }
      else
        {
          /* the caller reserved the return slot just above the saved RA / our frame */
          asmop slot;
          memset (&slot, 0, sizeof (slot));
          slot.type = AOP_STK;
          slot.size = G.stack.ret_size;
          for (int i = 0; i < slot.size && i < 8; i++)
            slot.aopu.bytes[i].byteu.stk = 1 + i;
          /* bytes beyond the 8 tracked ones are addressed from byte 0; a
             temporary that aopForRetSlot put in the slot is there already */
          for (int i = 0; i < G.stack.ret_size; i++)
            cheapMove (&slot, i, left->aop, i);
        }
      freeAsmop (left);
    }

  /* generate a jump to the return label if there is more code after this */
  if (!(ic->next && ic->next->op == LABEL && IC_LABEL (ic->next) == returnLabel))
    emitBranch ("br", returnLabel);
}

/*-----------------------------------------------------------------*/
/* genLabel - generates a label                                    */
/*-----------------------------------------------------------------*/
static void
genLabel (const iCode *ic)
{
  D (emit2 ("; genLabel", ""));

  /* special case never generate */
  if (IC_LABEL (ic) == entryLabel)
    return;

  emitLbl (IC_LABEL (ic));
}

/*-----------------------------------------------------------------*/
/* genGoto - generates a jump                                      */
/*-----------------------------------------------------------------*/
static void
genGoto (const iCode *ic)
{
  D (emit2 ("; genGoto", ""));

  emitBranch ("br", IC_LABEL (ic));
}

/*-----------------------------------------------------------------*/
/* genSend - the first byte parameter goes in A (lisa_reg_parm):   */
/* SDCC emits the SEND after the pushes, right before the call     */
/*-----------------------------------------------------------------*/
static void
genSend (const iCode *ic)
{
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genSend", ""));

  wassertl (ic->argreg == 1, "genSend: only the first parameter travels in A");
  aopOp (left, ic);
  loadA (left->aop, 0);
  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genReceive - the byte parameter in A, stored to its slot in the */
/* locals unless the allocator keeps it in A (or never reads it)   */
/*-----------------------------------------------------------------*/
static void
genReceive (const iCode *ic)
{
  operand *result = IC_RESULT (ic);

  D (emit2 ("; genReceive", ""));

  wassertl (ic->argreg == 1, "genReceive: only the first parameter travels in A");
  aopOp (result, ic);
  if (result->aop->type != AOP_DUMMY && !aopInReg (result->aop, 0, A_IDX))
    storeA (result->aop, 0);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genIpush - generate code for pushing an argument                */
/*-----------------------------------------------------------------*/
static void
genIpush (const iCode *ic)
{
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genIpush", ""));

  aopOp (left, ic);

  /* A holds something else that is still needed: it is parked in the low
     byte of IX (tax / txa) when the bytes to push do not go through IX,
     else pushed first and each byte swapped in underneath through IX
     (swap n(sp) is not used: on the TT07 silicon it addresses
     sp + n - 512, see lisa_isa.md) */
  bool keep = !regDead (A_IDX, ic) && !aopInReg (left->aop, 0, A_IDX);
  bool keep_ix = keep && left->aop->type != AOP_STL && left->aop->type != AOP_CODE &&
                 !(left->aop->type == AOP_DIR && left->aop->aopu.far) &&
                 !(left->aop->type == AOP_STK && stkIsFar (left->aop, left->aop->size - 1));
  typeof (G.a) a_kept = G.a;
  if (keep_ix)
    {
      emit2 ("tax", "");
      cost (1, 1);
      ixInvalidate ();
    }

  /* bytes are pushed high to low so that the low byte ends at the lowest address */
  for (int i = left->aop->size - 1; i >= 0; i--)
    {
      /* a pair of literal bytes through IX: ldxs loads the word
         byte-swapped, so that push ix (high half at the lower address)
         lands it low byte first - 3 words for 2 bytes, and A is kept.
         ldx sets ix_cond, which push ix puts out as bit 7 of that high
         half: only for a low byte that has the bit set already (so not
         for a symbol address, which the linker could tell but not fix) */
      if (i >= 1 && left->aop->type == AOP_LIT && (litByte (left->aop, i - 1) & 0x80) && !keep_ix)
        {
          emit2 ("ldxs", "#0x%02x%02x", litByte (left->aop, i), litByte (left->aop, i - 1));
          emit2 ("push", "ix");
          cost (3, 4);
          G.stack.pushed += 2;
          ixInvalidate ();
          i--;
          continue;
        }
      if (keep && !keep_ix)
        pushA ();                   /* the slot the byte is swapped into */
      if (left->aop->type == AOP_STL)
        {
          /* the address of a stack object, as seen after the pushes so far */
          int n = left->aop->aopu.stk_off + G.stack.pushed;
          emit2 ("spix", "");
          emit2 ("txa", "");
          emit2 ("ldc", "#0");
          emit2 ("adc", "#0x%02x", n & 0xff);
          cost (4, 4);
          if (i == 1)
            {
              emit2 ("txau", "");
              emit2 ("andi", "#0x7f");
              emit2 ("adc", "#0x%02x", (n >> 8) & 0xff);
              cost (3, 3);
            }
          ixInvalidate ();
        }
      else
        loadA (left->aop, i);
      if (keep && !keep_ix)
        {
          emit2 ("spix", "");
          emit2 ("adx", "#1");
          emit2 ("swap", "0(ix)");
          cost (3, 3);
          ixInvalidate ();
        }
      else
        pushA ();
    }
  if (keep_ix)
    {
      emit2 ("txa", "");
      cost (1, 1);
      G.a = a_kept;             /* the same byte is back in A */
    }

  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genCall - generates a call statement                            */
/*-----------------------------------------------------------------*/
static void
genCall (const iCode *ic)
{
  sym_link *dtype = operandType (IC_LEFT (ic));
  sym_link *ftype = IS_FUNCPTR (dtype) ? dtype->next : dtype;
  operand *left = IC_LEFT (ic);
  int retsize = getSize (ftype->next);
  const bool ixreturn = lisaRetInIX (ftype);
  const bool bigreturn = ((retsize > 1) || IS_STRUCT (ftype->next)) && !ixreturn;
  const bool SomethingReturned = (IS_ITEMP (IC_RESULT (ic)) &&
                       (OP_SYMBOL (IC_RESULT (ic))->nRegs || OP_SYMBOL (IC_RESULT (ic))->spildir || OP_SYMBOL (IC_RESULT (ic))->usl.spillLoc))
                       || IS_TRUE_SYMOP (IC_RESULT (ic));

  D (emit2 ("; genCall", ""));
  D (emit2 (";", "parmBytes %d", ic->parmBytes));

  aopOp (left, ic);
  if (SomethingReturned)
    aopOp (IC_RESULT (ic), ic);

  if (bigreturn)
    adjustStack (-retsize);

  if (ic->op == PCALL)
    {
      /* function pointer: into IX */
      if (left->aop->type == AOP_IMMD)
        {
          emit2 ("ldx", "#%s", left->aop->aopu.immd);
          cost (2, 2);
        }
      else if (left->aop->type == AOP_STK && !stkIsFar (left->aop, 0) && !stkIsFar (left->aop, 1))
        {
          emit2 ("ldxx", "%s", stkArg (left->aop, 0));
          cost (1, 2);
        }
      else
        {
          /* the first parameter may already be in A (genSend) */
          bool a_parm = FUNC_ARGS (ftype) && IS_REGPARM (FUNC_ARGS (ftype)->etype) && !IFFUNC_HASVARARGS (ftype);
          if (a_parm)
            pushA ();
          loadA (left->aop, 0);
          ixInvalidate ();
          emit2 ("tax", "");
          loadA (left->aop, 1);
          emit2 ("taxu", "");
          cost (2, 2);
          if (a_parm)
            popA ();
        }
      emit2 ("call", "ix");
      cost (1, 3);
    }
  else if (IS_OP_LITERAL (left))
    {
      /* a call through a constant: the word address in IX */
      emit2 ("ldx", "#0x%04x", (unsigned) operandLitValue (left) & 0x7fff);
      emit2 ("call", "ix");
      cost (3, 5);
    }
  else
    {
      emit2 ("jal", "%s", OP_SYMBOL (left)->rname);
      cost (1, 3);
    }
  ixInvalidate ();

  freeAsmop (left);

  /* the return value */
  if (ixreturn)
    aInvalidate ();
  if (SomethingReturned)
    {
      if (ixreturn)
        {
          /* the result in IX: stxx into its slot (IX then still holds
             it, a deref right after needs no ldxx), or through A */
          asmop *raop = IC_RESULT (ic)->aop;
          if (raop->type == AOP_DUMMY)
            ;
          else if (raop->type == AOP_STK && !stkIsFar (raop, 0) && !stkIsFar (raop, 1))
            {
              emit2 ("stxx", "%s", stkArg (raop, 0));
              cost (1, 2);
              G.ix.type = AOP_STK;
              G.ix.offset = raop->aopu.bytes[0].byteu.stk;
            }
          else
            {
              emit2 ("txa", "");
              cost (1, 1);
              storeA (raop, 0);
              emit2 ("txau", "");
              cost (1, 1);
              storeA (raop, 1);
            }
        }
      else if (!bigreturn)
        {
          if (IC_RESULT (ic)->aop->type != AOP_DUMMY)
            storeA (IC_RESULT (ic)->aop, 0);
        }
      else
        {
          asmop slot;
          memset (&slot, 0, sizeof (slot));
          slot.type = AOP_STK;
          slot.size = retsize;
          for (int i = 0; i < retsize && i < 8; i++)
            slot.aopu.bytes[i].byteu.stk = 1 - G.stack.pushed + i;   /* 1(sp) .. */
          /* a narrower result (the division support calls): only its bytes.
             High byte first, so that A ends up holding byte 0 for whatever
             uses the result next (the A tracker) */
          int n = min (retsize, IC_RESULT (ic)->aop->size);
          for (int i = n - 1; i >= 0; i--)
            cheapMove (IC_RESULT (ic)->aop, i, &slot, i);
        }
      freeAsmop (IC_RESULT (ic));
    }

  /* drop the return slot and the arguments */
  adjustStack ((bigreturn ? retsize : 0) + ic->parmBytes);
}

/*-----------------------------------------------------------------*/
/* genAssign - generate code for assignment                        */
/*-----------------------------------------------------------------*/
static void
genAssign (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *right = IC_RIGHT (ic);

  D (emit2 ("; genAssign", ""));

  aopOp (right, ic);
  aopOp (result, ic);

  wassert (result->aop->type != AOP_DUMMY || right->aop->type != AOP_DUMMY);

  if (right->aop->type == AOP_DUMMY)
    ;
  else if (result->aop->type == AOP_DUMMY)
    {
      /* a volatile read with the result discarded */
      for (int i = 0; i < right->aop->size; i++)
        loadA (right->aop, i);
    }
  else
    genMove (result->aop, right->aop);

  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genPlus / genMinus - byte-wise addition / subtraction           */
/*-----------------------------------------------------------------*/

/* The silicon's ALU (lisa_core.v, acc_adder is 8 bits wide):
     add  M     A <- A + M            (no carry in!), C = carry
     adc  #k    A <- A + ((k + C) & 0xff): k = 0xff with C = 1 adds 0 and loses the carry
     sub  M     A <- A - M - C, C = borrow; wrong (borrow) when M == 0 and C == 0
     cmp  M     C = borrow of A - M; wrong (borrow) when M == 0
     cpi  #k    C = (A < k) through a comparator: always right, no sign capture
   so a multi-byte add puts the incoming carry in with adc #0 (cannot
   truncate) before the add and merges the two carries, a subtraction by a
   memory operand guards the zero-operand case with cpi #1 / predication,
   and a subtraction by a literal is an addition of the complement with the
   adc #0xff case guarded. */

/* A <- A + r[i] + C, with C out, for a memory operand (C in is correct). */
static void
emitAddByteMem (const asmop *raop, int i, bool keep_carry)
{
  emit2 ("adc", "#0x00");
  cost (1, 1);
  /* the zero extension of a narrower operand: the carry in was all */
  if (i >= raop->size)
    return;
  if (keep_carry)
    {
      emit2 ("savec", "");
      cost (1, 1);
    }
  emitAluA ("add", raop, i);
  if (keep_carry)
    {
      emit2 ("if", "nc");
      emit2 ("restc", "");
      cost (2, 2);
    }
}

/* Load left byte i into A without disturbing C (ldi clears it). */
static void
loadAKeepC (const asmop *laop, int i)
{
  /* ldi clears C; the address of a stack object is computed with ldc/adc;
     code space reads return through an ldi */
  bool lit = (laop->type == AOP_LIT || laop->type == AOP_IMMD || laop->type == AOP_STL ||
              laop->type == AOP_CODE || i >= laop->size);
  if (lit)
    {
      emit2 ("savec", "");
      cost (1, 1);
    }
  loadA (laop, i);
  if (lit)
    {
      emit2 ("restc", "");
      cost (1, 1);
    }
}

/* result = left +/- right, byte-wise. */
/* A _BitInt whose width is not a multiple of 8 keeps its padding bits
   zero (unsigned) or sign-filled (signed); fix the top byte of a result
   after an operation that may have carried into them. */
static void
fixBitIntResult (const iCode *ic, bool sign_extend)
{
  operand *result = IC_RESULT (ic);
  sym_link *rtype = operandType (result);
  if (!IS_BITINT (rtype) || !(SPEC_BITINTWIDTH (rtype) % 8) || !result->aop || result->aop->type == AOP_DUMMY)
    return;
  int bits = SPEC_BITINTWIDTH (rtype) % 8;
  unsigned mask = 0xff >> (8 - bits);
  if (!SPEC_USIGN (rtype) && !sign_extend)
    return;                      /* signed overflow is undefined anyway */
  loadA (result->aop, result->aop->size - 1);
  emit2 ("andi", "#0x%02x", mask);
  cost (1, 1);
  if (!SPEC_USIGN (rtype))
    {
      /* the masked bits are zero, so adding the high mask is an or */
      emit2 ("ldc", "#0");
      emit2 ("btst", "%d", bits - 1);
      emit2 ("if", "z");
      emit2 ("adc", "#0x%02x", ~mask & 0xff);
      cost (4, 4);
    }
  storeA (result->aop, result->aop->size - 1);
}

/* A base for an indexed address: a data symbol, the address of a stack
   object or a pointer in a near stack slot. */
static bool
indexBase (const asmop *aop)
{
  if (aop->size != 2)
    return (false);
  switch (aop->type)
    {
    case AOP_IMMD:
      return (!aop->aopu.code && !aop->aopu.func);
    case AOP_STL:
      return (true);
    case AOP_STK:
      return (!stkIsFar (aop, 0) && !stkIsFar (aop, 1));
    default:
      return (false);
    }
}

/* The '+' of a[i] or p + i whose result nothing but reads and writes
   through it in data space use: IX = base + index (ldx / ldxx / spix,
   then ldax; addax, and ldax; addaxu for a two-byte index), stxx into
   the result's slot, and IX left holding it for the access that follows
   (ixLoadPtr finds it there).  addax sets ix_cond, so the slot carries
   bit 15: harmless to ldax / stax n(ix), wrong for anything else - hence
   the test of the uses. */
static bool
genIndexedAddr (const iCode *ic, asmop *laop, asmop *raop)
{
  operand *result = IC_RESULT (ic);
  if (!IS_ITEMP (result) || result->aop->type != AOP_STK || result->aop->size != 2 ||
      stkIsFar (result->aop, 0) || stkIsFar (result->aop, 1))
    return (false);
  sym_link *rtype = operandType (result);
  if (!IS_PTR (rtype) || IS_FUNC (rtype->next) || DCL_TYPE (rtype) == CPOINTER || DCL_TYPE (rtype) == GPOINTER)
    return (false);
  const bitVect *uses = OP_USES (result);
  if (!uses || bitVectIsZero (uses))
    return (false);
  for (int i = 0; i < uses->size; i++)
    {
      if (!bitVectBitValue (uses, i))
        continue;
      const iCode *uic = hTabItemWithKey (iCodehTab, i);
      if (!uic || uic->op != GET_VALUE_AT_ADDRESS && uic->op != SET_VALUE_AT_ADDRESS ||
          !IS_SYMOP (IC_LEFT (uic)) || IC_LEFT (uic)->key != result->key ||
          uic->op == SET_VALUE_AT_ADDRESS && IS_SYMOP (IC_RIGHT (uic)) && IC_RIGHT (uic)->key == result->key)
        return (false);
    }

  asmop *base, *idx;
  if (indexBase (laop))
    base = laop, idx = raop;
  else if (indexBase (raop))
    base = raop, idx = laop;
  else
    return (false);
  if (idx->size < 1 || idx->size > 2)
    return (false);
  if (idx->type == AOP_REG)
    {
      if (ixLoadPtrClobbersA (base))
        return (false);
    }
  else if (idx->type != AOP_STK || stkIsFar (idx, 0) || stkIsFar (idx, idx->size - 1))
    return (false);
  /* a one-byte index is added zero-extended */
  const operand *iop = (idx == IC_LEFT (ic)->aop) ? IC_LEFT (ic) : IC_RIGHT (ic);
  if (idx->size == 1 && !SPEC_USIGN (getSpec (operandType (iop))))
    return (false);

  ixLoadPtr (base);
  loadA (idx, 0);
  emit2 ("addax", "");
  cost (1, 1);
  if (idx->size == 2)
    {
      loadA (idx, 1);
      emit2 ("addaxu", "");
      cost (1, 1);
    }
  emit2 ("stxx", "%s", stkArg (result->aop, 0));
  cost (1, 2);
  G.ix.type = AOP_STK;
  G.ix.offset = result->aop->aopu.bytes[0].byteu.stk;
  return (true);
}

static void
genAddSub (const iCode *ic, bool sub)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int size;

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);
  size = result->aop->size;

  asmop *laop = left->aop, *raop = right->aop;

  /* addition: literals on the right, and a byte in A on the left so that
     it is the one loaded (a no-op) first */
  if (!sub && (laop->type == AOP_LIT || laop->type == AOP_IMMD) && raop->type != AOP_LIT && raop->type != AOP_IMMD ||
      !sub && raop->type == AOP_REG && laop->type != AOP_REG)
    {
      asmop *t = laop; laop = raop; raop = t;
    }

  /* subtracting a symbol address (the linker cannot relocate ~sym), the
     address of a stack object or code space data: the bytes go onto the
     stack first and are subtracted as memory - the sub sequences are
     predicated and cannot hold the multi-instruction loads */
  asmop immd_tmp;
  int immd_pushed = 0;
  if (sub && raop->type != AOP_LIT && !aopIsMem (raop, 0) && raop->type != AOP_DUMMY)
    {
      int n = raop->type == AOP_STL ? 2 : raop->size;
      if (n > size)
        n = size;
      for (int i = n - 1; i >= 0; i--)
        {
          loadA (raop, i);
          pushA ();
        }
      memset (&immd_tmp, 0, sizeof (immd_tmp));
      immd_tmp.type = AOP_STK;
      immd_tmp.size = n;
      for (int i = 0; i < n; i++)
        immd_tmp.aopu.bytes[i].byteu.stk = 1 + i - G.stack.pushed;
      raop = &immd_tmp;
      immd_pushed = n;
    }

  bool litright = (raop->type == AOP_LIT || raop->type == AOP_IMMD);
  bool litleft = (laop->type == AOP_LIT || laop->type == AOP_IMMD);

  /* inc / dec of a memory operand by one: inx/dcx with predication.  On
     the TT07 silicon a read-modify-write that misses the data cache
     works on stale data (lisa_isa.md), so each byte's line is touched
     with a cmp first - a read waits for the cache - and the inx / dcx
     then hit (the bytes of one operand are in adjacent lines at most,
     which never share a cache index) */
  if (!litleft && aopIsMem (laop, 0) && raop->type == AOP_LIT && aopSame (result->aop, 0, laop, 0, size) &&
      (aopIsLitVal (raop, 0, size, 1)))
    {
      const char *op = sub ? "dcx" : "inx";
      if (lisa_tt07_cache)
        for (int i = size - 1; i >= 0; i--)
          {
            emit2 ("cmp", "%s", memArg (laop, i));
            cost (1, 1);
          }
      emit2 (op, "%s", memArg (laop, 0));
      cost (1, 2);
      for (int i = 1; i < size; i++)
        {
          emit2 ("if", "c");
          emit2 (op, "%s", memArg (laop, i));
          cost (2, 3);
        }
      goto release;
    }

  /* a[i]: the address formed in IX */
  if (!sub && genIndexedAddr (ic, laop, raop))
    goto release;

  for (int i = 0; i < size; i++)
    {
      bool last = (i == size - 1);

      if (!sub)
        {
          if (i == 0)
            {
              loadA (laop, 0);
              if (litright)
                {
                  if (!litleft)
                    {
                      emit2 ("ldc", "#0");
                      cost (1, 1);
                    }
                  emitAluA ("add", raop, 0);          /* adc #k, C = 0: cannot truncate */
                }
              else
                emitAluA ("add", raop, 0);            /* add M: no carry in anyway */
            }
          else
            {
              loadAKeepC (laop, i);
              if (litright)
                {
                  unsigned char k = (raop->type == AOP_LIT) ? litByte (raop, i) : 0;
                  if (k == 0xff && raop->type == AOP_LIT && !last)
                    {
                      /* k + C would be 0x100: when C is set the byte is unchanged and C stays */
                      emit2 ("if", "nc");
                      emit2 ("adc", "#0xff");
                      cost (2, 2);
                    }
                  else
                    emitAluA ("add", raop, i);
                }
              else
                emitAddByteMem (raop, i, !last);
            }
          storeA (result->aop, i);
          continue;
        }

      /* subtraction */
      if (litright)
        {
          /* A + ~k + C, C = 1 means no borrow */
          unsigned char k = (raop->type == AOP_LIT) ? litByte (raop, i) : 0;
          if (i == 0)
            {
              loadA (laop, 0);
              emit2 ("ldc", "#1");
              cost (1, 1);
            }
          else
            loadAKeepC (laop, i);
          if (raop->type != AOP_LIT)
            emitAluA ("sub", raop, i);               /* address byte: adc #(~sym) */
          else if (k == 0)
            {
              /* ~k = 0xff: with C set the byte is unchanged and C stays (the hardware would add 0 and clear C) */
              if (i != 0)
                {
                  if (!last)
                    {
                      emit2 ("if", "nc");
                      emit2 ("adc", "#0xff");
                      cost (2, 2);
                    }
                  else
                    {
                      emit2 ("adc", "#0xff");
                      cost (1, 1);
                    }
                }
            }
          else
            {
              emit2 ("adc", "#0x%02x", (~k) & 0xff);
              cost (1, 1);
            }
          storeA (result->aop, i);
          continue;
        }

      /* memory operand: sub M, C = borrow.  Wrong only when M == 0 and no borrow in. */
      if (i >= raop->size)
        {
          /* the zero extension of a narrower operand: A - borrow, with the
             borrow convention of the memory sub (C = 1 means borrow) */
          loadAKeepC (laop, i);
          if (last)
            {
              emit2 ("if", "c");
              emit2 ("adc", "#0xfe");                  /* C = 1: adds 0xff */
              cost (2, 2);
            }
          else
            {
              /* borrow out = borrow in && old A == 0 (== new A == 0xff) */
              emit2 ("savec", "");
              emit2 ("if", "c");
              emit2 ("adc", "#0xfe");
              emit2 ("cpi", "#0xff");
              emit2 ("restc", "");
              emit2 ("if", "nz");
              emit2 ("ldc", "#0");
              cost (7, 7);
            }
        }
      else if (last && i == 0)
        {
          loadA (laop, 0);
          emit2 ("ldc", "#0");
          cost (1, 1);
          emitAluA ("sub", raop, 0);                   /* value right, flag unused */
        }
      else if (last)
        {
          loadAKeepC (laop, i);
          emitAluA ("sub", raop, i);                   /* value right with the right borrow in */
        }
      else if (i == 0)
        {
          /* C <- (M == 0) through the comparator, then either sub (M != 0, C in 0) or just clear C */
          loadA (raop, 0);
          emit2 ("cpi", "#0x01");
          cost (1, 1);
          loadAKeepC (laop, 0);
          prepareMem (raop, 0);
          emit2 ("ifte", "nc");
          emitAluA ("sub", raop, 0);
          emit2 ("ldc", "#0");
          cost (2, 2);
        }
      else
        {
          /* middle byte: borrow in C.  sub is right unless M == 0 && C == 0. */
          symbol *tlbl_sub = regalloc_dry_run ? 0 : newiTempLabel (0);
          symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);
          emit2 ("savec", "");
          cost (1, 1);
          loadA (raop, i);
          emit2 ("cpi", "#0x01");
          emit2 ("if", "nc");
          cost (2, 2);
          emitBranch ("br", tlbl_sub);
          emit2 ("restc", "");
          cost (1, 1);
          loadAKeepC (laop, i);
          prepareMem (raop, i);
          emit2 ("if", "c");
          emitAluA ("sub", raop, i);                   /* M == 0: only with a borrow in (then it is right) */
          cost (1, 1);
          emitBranch ("br", tlbl_done);
          emitLbl (tlbl_sub);
          emit2 ("restc", "");
          cost (1, 1);
          loadAKeepC (laop, i);
          emitAluA ("sub", raop, i);
          emitLbl (tlbl_done);
        }
      storeA (result->aop, i);
    }

release:
  if (immd_pushed)
    adjustStack (immd_pushed);
  fixBitIntResult (ic, false);
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

static void
genPlus (const iCode *ic)
{
  D (emit2 ("; genPlus", ""));
  genAddSub (ic, false);
}

static void
genMinus (const iCode *ic)
{
  D (emit2 ("; genMinus", ""));
  genAddSub (ic, true);
}

/*-----------------------------------------------------------------*/
/* genUminus - unary minus: ~x + 1, the carry through adc #0       */
/*-----------------------------------------------------------------*/
static void
genUminus (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genUminus", ""));

  aopOp (left, ic);
  aopOp (result, ic);

  /* float: flip the sign bit */
  if (IS_FLOAT (operandType (left)))
    {
      int size = result->aop->size;
      for (int i = 0; i < size - 1; i++)
        cheapMove (result->aop, i, left->aop, i);
      loadA (left->aop, size - 1);
      /* A ^ 0x80 through the stack: xor has no immediate form */
      emit2 ("push", "a");
      emit2 ("ldi", "#0x80");
      emit2 ("xor", "1(sp)");
      emit2 ("ads", "#1");
      cost (4, 4);
      storeA (result->aop, size - 1);
      freeAsmop (left);
      freeAsmop (result);
      return;
    }

  for (int i = 0; i < result->aop->size; i++)
    {
      if (i)
        {
          emit2 ("savec", "");
          cost (1, 1);
        }
      emit2 ("ldi", "#0xff");
      cost (1, 1);
      if (i < left->aop->size)
        emitAluA ("sub", left->aop, i);            /* A = ~x (C = 0 from ldi; the flag is garbage) */
      /* else: the zero extension, ~0 is the 0xff already in A */
      if (i)
        emit2 ("restc", "");
      else
        emit2 ("ldc", "#1");
      emit2 ("adc", "#0x00");                      /* + carry: 0 + C never truncates */
      cost (2, 2);
      storeA (result->aop, i);
    }

  fixBitIntResult (ic, false);
  freeAsmop (left);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genCpl - bitwise complement                                     */
/*-----------------------------------------------------------------*/
static void
genCpl (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genCpl", ""));

  aopOp (left, ic);
  aopOp (result, ic);

  /* ~x == 0xff - x */
  for (int i = 0; i < result->aop->size; i++)
    {
      emit2 ("ldi", "#0xff");
      cost (1, 1);
      emitAluA ("sub", left->aop, i);
      storeA (result->aop, i);
    }

  freeAsmop (left);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genNot - logical not                                            */
/*-----------------------------------------------------------------*/
static void
genNot (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);

  D (emit2 ("; genNot", ""));

  aopOp (left, ic);
  aopOp (result, ic);

  loadTruth (left);
  emit2 ("ldac", "eq");
  cost (1, 1);
  storeA (result->aop, 0);
  for (int i = 1; i < result->aop->size; i++)
    {
      emit2 ("ldi", "#0x00");
      cost (1, 1);
      storeA (result->aop, i);
    }

  freeAsmop (left);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genAnd / genOr / genXor - bitwise operations                    */
/*-----------------------------------------------------------------*/
static void
genBitwise (const iCode *ic, const char *op, iCode *ifx)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int size;

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);
  size = result->aop->type == AOP_CND ? max (left->aop->size, right->aop->size) : result->aop->size;

  asmop *laop = left->aop, *raop = right->aop;
  /* literals on the right (andi), else memory on the right; a byte in A
     on the left, where loading it is a no-op */
  if (laop->type == AOP_LIT || laop->type == AOP_IMMD ||
      raop->type != AOP_LIT && raop->type != AOP_IMMD && !aopIsMem (raop, 0) && aopIsMem (laop, 0) ||
      raop->type == AOP_REG && laop->type != AOP_REG)
    {
      asmop *t = laop; laop = raop; raop = t;
    }

  /* bit test used by an if: and with a literal, result unused (or marked
     as a condition by the allocator, then it has no storage at all) */
  if (ifx && !strcmp (op, "and") && (result->aop->type == AOP_CND || !resultUsed (ic)))
    {
      /* OR together the bytes of (left & right) that can be nonzero */
      bool first = true;
      for (int i = 0; i < size; i++)
        {
          if (raop->type == AOP_LIT && aopIsLitVal (raop, i, 1, 0))
            continue;
          if (!first)
            pushA ();
          loadA (laop, i);
          emitAluA ("and", raop, i);
          if (!first)
            {
              emit2 ("or", "1(sp)");
              cost (1, 1);
              adjustStack (1);
            }
          first = false;
        }
      if (first)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
        }
      if (IC_TRUE (ifx))
        emitBranch ("bnz", IC_TRUE (ifx));
      else
        emitBranch ("bz", IC_FALSE (ifx));
      ifx->generated = 1;
      goto release;
    }

  for (int i = 0; i < size; i++)
    {
      if (raop->type == AOP_LIT)
        {
          unsigned char b = litByte (raop, i);
          if (!strcmp (op, "and") && b == 0)
            {
              emit2 ("ldi", "#0x00");
              cost (1, 1);
              storeA (result->aop, i);
              continue;
            }
          if ((!strcmp (op, "and") && b == 0xff) || (strcmp (op, "and") && b == 0))
            {
              cheapMove (result->aop, i, laop, i);
              continue;
            }
          if (!strcmp (op, "xor") && b == 0xff && laop->type != AOP_REG)
            {
              emit2 ("ldi", "#0xff");
              cost (1, 1);
              emitAluA ("sub", laop, i);
              storeA (result->aop, i);
              continue;
            }
        }
      loadA (laop, i);
      emitAluA (op, raop, i);
      storeA (result->aop, i);
    }

release:
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

static void
genAnd (const iCode *ic, iCode *ifx)
{
  D (emit2 ("; genAnd", ""));
  genBitwise (ic, "and", ifx);
}

static void
genOr (const iCode *ic)
{
  D (emit2 ("; genOr", ""));
  genBitwise (ic, "or", 0);
}

static void
genXor (const iCode *ic)
{
  D (emit2 ("; genXor", ""));
  genBitwise (ic, "xor", 0);
}

/*-----------------------------------------------------------------*/
/* Comparisons                                                     */
/*-----------------------------------------------------------------*/

/* Ordered compares on the silicon: cmp M reports a borrow for M == 0, so a
   memory operand is first tested with cpi #1 (C <- M == 0, comparator) and
   the cmp is predicated on it; cpi #k is always right but never captures
   the sign inversion, so a signed compare against a literal offsets both
   sides by 0x80 and compares unsigned.  A signed compare against a memory
   byte branches on the zero case. */

/* The '!' that consumes a compare's result and nothing else does (x >= 0
   reaches the generator as !(x < 0)): like ifxForOp, the next iCode past
   any IPOP, with the temporary dying there.  The compare then produces
   the inverted truth value straight into the !'s result. */
static iCode *
notForOp (const operand *op, const iCode *ic)
{
  if (IS_TRUE_SYMOP (op) || !IS_ITEMP (op))
    return (0);
  iCode *nic = ic->next;
  while (nic && nic->op == IPOP)
    nic = nic->next;
  if (nic && nic->op == '!' && IS_SYMOP (IC_LEFT (nic)) && IC_LEFT (nic)->key == op->key &&
      OP_SYMBOL_CONST (op)->liveFrom >= ic->seq && OP_SYMBOL_CONST (op)->liveTo <= nic->seq)
    return (nic);
  return (0);
}

/* The condition that is true exactly when cc is false (lt is C && !Z,
   ge is !C || Z, and so on), or 0 for a code that is not known. */
static const char *
condInverse (const char *cc)
{
  static const char *const pairs[][2] = {
    {"lt", "ge"}, {"gt", "le"}, {"slt", "sge"}, {"sgt", "sle"}, {"eq", "ne"}, {"z", "nz"}, {"c", "nc"},
    {"gte", "lt"}, {"lte", "gt"}, {"sgte", "slt"}, {"slte", "sgt"}, {0, 0}
  };
  for (int i = 0; pairs[i][0]; i++)
    {
      if (!strcmp (cc, pairs[i][0]))
        return (pairs[i][1]);
      if (!strcmp (cc, pairs[i][1]))
        return (pairs[i][0]);
    }
  return (0);
}

/* A <- left[i]; compare with right[i] (a literal or in memory), leaving C
   (left < right, unsigned) and Z (equal) correct.  sign: the byte is the
   top byte of a signed compare - only allowed with a literal right. */
static void
cmpByteFlags (const asmop *laop, const asmop *raop, int i, bool sign)
{
  if (raop->type == AOP_LIT || raop->type == AOP_IMMD || i >= raop->size)
    {
      loadA (laop, i);
      if (sign)
        {
          unsigned char k = (raop->type == AOP_LIT) ? litByte (raop, i) : 0;
          emit2 ("ldc", "#0");
          emit2 ("adc", "#0x80");
          emit2 ("cpi", "#0x%02x", (k ^ 0x80) & 0xff);
          cost (3, 3);
        }
      else
        {
          emit2 ("cpi", "%s", immArg (raop, i));
          cost (1, 1);
        }
      return;
    }
  wassert_bt (!sign);
  if (!aopIsMem (raop, i))
    {
      /* sfr / code: get it onto the stack first */
      loadA (raop, i);
      pushA ();
      emit2 ("cpi", "#0x01");
      cost (1, 1);
      loadA (laop, i);
      emit2 ("ifte", "nc");
      emit2 ("cmp", "1(sp)");
      emit2 ("ldc", "#0");
      cost (3, 3);
      adjustStack (1);
      return;
    }
  loadA (raop, i);
  emit2 ("cpi", "#0x01");
  cost (1, 1);
  loadA (laop, i);
  prepareMem (raop, i);
  emit2 ("ifte", "nc");
  emitAluA ("cmp", raop, i);
  emit2 ("ldc", "#0");
  cost (2, 2);
}

/* result = left < right (op '<') or left > right (op '>') */
static void
genCmp (const iCode *ic, iCode *ifx)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  bool lt = (ic->op == '<');
  bool sign;
  int size;

  D (emit2 ("; genCmp", ""));

  /* a ! on the result: produce its inverse into the !'s result instead */
  iCode *notic = ifx ? 0 : notForOp (result, ic);
  bool inv = (notic != 0);
  if (notic)
    {
      result = IC_RESULT (notic);
      notic->generated = 1;
    }

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  sign = !(isUnsignedOp (left) || isUnsignedOp (right));
  size = max (left->aop->size, right->aop->size);

  asmop *laop = left->aop, *raop = right->aop;
  /* the literal (if any) on the right: swap and flip the relation.  A byte
     in A compared with memory goes to the right, where cmpByteFlags
     pushes it before loading the left (against a literal it stays left,
     cpi reads A as it is). */
  if ((laop->type == AOP_LIT || laop->type == AOP_IMMD) && raop->type != AOP_LIT && raop->type != AOP_IMMD ||
      laop->type == AOP_REG && raop->type != AOP_LIT && raop->type != AOP_IMMD && raop->type != AOP_REG)
    {
      asmop *t = laop; laop = raop; raop = t;
      lt = !lt;
    }
  bool litright = (raop->type == AOP_LIT || raop->type == AOP_IMMD);
  const char *cc_true = lt ? "lt" : "gt";       /* after cmpByteFlags: unsigned conditions */
  const char *cc_false = lt ? "gt" : "lt";

  symbol *tlbl_true = regalloc_dry_run ? 0 : newiTempLabel (0);
  symbol *tlbl_false = regalloc_dry_run ? 0 : newiTempLabel (0);
  symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);

  /* against the literal 0: left < 0 (signed) is the sign bit of the top
     byte, and a byte can not be below 0, so the chain below only has to
     look for a non-zero byte */
  bool litzero = (raop->type == AOP_LIT);
  for (int i = 0; litzero && i < size; i++)
    if (litByte (raop, i))
      litzero = false;
  if (litzero && (sign ? lt : !lt))
    {
      const char *cc;
      if (sign)
        {
          loadA (laop, size - 1);
          emit2 ("btst", "7");          /* Z = bit 7 */
          cost (1, 1);
          cc = "eq";
        }
      else
        {
          /* unsigned left > 0: not zero */
          loadOrBytes (laop, size);
          cc = "ne";
        }
      if (ifx)
        {
          if (IC_TRUE (ifx))
            emitBranch (!strcmp (cc, "eq") ? "bz" : "bnz", IC_TRUE (ifx));
          else
            emitBranch (!strcmp (cc, "eq") ? "bnz" : "bz", IC_FALSE (ifx));
          ifx->generated = 1;
        }
      else
        {
          emit2 ("ldac", "%s", inv ? condInverse (cc) : cc);
          cost (1, 1);
          storeA (result->aop, 0);
          for (int i = 1; i < result->aop->size; i++)
            {
              emit2 ("ldi", "#0x00");
              cost (1, 1);
              storeA (result->aop, i);
            }
        }
      goto release;
    }

  /* single byte with correct flags after one sequence: no labels needed */
  if (size == 1 && (litright || !sign))
    {
      cmpByteFlags (laop, raop, 0, sign);
      if (ifx)
        {
          if (IC_TRUE (ifx))
            {
              emit2 ("if", "%s", cc_true);
              cost (1, 1);
              emitBranch ("br", IC_TRUE (ifx));
            }
          else
            {
              emit2 ("if", "%s", condInverse (cc_true));
              cost (1, 1);
              emitBranch ("br", IC_FALSE (ifx));
            }
          ifx->generated = 1;
        }
      else
        {
          emit2 ("ldac", "%s", inv ? condInverse (cc_true) : cc_true);
          cost (1, 1);
          storeA (result->aop, 0);
          for (int i = 1; i < result->aop->size; i++)
            {
              emit2 ("ldi", "#0x00");
              cost (1, 1);
              storeA (result->aop, i);
            }
        }
      goto release;
    }

  /* general case: from the most significant byte down, branching to
     tlbl_true / tlbl_false as soon as the bytes differ */
  for (int i = size - 1; i >= 0; i--)
    {
      bool s = sign && i == size - 1;
      bool last = (i == 0);

      if (s && !litright)
        {
          /* signed top byte against memory: cmp only when the operand is not 0 */
          symbol *tlbl_zero = regalloc_dry_run ? 0 : newiTempLabel (0);
          symbol *tlbl_next = regalloc_dry_run ? 0 : newiTempLabel (0);
          loadA (raop, i);
          emit2 ("cpi", "#0x01");
          emit2 ("if", "c");
          cost (2, 2);
          emitBranch ("br", tlbl_zero);
          loadA (laop, i);
          emitAluA ("cmp", raop, i);
          emit2 ("if", "%s", lt ? "slt" : "sgt");
          cost (1, 1);
          emitBranch ("br", tlbl_true);
          emit2 ("if", "%s", lt ? "sgt" : "slt");
          cost (1, 1);
          emitBranch ("br", tlbl_false);
          emitBranch ("br", tlbl_next);
          emitLbl (tlbl_zero);
          /* right byte is 0: left < 0 is its sign, left > 0 is "not negative and not zero" */
          loadA (laop, i);
          emit2 ("btst", "7");
          cost (1, 1);
          emitBranch ("bz", lt ? tlbl_true : tlbl_false);   /* Z = bit 7 */
          emit2 ("cpi", "#0x00");
          cost (1, 1);
          emitBranch ("bnz", lt ? tlbl_false : tlbl_true);
          emitLbl (tlbl_next);
          continue;
        }

      if (litzero)
        {
          if (s)
            {
              /* the signed top byte against 0: negative is below, 0 goes on, else above */
              loadA (laop, i);
              emit2 ("btst", "7");
              cost (1, 1);
              emitBranch ("bz", lt ? tlbl_true : tlbl_false);     /* Z = bit 7 */
              emit2 ("cpi", "#0x00");
              cost (1, 1);
              emitBranch ("bnz", lt ? tlbl_false : tlbl_true);
              continue;
            }
          /* a byte is never below 0: non-zero is above, and the last one
             can not make the left side smaller */
          if (lt && last)
            break;
          loadAZ (laop, i);
          emitBranch ("bnz", lt ? tlbl_false : tlbl_true);
          continue;
        }

      cmpByteFlags (laop, raop, i, s);
      emit2 ("if", "%s", cc_true);
      cost (1, 1);
      emitBranch ("br", tlbl_true);
      if (!last)
        {
          emit2 ("if", "%s", cc_false);
          cost (1, 1);
          emitBranch ("br", tlbl_false);
        }
    }

  if (ifx)
    {
      if (IC_TRUE (ifx))
        {
          emitLbl (tlbl_false);
          emitBranch ("br", tlbl_done);
          emitLbl (tlbl_true);
          emitBranch ("br", IC_TRUE (ifx));
          emitLbl (tlbl_done);
        }
      else
        {
          emitLbl (tlbl_false);
          emitBranch ("br", IC_FALSE (ifx));
          emitLbl (tlbl_true);
        }
      ifx->generated = 1;
    }
  else
    {
      emitLbl (tlbl_false);
      emit2 ("ldi", inv ? "#0x01" : "#0x00");
      cost (1, 1);
      emitBranch ("br", tlbl_done);
      emitLbl (tlbl_true);
      emit2 ("ldi", inv ? "#0x00" : "#0x01");
      cost (1, 1);
      emitLbl (tlbl_done);
      storeA (result->aop, 0);
      for (int i = 1; i < result->aop->size; i++)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
          storeA (result->aop, i);
        }
    }

release:
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genCmpEQorNE - equality                                         */
/*-----------------------------------------------------------------*/
static void
genCmpEQorNE (const iCode *ic, iCode *ifx)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  bool eq = (ic->op == EQ_OP);
  int size;

  D (emit2 ("; genCmpEQorNE", ""));

  /* a ! on the result: the inverse into the !'s result instead */
  iCode *notic = ifx ? 0 : notForOp (result, ic);
  if (notic)
    {
      result = IC_RESULT (notic);
      notic->generated = 1;
      eq = !eq;
    }

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  size = max (left->aop->size, right->aop->size);

  asmop *laop = left->aop, *raop = right->aop;
  /* literals on the right; a byte in A on the left (loading it is a no-op) */
  if (laop->type == AOP_LIT || laop->type == AOP_IMMD ||
      raop->type == AOP_REG && laop->type != AOP_REG)
    {
      asmop *t = laop; laop = raop; raop = t;
    }

  symbol *tlbl_ne = regalloc_dry_run ? 0 : newiTempLabel (0);
  symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);

  /* compare with zero: or the bytes together */
  if (raop->type == AOP_LIT && aopIsLitVal (raop, 0, size, 0))
    {
      if (laop == left->aop)
        loadTruth (left);
      else
        loadOrBytes (laop, size);
    }
  else
    {
      for (int i = 0; i < size; i++)
        {
          /* only Z is used here, and cmp/cpi get Z right */
          loadA (laop, i);
          emitAluA ("cmp", raop, i);
          if (i < size - 1)
            emitBranch ("bnz", tlbl_ne);
        }
    }
  /* here: Z == 1 iff equal (last byte), earlier differing bytes went to tlbl_ne */

  if (ifx)
    {
      if (size == 1 || raop->type == AOP_LIT && aopIsLitVal (raop, 0, size, 0))
        {
          if (IC_TRUE (ifx))
            emitBranch (eq ? "bz" : "bnz", IC_TRUE (ifx));
          else
            emitBranch (eq ? "bnz" : "bz", IC_FALSE (ifx));
        }
      else if (eq)
        {
          if (IC_TRUE (ifx))
            {
              emitBranch ("bz", IC_TRUE (ifx));
              emitLbl (tlbl_ne);
            }
          else
            {
              emitLbl (tlbl_ne);
              emitBranch ("bnz", IC_FALSE (ifx));
            }
        }
      else
        {
          if (IC_TRUE (ifx))
            {
              emitLbl (tlbl_ne);
              emitBranch ("bnz", IC_TRUE (ifx));
            }
          else
            {
              emitBranch ("bz", IC_FALSE (ifx));
              emitLbl (tlbl_ne);
            }
        }
      ifx->generated = 1;
    }
  else
    {
      if (size == 1 || raop->type == AOP_LIT && aopIsLitVal (raop, 0, size, 0))
        {
          emit2 ("ldac", eq ? "eq" : "ne");
          cost (1, 1);
        }
      else
        {
          emit2 ("ldac", eq ? "eq" : "ne");
          cost (1, 1);
          emitBranch ("br", tlbl_done);
          emitLbl (tlbl_ne);
          emit2 ("ldi", eq ? "#0x00" : "#0x01");
          cost (1, 1);
          emitLbl (tlbl_done);
        }
      storeA (result->aop, 0);
      for (int i = 1; i < result->aop->size; i++)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
          storeA (result->aop, i);
        }
    }

  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genIfx - generate code for Ifx statement                        */
/*-----------------------------------------------------------------*/
static void
genIfx (iCode *ic)
{
  operand *cond = IC_COND (ic);

  D (emit2 ("; genIfx", ""));

  aopOp (cond, ic);

  if (cond->aop->type == AOP_CND)
    wassertl (0, "Condition operand without a comparison");

  loadTruth (cond);
  if (IC_TRUE (ic))
    emitBranch ("bnz", IC_TRUE (ic));
  else
    emitBranch ("bz", IC_FALSE (ic));

  freeAsmop (cond);
}

/*-----------------------------------------------------------------*/
/* Shifts                                                          */
/*-----------------------------------------------------------------*/

/* Shift the (memory) operand aop left by one bit in place: shl rotates
   through C (amode[0] = 1), the first byte starts with C = 0. */
static void
shiftLeft1 (const asmop *aop, int size)
{
  for (int i = 0; i < size; i++)
    {
      loadA (aop, i);
      if (!i)
        {
          emit2 ("ldc", "#0");
          cost (1, 1);
        }
      emit2 ("shl", "");
      cost (1, 1);
      storeA (aop, i);
    }
}

/* C <- bit 7 of A (the sign), A and Z kept: for an arithmetic shift the
   carry fed into the top byte is the sign.  amode stays 1 throughout the
   generated code: it cannot be read back, so an interrupt handler could
   not restore it. */
static void
emitSignToC (void)
{
  emit2 ("ldc", "#0");
  emit2 ("btst", "7");
  emit2 ("if", "z");
  emit2 ("ldc", "#1");
  cost (4, 4);
}

/* Shift the (memory) operand right by one bit in place, from the top byte down. */
static void
shiftRight1 (const asmop *aop, int size, bool sign)
{
  for (int i = size - 1; i >= 0; i--)
    {
      loadA (aop, i);
      if (i == size - 1)
        {
          if (sign)
            emitSignToC ();
          else
            {
              emit2 ("ldc", "#0");
              cost (1, 1);
            }
        }
      emit2 ("shr", "");
      cost (1, 1);
      storeA (aop, i);
    }
}

static void
genShift (const iCode *ic, bool left_shift)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int size;
  bool sign;

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);
  size = result->aop->size;
  sign = !left_shift && !isUnsignedOp (left);

  if (right->aop->type == AOP_LIT)
    {
      int n = (int) ulFromVal (right->aop->aopu.aop_lit);

      if (n >= size * 8)
        {
          /* all bits shifted out (or sign fill) */
          if (sign)
            {
              loadA (left->aop, size - 1);
              emit2 ("shl", "");
              emit2 ("ifte", "c");
              emit2 ("ldi", "#0xff");
              emit2 ("ldi", "#0x00");
              cost (4, 4);
            }
          else
            {
              emit2 ("ldi", "#0x00");
              cost (1, 1);
            }
          for (int i = 0; i < size; i++)
            storeA (result->aop, i);
          goto release;
        }

      /* single byte: shift in A.  The shifts rotate through C: let the
         garbage in and mask it afterwards (one andi for any count); an
         arithmetic shift then fills the top bits with the sign. */
      if (size == 1)
        {
          loadA (left->aop, 0);
          if (n == 1 && !sign)
            {
              emit2 ("ldc", "#0");
              emit2 (left_shift ? "shl" : "shr", "");
              cost (2, 2);
            }
          else
            {
              for (int i = 0; i < n; i++)
                {
                  emit2 (left_shift ? "shl" : "shr", "");
                  cost (1, 1);
                }
              emit2 ("andi", "#0x%02x", left_shift ? (0xff << n) & 0xff : 0xff >> n);
              cost (1, 1);
              if (sign)
                {
                  /* the masked bits are zero: adding the fill is an or */
                  emit2 ("ldc", "#0");
                  emit2 ("btst", "%d", 7 - n);
                  emit2 ("if", "z");
                  emit2 ("adc", "#0x%02x", (0xff << (8 - n)) & 0xff);
                  cost (4, 4);
                }
            }
          storeA (result->aop, 0);
          goto release;
        }

      /* multi-byte: whole bytes first */
      int bytes = n / 8, bits = n % 8;

      /* 16 bits: by a whole byte, the moved byte is shifted in A alone;
         by 1..7 bits, shl16 / shr16 shift {n(sp), A} as a pair (the high
         byte is pushed to 1(sp) unless it is there already), with C as the
         bit shifted in (amode 1): 0, or the sign set once (it stays, the
         shifts do not touch C) */
      if (size == 2 && left->aop->size == 2 && bytes == 1)
        {
          if (left_shift)
            {
              loadA (left->aop, 0);
              for (int i = 0; i < bits; i++)
                {
                  emit2 ("shl", "");
                  cost (1, 1);
                }
              if (bits)
                {
                  emit2 ("andi", "#0x%02x", (0xff << bits) & 0xff);
                  cost (1, 1);
                }
              storeA (result->aop, 1);
              emit2 ("ldi", "#0x00");
              cost (1, 1);
              storeA (result->aop, 0);
            }
          else
            {
              loadA (left->aop, 1);
              for (int i = 0; i < bits; i++)
                {
                  emit2 ("shr", "");
                  cost (1, 1);
                }
              if (bits)
                {
                  emit2 ("andi", "#0x%02x", 0xff >> bits);
                  cost (1, 1);
                  if (sign)
                    {
                      emit2 ("ldc", "#0");
                      emit2 ("btst", "%d", 7 - bits);
                      emit2 ("if", "z");
                      emit2 ("adc", "#0x%02x", (0xff << (8 - bits)) & 0xff);
                      cost (4, 4);
                    }
                }
              storeA (result->aop, 0);
              if (sign)
                {
                  emit2 ("btst", "7");
                  emit2 ("ifte", "z");
                  emit2 ("ldi", "#0xff");
                  emit2 ("ldi", "#0x00");
                  cost (4, 4);
                }
              else
                {
                  emit2 ("ldi", "#0x00");
                  cost (1, 1);
                }
              storeA (result->aop, 1);
            }
          goto release;
        }
      if (size == 2 && left->aop->size == 2 && bits)
        {
          bool inplace = aopSame (result->aop, 0, left->aop, 0, 2) && result->aop->type == AOP_STK &&
                         !stkIsFar (result->aop, 1) && stkOffset (result->aop, 1) <= 3;
          /* one bit, byte-wise, is as short and quicker */
          if (inplace || bits > 1)
            {
              const char *sh = left_shift ? "shl16" : "shr16";
              char hi[8];
              if (inplace)
                SNPRINTF (hi, sizeof (hi), "%d", stkOffset (result->aop, 1));
              else
                strcpy (hi, "1");
              if (!inplace)
                {
                  loadA (left->aop, 1);
                  if (!left_shift && sign)
                    emitSignToC ();
                  pushA ();
                }
              else if (!left_shift && sign)
                loadA (left->aop, 1), emitSignToC ();
              if (!left_shift && sign)
                loadAKeepC (left->aop, 0);
              else
                {
                  loadA (left->aop, 0);
                  emit2 ("ldc", "#0");
                  cost (1, 1);
                }
              for (int b = 0; b < bits; b++)
                {
                  emit2 (sh, "%s(sp)", hi);
                  cost (1, 2);
                }
              storeA (result->aop, 0);
              if (!inplace)
                {
                  popA ();
                  storeA (result->aop, 1);
                }
              goto release;
            }
        }

      if (left_shift)
        {
          for (int i = size - 1; i >= 0; i--)
            {
              if (i - bytes >= 0)
                cheapMove (result->aop, i, left->aop, i - bytes);
              else
                {
                  emit2 ("ldi", "#0x00");
                  cost (1, 1);
                  storeA (result->aop, i);
                }
            }
          for (int b = 0; b < bits; b++)
            shiftLeft1 (result->aop, size);
        }
      else
        {
          for (int i = 0; i < size; i++)
            {
              if (i + bytes < size)
                cheapMove (result->aop, i, left->aop, i + bytes);
              else if (sign)
                {
                  loadA (left->aop, size - 1);
                  emit2 ("shl", "");
                  emit2 ("ifte", "c");
                  emit2 ("ldi", "#0xff");
                  emit2 ("ldi", "#0x00");
                  cost (4, 4);
                  storeA (result->aop, i);
                }
              else
                {
                  emit2 ("ldi", "#0x00");
                  cost (1, 1);
                  storeA (result->aop, i);
                }
            }
          for (int b = 0; b < bits; b++)
            shiftRight1 (result->aop, size, sign);
        }
      goto release;
    }

  /* variable shift count: loop */
  {
    symbol *tlbl = regalloc_dry_run ? 0 : newiTempLabel (0);
    symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);

    /* the count first: the result may share its stack slot with the
       count (an operand that dies here) */
    loadA (right->aop, 0);
    pushA ();

    /* The loops test the count once (the ldax brings its line into the
       TT07 data cache for the first dcx) and count it down after each
       step - dcx sets Z - with the bit shifted in from C: 0, or the sign
       kept in the save shadow (dcx and the cache's cmp clobber C). */

    /* 16 bits: the pair {1(sp), A} shifted by shl16 / shr16, the count
       at 2(sp) */
    if (size == 2 && left->aop->size == 2)
      {
        symbol *tlbl_zero = regalloc_dry_run ? 0 : newiTempLabel (0);
        loadA (left->aop, 1);
        if (!left_shift && sign)
          {
            emitSignToC ();
            emit2 ("savec", "");
            cost (1, 1);
          }
        pushA ();
        emit2 ("ldax", "2(sp)");
        cost (1, 1);
        emitBranch ("bz", tlbl_zero);
        loadA (left->aop, 0);
        emitLbl (tlbl);
        if (!left_shift && sign)
          emit2 ("restc", "");
        else
          emit2 ("ldc", "#0");
        emit2 (left_shift ? "shl16" : "shr16", "1(sp)");
        emit2 ("dcx", "2(sp)");
        cost (3, 5);
        emitBranch ("bnz", tlbl);
        emitBranch ("br", tlbl_done);
        emitLbl (tlbl_zero);
        loadA (left->aop, 0);
        emitLbl (tlbl_done);
        storeA (result->aop, 0);
        popA ();
        storeA (result->aop, 1);
        adjustStack (1);
        goto release;
      }

    genMove (result->aop, left->aop);
    emit2 ("ldax", "1(sp)");
    cost (1, 1);
    emitBranch ("bz", tlbl_done);

    /* a byte: in A throughout, dcx leaves it alone */
    if (size == 1)
      {
        loadA (result->aop, 0);
        if (!left_shift && sign)
          {
            emitSignToC ();
            emit2 ("savec", "");
            cost (1, 1);
          }
        emitLbl (tlbl);
        if (!left_shift && sign)
          emit2 ("restc", "");
        else
          emit2 ("ldc", "#0");
        emit2 (left_shift ? "shl" : "shr", "");
        cost (2, 2);
        if (lisa_tt07_cache)
          {
            emit2 ("cmp", "1(sp)");
            cost (1, 1);
          }
        emit2 ("dcx", "1(sp)");
        cost (1, 2);
        emitBranch ("bnz", tlbl);
        storeA (result->aop, 0);
        emitLbl (tlbl_done);
        adjustStack (1);
        goto release;
      }

    /* a long: whole bytes first - while 8 or more remain the bytes move
       up (down) one and the count loses 8 (adc #0xf8 with C = 0; the
       TT07 adc adds (k + C) & 0xff) - then the bits that remain */
    if (size >= 3)
      {
        symbol *tlbl_bytes = regalloc_dry_run ? 0 : newiTempLabel (0);
        symbol *tlbl_bits = regalloc_dry_run ? 0 : newiTempLabel (0);
        emitLbl (tlbl_bytes);
        emit2 ("cpi", "#8");
        emit2 ("if", "c");
        cost (2, 2);
        emitBranch ("br", tlbl_bits);
        if (left_shift)
          {
            for (int i = size - 1; i >= 1; i--)
              cheapMove (result->aop, i, result->aop, i - 1);
            emit2 ("ldi", "#0x00");
            cost (1, 1);
            storeA (result->aop, 0);
          }
        else
          {
            for (int i = 0; i < size - 1; i++)
              cheapMove (result->aop, i, result->aop, i + 1);
            if (sign)
              {
                loadA (result->aop, size - 1);
                emit2 ("shl", "");
                emit2 ("ifte", "c");
                emit2 ("ldi", "#0xff");
                emit2 ("ldi", "#0x00");
                cost (4, 4);
              }
            else
              {
                emit2 ("ldi", "#0x00");
                cost (1, 1);
              }
            storeA (result->aop, size - 1);
          }
        emit2 ("ldax", "1(sp)");
        emit2 ("ldc", "#0");
        emit2 ("adc", "#0xf8");
        emit2 ("stax", "1(sp)");
        cost (4, 4);
        emitBranch ("br", tlbl_bytes);
        emitLbl (tlbl_bits);
        emit2 ("cpi", "#0");
        cost (1, 1);
        emitBranch ("bz", tlbl_done);
      }

    if (!left_shift && sign)
      {
        loadA (result->aop, size - 1);
        emitSignToC ();
        emit2 ("savec", "");
        cost (1, 1);
      }
    emitLbl (tlbl);
    if (left_shift)
      shiftLeft1 (result->aop, size);
    else
      for (int i = size - 1; i >= 0; i--)
        {
          loadA (result->aop, i);
          if (i == size - 1)
            {
              if (sign)
                emit2 ("restc", "");
              else
                emit2 ("ldc", "#0");
              cost (1, 1);
            }
          emit2 ("shr", "");
          cost (1, 1);
          storeA (result->aop, i);
        }
    if (lisa_tt07_cache)
      {
        emit2 ("cmp", "1(sp)");
        cost (1, 1);
      }
    emit2 ("dcx", "1(sp)");
    cost (1, 2);
    emitBranch ("bnz", tlbl);
    emitLbl (tlbl_done);
    adjustStack (1);
  }

release:
  if (left_shift)
    fixBitIntResult (ic, false);
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

static void
genLeftShift (const iCode *ic)
{
  D (emit2 ("; genLeftShift", ""));
  genShift (ic, true);
}

static void
genRightShift (const iCode *ic)
{
  D (emit2 ("; genRightShift", ""));
  genShift (ic, false);
}

/*-----------------------------------------------------------------*/
/* genMult - multiplication with mul / mulu                        */
/*-----------------------------------------------------------------*/
static void
genMult (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);

  D (emit2 ("; genMult", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  int size = result->aop->size;
  asmop *laop = left->aop, *raop = right->aop;

  /* mul needs a memory operand: literals on the right; a byte in A on the left */
  if (laop->type == AOP_LIT || laop->type == AOP_IMMD || !aopIsMem (raop, 0) && aopIsMem (laop, 0) ||
      raop->type == AOP_REG && laop->type != AOP_REG)
    {
      asmop *t = laop; laop = raop; raop = t;
    }

  if (size == 1)
    {
      loadA (laop, 0);
      emitAluA ("mul", raop, 0);
      storeA (result->aop, 0);
    }
  else if (laop->size == 1 && raop->size == 1)
    {
      /* 8x8 -> 16: mul / mulu give the unsigned product; signed (both
         operands are) is corrected afterwards, high -= b if a < 0 and
         high -= a if b < 0, instead of the amode 3 mul (amode stays 1, an
         interrupt handler could not restore it) */
      bool sign = !isUnsignedOp (left) && !isUnsignedOp (right);
      asmop rtmp;
      int rpushed = 0;
      if (!aopIsMem (raop, 0))
        {
          loadA (raop, 0);
          pushA ();
          memset (&rtmp, 0, sizeof (rtmp));
          rtmp.type = AOP_STK;
          rtmp.size = 1;
          rtmp.aopu.bytes[0].byteu.stk = 1 - G.stack.pushed;
          raop = &rtmp;
          rpushed = 1;
        }
      loadA (laop, 0);
      emitAluA ("mul", raop, 0);
      pushA ();                                      /* low byte */
      loadA (laop, 0);
      emitAluA ("mulu", raop, 0);                    /* A = high byte */
      if (sign)
        {
          symbol *tlbl_a = regalloc_dry_run ? 0 : newiTempLabel (0);
          symbol *tlbl_b = regalloc_dry_run ? 0 : newiTempLabel (0);
          pushA ();                                  /* high byte at 1(sp) */
          loadA (laop, 0);
          emit2 ("btst", "7");
          cost (1, 1);
          emitBranch ("bnz", tlbl_a);                /* Z = bit 7 */
          emit2 ("ldax", "1(sp)");
          /* a literal is subtracted as adc #~k with C = 1 (no borrow in),
             memory with the hardware sub and C = 0; the value is right
             whatever C ends up as */
          emit2 ("ldc", (raop->type == AOP_LIT) ? "#1" : "#0");
          cost (2, 2);
          emitAluA ("sub", raop, 0);
          emit2 ("stax", "1(sp)");
          cost (1, 1);
          emitLbl (tlbl_a);
          loadA (raop, 0);
          emit2 ("btst", "7");
          cost (1, 1);
          emitBranch ("bnz", tlbl_b);
          emit2 ("ldax", "1(sp)");
          emit2 ("ldc", (laop->type == AOP_LIT) ? "#1" : "#0");
          cost (2, 2);
          emitAluA ("sub", laop, 0);
          emit2 ("stax", "1(sp)");
          cost (1, 1);
          emitLbl (tlbl_b);
          popA ();
        }
      storeA (result->aop, 1);
      popA ();
      storeA (result->aop, 0);
      if (rpushed)
        adjustStack (1);
      for (int i = 2; i < size; i++)
        {
          wassertl (regalloc_dry_run || !sign, "Unimplemented signed 8x8 multiplication with result wider than 16 bits");
          emit2 ("ldi", "#0x00");
          cost (1, 1);
          storeA (result->aop, i);
        }
    }
  else
    {
      /* 16x16 -> 16 (the low 16 bits do not depend on signedness):
         hi = mulu(l0,r0) + l0*r1 + l1*r0, lo = l0*r0 */
      wassertl (regalloc_dry_run || size == 2, "Unimplemented multiplication width");
      if (!aopIsMem (raop, 0))
        {
          /* right operand not in memory: put a copy on the stack */
          loadA (raop, 1);
          pushA ();
          loadA (raop, 0);
          pushA ();
          asmop tmp;
          memset (&tmp, 0, sizeof (tmp));
          tmp.type = AOP_STK;
          tmp.size = 2;
          tmp.aopu.bytes[0].byteu.stk = 1 - G.stack.pushed;
          tmp.aopu.bytes[1].byteu.stk = 2 - G.stack.pushed;
          raop = &tmp;
          loadA (laop, 0);
          emitAluA ("mulu", raop, 0);
          pushA ();
          loadA (laop, 0);
          emitAluA ("mul", raop, 1);
          emit2 ("ldc", "#0");
          emit2 ("add", "1(sp)");
          emit2 ("stax", "1(sp)");
          cost (3, 3);
          loadA (laop, 1);
          emitAluA ("mul", raop, 0);
          emit2 ("ldc", "#0");
          emit2 ("add", "1(sp)");
          emit2 ("stax", "1(sp)");
          cost (3, 3);
          loadA (laop, 0);
          emitAluA ("mul", raop, 0);
          pushA ();
          /* stack: lo, hi, r0, r1 */
          popA ();
          storeA (result->aop, 0);
          popA ();
          storeA (result->aop, 1);
          adjustStack (2);
        }
      else
        {
          loadA (laop, 0);
          emitAluA ("mulu", raop, 0);
          pushA ();
          if (raop->size > 1)
            {
              loadA (laop, 0);
              emitAluA ("mul", raop, 1);
              emit2 ("ldc", "#0");
              emit2 ("add", "1(sp)");
              emit2 ("stax", "1(sp)");
              cost (3, 3);
            }
          if (laop->size > 1)
            {
              loadA (laop, 1);
              emitAluA ("mul", raop, 0);
              emit2 ("ldc", "#0");
              emit2 ("add", "1(sp)");
              emit2 ("stax", "1(sp)");
              cost (3, 3);
            }
          loadA (laop, 0);
          emitAluA ("mul", raop, 0);
          storeA (result->aop, 0);
          popA ();
          storeA (result->aop, 1);
        }
    }

  fixBitIntResult (ic, false);
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genDivMod - unsigned division / modulus on the hardware divider */
/*-----------------------------------------------------------------*/

/* The divider takes the dividend from {RA[7:0], IX[7:0]} (lddiv n(sp):
   IX[7:0] <- A, RA[7:0] <- n(sp); or tax for an 8-bit one) and the
   divisor from {n(sp), A} (div 0, n(sp)) or A alone (div 3, both 8-bit);
   dv bit 1 marks an 8-bit dividend.  The low byte of the result comes
   back in A, and the whole 16 bits in RA (ra_cond set); IX is zeroed.
   TT07 silicon: the result's high byte is not stored to the stack as the
   RTL intends, and the forms whose second word has bit 1 clear (offsets
   0, 1, 4, 5 ...; the one-word div 1) write the low byte to the offset's
   address instead - so the divisor's high byte goes to 2(sp), and the
   high byte of the result is read from RA (xchg ra; txau).  Signed mode
   is amode[1], which stays off: everything here is unsigned. */
static void
genDivMod (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  const char *op = (ic->op == '/') ? "div" : "rem";

  D (emit2 ("; genDivMod", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  int lsize = left->aop->size, rsize = right->aop->size, size = result->aop->size;
  asmop *laop = left->aop, *raop = right->aop;
  asmop tmp;
  int pushed = 0;

  /* by one: the quotient is the dividend (the divider's RA would say 0
     for the high byte), the remainder 0 */
  if (raop->type == AOP_LIT && aopIsLitVal (raop, 0, rsize, 1))
    {
      if (ic->op == '/')
        genMove (result->aop, laop);
      else
        for (int i = 0; i < size; i++)
          cheapMove (result->aop, i, ASMOP_ZERO, 0);
      goto release;
    }

  /* a signed byte (or a mixed pair) with a byte result: the library's
     byte-returning helpers (divu.s), b pushed and a in A, no return slot */
  if (size == 1 && lsize == 1 && rsize == 1 && !(isUnsignedOp (left) && isUnsignedOp (right)))
    {
      const char *name = !isUnsignedOp (left) && !isUnsignedOp (right) ? "schar8" :
                         !isUnsignedOp (left) ? "uschar8" : "suchar8";
      bool a_in_A = aopInReg (laop, 0, A_IDX);
      if (a_in_A && raop->type != AOP_CODE && raop->type != AOP_STL)
        {
          /* a parked in IX's low byte while b is loaded (b needs no IX) */
          emit2 ("tax", "");
          cost (1, 1);
          loadA (raop, 0);
          pushA ();
          emit2 ("txa", "");
          cost (1, 1);
          pushed = 1;
        }
      else if (a_in_A)
        {
          /* b's load needs IX: a pushed first, b on top, a back in A (its
             copy stays under b) */
          pushA ();
          loadA (raop, 0);
          pushA ();
          emit2 ("ldax", "2(sp)");
          cost (1, 1);
          pushed = 2;
        }
      else
        {
          loadA (raop, 0);
          pushA ();
          loadA (laop, 0);
          pushed = 1;
        }
      emit2 ("jal", "__%s%s", ic->op == '/' ? "div" : "mod", name);
      cost (1, 40);
      ixInvalidate ();
      adjustStack (pushed);
      storeA (result->aop, 0);
      goto release;
    }

  /* the divisor is loaded after the dividend sits in IX / RA: a code
     space read (call ix) or a stack address (spix) would clobber them,
     and a byte in A has to be taken now */
  if (raop->type == AOP_CODE || raop->type == AOP_STL || raop->type == AOP_REG)
    {
      if (laop->type == AOP_REG)
        {
          /* the dividend is in A too: it goes first */
          pushA ();
          memset (&tmp, 0, sizeof (tmp));
          tmp.type = AOP_STK;
          tmp.size = 1;
          tmp.aopu.bytes[0].byteu.stk = 1 - G.stack.pushed;
          laop = &tmp;
          pushed++;
        }
      asmop *rt = Safe_calloc (1, sizeof (asmop));
      rt->type = AOP_STK;
      rt->size = rsize;
      for (int i = rsize - 1; i >= 0; i--)
        {
          loadA (raop, i);
          pushA ();
        }
      for (int i = 0; i < rsize; i++)
        rt->aopu.bytes[i].byteu.stk = 1 + i - G.stack.pushed;
      raop = rt;
      pushed += rsize;
    }

  /* the dividend */
  int dv = 0;
  if (lsize == 1)
    {
      loadA (laop, 0);
      emit2 ("tax", "");
      cost (1, 1);
      dv = 2;
    }
  else if (laop->type == AOP_STK && !stkIsFar (laop, 0) && !stkIsFar (laop, 1))
    {
      loadA (laop, 0);
      emit2 ("lddiv", "%s", stkArg (laop, 1));
      cost (2, 2);
    }
  else
    {
      loadA (laop, 1);
      pushA ();
      loadA (laop, 0);
      emit2 ("lddiv", "1(sp)");
      cost (2, 2);
      adjustStack (1);
    }
  ixInvalidate ();

  /* the divisor: a byte (an operand of one, or a literal below 256) */
  bool r8 = (rsize == 1 || raop->type == AOP_LIT && aopIsLitVal (raop, 1, 1, 0));
  if (dv == 2 && r8)
    {
      loadA (raop, 0);
      emit2 (op, "3");
      cost (1, 2);
    }
  else
    {
      /* the high byte at 2(sp): pushed twice (the second one is the copy under it) */
      if (r8)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
        }
      else
        loadA (raop, 1);
      pushA ();
      pushA ();
      loadA (raop, 0);
      emit2 (op, "%d, 2(sp)", dv);
      cost (2, 2);
      adjustStack (2);
    }
  storeA (result->aop, 0);
  if (size > 1)
    {
      /* the high byte from RA (15 bits; bit 7 of the upper half is
         ra_cond): _hasNativeMulFor only lets the cases through where it
         fits - a 16-bit quotient by a literal (RA says 0 for a divisor of
         1, which genMove handled above), a remainder by a literal below
         0x8000.  An 8-bit dividend, or a remainder by a byte, cannot have
         one */
      if (dv == 2 || ic->op == '%' && r8)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
        }
      else
        {
          emit2 ("xchg", "ra");
          emit2 ("txau", "");
          emit2 ("andi", "#0x7f");
          cost (3, 3);
        }
      storeA (result->aop, 1);
      for (int i = 2; i < size; i++)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
          storeA (result->aop, i);
        }
    }

  if (pushed)
    adjustStack (pushed);
  if (raop != right->aop && raop != &tmp)
    Safe_free (raop);
release:
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* Pointers                                                        */
/*-----------------------------------------------------------------*/

/* Pointer type of an operand: POINTER (data), CPOINTER (code), GPOINTER. */
static int
ptrType (const operand *op)
{
  sym_link *type = operandType (op);
  int ptype = (IS_PTR (type) && !IS_FUNC (type->next)) ? DCL_TYPE (type) : PTR_TYPE (SPEC_OCLS (getSpec (type)));

  if (op->aop->type == AOP_IMMD)
    ptype = op->aop->aopu.code ? CPOINTER : POINTER;
  else if (op->aop->type == AOP_LIT)
    /* ldx cannot carry bit 15 (it sets the condition tag), so the space
       of a literal address has to be decided here */
    ptype = (ulFromVal (op->aop->aopu.aop_lit) & 0x8000) ? CPOINTER : POINTER;
  else if (op->aop->type == AOP_STL)
    ptype = POINTER;
  else if (ptype != CPOINTER && ptype != GPOINTER)
    ptype = POINTER;
  return (ptype);
}

/* Can size bytes at this constant address be reached by the direct
   lda/sta forms (9-bit address)?  A symbol is assumed to be placed there
   by the linker. */
static bool
litDirect (const asmop *aop, int size)
{
  if (aop->type == AOP_IMMD)
    return !aop->aopu.far;
  if (aop->type == AOP_LIT)
    return ((ulFromVal (aop->aopu.aop_lit) & 0xffff) + size <= 0x200);
  return false;
}

/* IX <- pointer operand. Clobbers A for pointers in data memory. */
static void
ixLoadPtr (const asmop *aop)
{
  switch (aop->type)
    {
    case AOP_IMMD:
      ixLoadSym (aop, 0);
      break;
    case AOP_STK:
      {
        int stk = aop->aopu.bytes[0].byteu.stk;
        if (G.ix.type == AOP_STK && G.ix.offset == stk)
          return;
        if (stkIsFar (aop, 0) || stkIsFar (aop, 1))
          {
            /* ldxx is an SP window too: byte by byte, the far loads keep IX */
            loadA (aop, 0);
            ixInvalidate ();
            emit2 ("tax", "");
            loadA (aop, 1);
            emit2 ("taxu", "");
            cost (2, 2);
          }
        else
          {
            emit2 ("ldxx", "%s", stkArg (aop, 0));
            cost (1, 2);
          }
        G.ix.type = AOP_STK;
        G.ix.offset = stk;
      }
      break;
    case AOP_STL:
      ixLoadStackAddr (aop->aopu.stk_off);
      break;
    case AOP_DIR:
    case AOP_SFR:
      if (G.ix.type == AOP_DIR && G.ix.base && !strcmp (G.ix.base, aop->aopu.immd) && G.ix.offset == aop->aopu.immd_off)
        return;
      loadA (aop, 0);
      emit2 ("tax", "");
      loadA (aop, 1);
      emit2 ("taxu", "");
      cost (2, 2);
      G.ix.type = AOP_DIR;
      G.ix.base = aop->aopu.immd;
      G.ix.offset = aop->aopu.immd_off;
      break;
    case AOP_LIT:
      emit2 ("ldx", "#0x%04x", (unsigned) ulFromVal (aop->aopu.aop_lit) & 0x7fff);
      cost (2, 2);
      ixInvalidate ();
      break;
    default:
      loadA (aop, 0);
      emit2 ("tax", "");
      loadA (aop, 1);
      emit2 ("taxu", "");
      cost (2, 2);
      ixInvalidate ();
    }
}

/* {ix_cond, IX} <- a two-byte value (a result on its way out).  ldxx
   from the stack; a literal with bit 15 set is an ldx (which sets
   ix_cond); the rest through A, the high byte first and parked when its
   load itself needs IX (code space, a far slot). */
static void
ixLoadValue (const asmop *aop)
{
  if (aop->type == AOP_STK && !stkIsFar (aop, 0) && !stkIsFar (aop, 1))
    {
      int stk = aop->aopu.bytes[0].byteu.stk;
      if (G.ix.type == AOP_STK && G.ix.offset == stk)
        return;
      emit2 ("ldxx", "%s", stkArg (aop, 0));
      cost (1, 2);
      G.ix.type = AOP_STK;
      G.ix.offset = stk;
      return;
    }
  if (aop->type == AOP_LIT && (ulFromVal (aop->aopu.aop_lit) & 0x8000))
    {
      emit2 ("ldx", "#0x%04x", (unsigned) ulFromVal (aop->aopu.aop_lit) & 0x7fff);
      cost (2, 2);
      ixInvalidate ();
      return;
    }
  bool via_ix = aop->type == AOP_CODE || aop->type == AOP_STK || (aop->type == AOP_DIR && aop->aopu.far);
  ixInvalidate ();
  if (via_ix)
    {
      loadA (aop, 1);
      pushA ();
      loadA (aop, 0);
      emit2 ("tax", "");
      cost (1, 1);
      popA ();
    }
  else
    {
      loadA (aop, 0);
      emit2 ("tax", "");
      cost (1, 1);
      loadA (aop, 1);
    }
  emit2 ("taxu", "");
  cost (1, 1);
}

/* Would ixLoadPtr go through A (tax / taxu) for this pointer? */
static bool
ixLoadPtrClobbersA (const asmop *aop)
{
  switch (aop->type)
    {
    case AOP_IMMD:
    case AOP_LIT:
    case AOP_STL:
      return (false);
    case AOP_STK:
      return (stkIsFar (aop, 0) || stkIsFar (aop, 1));
    default:
      return (true);
    }
}

/* A write to memory may change what IX was loaded from. */
static void
ixNoteStore (const asmop *aop, int offset)
{
  if (G.ix.type == AOP_STK && aop->type == AOP_STK && offset < 8 &&
      (aop->aopu.bytes[offset].byteu.stk == G.ix.offset || aop->aopu.bytes[offset].byteu.stk == G.ix.offset + 1))
    ixInvalidate ();
  if (G.ix.type == AOP_DIR && (aop->type == AOP_DIR || aop->type == AOP_SFR) && G.ix.base && !strcmp (G.ix.base, aop->aopu.immd))
    ixInvalidate ();
}

/* IX holds a pointer to constant data in code space (a pair index: word
   address / 2, bit 15 set).  Turn it into the word address: IX += IX. */
static void
ixCodePtrToWord (void)
{
  emit2 ("txau", "");
  emit2 ("andi", "#0x7f");
  emit2 ("addaxu", "");
  emit2 ("txa", "");
  emit2 ("addax", "");
  cost (5, 5);
  ixInvalidate ();
}

/* Read size bytes of code space data at IX (+ 2*off words) into result. Clobbers IX and RA. */
static void
codeReadToResult (const asmop *result, int off, int size)
{
  if (off)
    {
      emit2 ("adx", "#%d", 2 * off);
      cost (1, 1);
    }
  for (int i = 0; i < size; i++)
    {
      emit2 ("call", "ix");
      cost (1, 4);
      if (i < size - 1)
        {
          emit2 ("adx", "#1");
          cost (1, 1);
        }
      storeA (result, i);
    }
  ixInvalidate ();
}

/* The raw bytes of a bit-field have been read into result[0..nbytes-1].
   Shift and mask the partial byte, sign-extend it, and fill the rest of
   the result.  Shifts rotate through C (amode 1); the bits that come in
   land in the part the mask removes. */
static void
bitFieldFixResult (const asmop *result, int nbytes, int blen, int bstr, bool sign)
{
  int last = nbytes - 1;
  int bits = blen - 8 * last;          /* bits in the last byte, 1..8 */
  bool partial = bits < 8;

  if (partial || (sign && last + 1 < result->size))
    loadA (result, last);
  if (partial)
    {
      for (int j = 0; j < bstr; j++)
        {
          emit2 ("shr", "");
          cost (1, 1);
        }
      emit2 ("andi", "#0x%02x", 0xff >> (8 - bits));
      cost (1, 1);
      if (sign)
        {
          /* the masked bits are zero, so adding the high mask is an or */
          emit2 ("ldc", "#0");
          emit2 ("btst", "%d", bits - 1);
          emit2 ("if", "z");
          emit2 ("adc", "#0x%02x", (0xff00 >> (8 - bits)) & 0xff);
          cost (4, 4);
        }
      storeA (result, last);
    }
  if (last + 1 < result->size)
    {
      if (sign)
        {
          emit2 ("btst", "7");
          emit2 ("ifte", "z");
          emit2 ("ldi", "#0xff");
          emit2 ("ldi", "#0x00");
          cost (4, 4);
          for (int i = last + 1; i < result->size; i++)
            storeA (result, i);
        }
      else
        for (int i = last + 1; i < result->size; i++)
          cheapMove (result, i, ASMOP_ZERO, 0);
    }
}

/*-----------------------------------------------------------------*/
/* genPointerGet - generate code for pointer get                   */
/*-----------------------------------------------------------------*/
static void
genPointerGet (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int size, off = 0;

  D (emit2 ("; genPointerGet", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  wassertl (right, "GET_VALUE_AT_ADDRESS without right operand");
  wassertl (IS_OP_LITERAL (right), "GET_VALUE_AT_ADDRESS with non-literal right operand");
  off = (int) operandLitValue (right);

  size = result->aop->size;
  /* Like stm8: IS_BITVAR (operandType (left)->next) would be the natural test,
     but pointer reuse in unions makes the result type the reliable one. */
  bool bit_field = IS_BITVAR (operandType (result));
  int blen = bit_field ? SPEC_BLEN (getSpec (operandType (result))) : 0;
  int bstr = bit_field ? SPEC_BSTR (getSpec (operandType (result))) : 0;
  if (bit_field)
    {
      /* read only the bytes that hold the field; the rest is filled below */
      int nbytes = (blen + 7) / 8;
      wassertl (nbytes == 1 || !bstr, "Multi-byte bit-field not byte-aligned");
      if (nbytes < size)
        size = nbytes;
    }

  int ptype = ptrType (left);

  /* constant address in data space: direct */
  if (ptype == POINTER && litDirect (left->aop, size))
    {
      asmop dir;
      memset (&dir, 0, sizeof (dir));
      dir.type = AOP_DIR;
      dir.size = size;
      char buf[32];
      if (left->aop->type == AOP_LIT)
        {
          SNPRINTF (buf, sizeof (buf), "0x%04x", (unsigned) ulFromVal (left->aop->aopu.aop_lit) & 0xffff);
          dir.aopu.immd = buf;
          dir.aopu.immd_off = off;
        }
      else
        {
          dir.aopu.immd = left->aop->aopu.immd;
          dir.aopu.immd_off = left->aop->aopu.immd_off + off;
        }
      for (int i = 0; i < size; i++)
        cheapMove (result->aop, i, &dir, i);
      goto release;
    }

  /* constant address in code space */
  if (ptype == CPOINTER && left->aop->type == AOP_IMMD)
    {
      asmop code;
      memset (&code, 0, sizeof (code));
      code.type = AOP_CODE;
      code.size = size;
      code.aopu.immd = left->aop->aopu.immd;
      code.aopu.immd_off = left->aop->aopu.immd_off + off;
      code.aopu.code = true;
      ixLoadSym (&code, 0);
      codeReadToResult (result->aop, 0, size);
      goto release;
    }

  ixLoadPtr (left->aop);

  if (ptype == POINTER)
    {
      for (int i = 0; i < size; i++)
        {
          emit2 ("ldax", "%d(ix)", off + i);
          cost (1, 1);
          storeA (result->aop, i);
          ixNoteStore (result->aop, i);
        }
      goto release;
    }

  /* out of line (lib/lisa/gptrget.s) unless speed matters: the helpers
     read a byte and leave the raw address of the next one in IX with
     the space in C, which the stores between the calls keep */
  if (!optimize.codeSpeed && off <= 127)
    {
      if (off)
        {
          emit2 ("ldi", "#0x%02x", off);
          cost (1, 1);
          emit2 ("jal", ptype == CPOINTER ? "__gptrcodeo" : "__gptrgeto");
        }
      else
        emit2 ("jal", ptype == CPOINTER ? "__gptrcode" : "__gptrget");
      cost (1, 12);
      ixInvalidate ();
      storeA (result->aop, 0);
      for (int i = 1; i < size; i++)
        {
          emit2 ("jal", "__gptrnext");
          cost (1, 12);
          ixInvalidate ();
          storeA (result->aop, i);
        }
      goto release;
    }

  if (ptype == CPOINTER)
    {
      ixCodePtrToWord ();
      codeReadToResult (result->aop, off, size);
      goto release;
    }

  /* generic pointer: bit 15 (ix_cond after the load) selects code space */
  {
    symbol *tlbl_code = regalloc_dry_run ? 0 : newiTempLabel (0);
    symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);

    emit2 ("txau", "");
    emit2 ("btst", "7");
    cost (2, 2);
    emitBranch ("bz", tlbl_code);
    for (int i = 0; i < size; i++)
      {
        emit2 ("ldax", "%d(ix)", off + i);
        cost (1, 1);
        storeA (result->aop, i);
      }
    emitBranch ("br", tlbl_done);
    emitLbl (tlbl_code);
    ixCodePtrToWord ();
    codeReadToResult (result->aop, off, size);
    emitLbl (tlbl_done);
    ixInvalidate ();
  }

release:
  if (bit_field)
    bitFieldFixResult (result->aop, size, blen, bstr, !SPEC_USIGN (getSpec (operandType (result))));
  freeAsmop (left);
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genPointerPush - push the bytes of an object through a pointer  */
/* (a struct passed by value).  High byte first, like genIpush.    */
/*-----------------------------------------------------------------*/
static void
genPointerPush (const iCode *ic)
{
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);

  D (emit2 ("; genPointerPush", ""));

  aopOp (left, ic);

  wassertl (right, "IPUSH_VALUE_AT_ADDRESS without right operand");
  wassertl (IS_OP_LITERAL (right), "IPUSH_VALUE_AT_ADDRESS with non-literal right operand");
  int off = (int) operandLitValue (right);
  int size = getSize (operandType (left)->next);
  int ptype = ptrType (left);

  if (ptype == POINTER && litDirect (left->aop, size))
    {
      /* constant address in data space: direct loads */
      asmop dir;
      memset (&dir, 0, sizeof (dir));
      dir.type = AOP_DIR;
      dir.size = size;
      char buf[32];
      if (left->aop->type == AOP_LIT)
        {
          SNPRINTF (buf, sizeof (buf), "0x%04x", (unsigned) ulFromVal (left->aop->aopu.aop_lit) & 0xffff);
          dir.aopu.immd = buf;
          dir.aopu.immd_off = off;
        }
      else
        {
          dir.aopu.immd = left->aop->aopu.immd;
          dir.aopu.immd_off = left->aop->aopu.immd_off + off;
        }
      for (int i = size - 1; i >= 0; i--)
        {
          loadA (&dir, i);
          pushA ();
        }
      goto release;
    }

  ixLoadPtr (left->aop);

  if (ptype == POINTER)
    {
      for (int i = size - 1; i >= 0; i--)
        {
          emit2 ("ldax", "%d(ix)", off + i);
          cost (1, 1);
          pushA ();
        }
      goto release;
    }

  /* out of line (lib/lisa/gptrget.s) unless speed matters: the last byte
     through the offset helper, then __gptrprev walks back a byte at a
     time (push a leaves C, the space, alone) */
  if (!optimize.codeSpeed && off + size - 1 <= 127 && !(ptype == CPOINTER && left->aop->type == AOP_IMMD))
    {
      int last = off + size - 1;
      if (last)
        {
          emit2 ("ldi", "#0x%02x", last);
          cost (1, 1);
          emit2 ("jal", ptype == CPOINTER ? "__gptrcodeo" : "__gptrgeto");
        }
      else
        emit2 ("jal", ptype == CPOINTER ? "__gptrcode" : "__gptrget");
      cost (1, 12);
      pushA ();
      for (int i = size - 2; i >= 0; i--)
        {
          emit2 ("jal", "__gptrprev");
          cost (1, 12);
          pushA ();
        }
      ixInvalidate ();
      goto release;
    }

  /* code space, or a generic pointer that may point there: walk the
     ldi/ret pairs backwards.  call ix post-increments IX, so the previous
     pair is 3 words back. */
  {
    symbol *tlbl_code = regalloc_dry_run ? 0 : newiTempLabel (0);
    symbol *tlbl_done = regalloc_dry_run ? 0 : newiTempLabel (0);

    if (ptype == CPOINTER && left->aop->type == AOP_IMMD)
      {
        /* a constant object by name: ldx #sym is the word address already */
        asmop code;
        memset (&code, 0, sizeof (code));
        code.type = AOP_CODE;
        code.size = size;
        code.aopu.immd = left->aop->aopu.immd;
        code.aopu.immd_off = left->aop->aopu.immd_off + off;
        code.aopu.code = true;
        ixLoadSym (&code, 0);
        if (size - 1)
          {
            emit2 ("adx", "#%d", 2 * (size - 1));
            cost (1, 1);
          }
        for (int i = size - 1; i >= 0; i--)
          {
            emit2 ("call", "ix");
            cost (1, 4);
            pushA ();
            if (i)
              {
                emit2 ("adx", "#-3");
                cost (1, 1);
              }
          }
        ixInvalidate ();
        goto release;
      }

    if (ptype != CPOINTER)
      {
        emit2 ("txau", "");
        emit2 ("btst", "7");
        cost (2, 2);
        emitBranch ("bz", tlbl_code);
        for (int i = size - 1; i >= 0; i--)
          {
            emit2 ("ldax", "%d(ix)", off + i);
            cost (1, 1);
            pushA ();
          }
        G.stack.pushed -= size;
        emitBranch ("br", tlbl_done);
        emitLbl (tlbl_code);
      }
    ixCodePtrToWord ();
    if (off + size - 1)
      {
        emit2 ("adx", "#%d", 2 * (off + size - 1));
        cost (1, 1);
      }
    for (int i = size - 1; i >= 0; i--)
      {
        emit2 ("call", "ix");
        cost (1, 4);
        pushA ();
        if (i)
          {
            emit2 ("adx", "#-3");
            cost (1, 1);
          }
      }
    if (ptype != CPOINTER)
      emitLbl (tlbl_done);
    ixInvalidate ();
  }

release:
  freeAsmop (left);
}

/* Store a bit-field whose last byte is partial: the full bytes are plain
   stores, the last one is (old & ~mask) | ((value << bstr) & mask).
   The memory is either direct (a constant address) or i(ix). */
static void
genPointerSetBitField (operand *left, operand *right, int size, int blen, int bstr)
{
  asmop dir;
  bool direct = litDirect (left->aop, size);
  char buf[32];
  int last = size - 1;
  int bits = blen - 8 * last;
  unsigned char mask = (0xff >> (8 - bits)) << bstr;

  if (direct)
    {
      memset (&dir, 0, sizeof (dir));
      dir.type = AOP_DIR;
      dir.size = size;
      if (left->aop->type == AOP_LIT)
        {
          SNPRINTF (buf, sizeof (buf), "0x%04x", (unsigned) ulFromVal (left->aop->aopu.aop_lit) & 0xffff);
          dir.aopu.immd = buf;
          dir.aopu.immd_off = 0;
        }
      else
        {
          dir.aopu.immd = left->aop->aopu.immd;
          dir.aopu.immd_off = left->aop->aopu.immd_off;
        }
    }

  /* a value that needs IX to be read (code space, or the address of a
     stack object) goes through the stack first */
  bool prepushed = !direct && (right->aop->type == AOP_CODE || right->aop->type == AOP_STL);
  if (prepushed)
    for (int i = size - 1; i >= 0; i--)
      {
        loadA (right->aop, i);
        pushA ();
      }
  if (!direct)
    ixLoadPtr (left->aop);

  for (int i = 0; i < size; i++)
    {
      if (i < last)
        {
          if (prepushed)
            popA ();
          else
            loadA (right->aop, i);
          if (direct)
            storeA (&dir, i);
          else
            {
              emit2 ("stax", "%d(ix)", i);
              cost (1, 1);
            }
          continue;
        }

      if (right->aop->type == AOP_LIT)
        {
          unsigned char bval = (byteOfVal (right->aop->aopu.aop_lit, i) << bstr) & mask;
          if (direct)
            loadA (&dir, i);
          else
            {
              emit2 ("ldax", "%d(ix)", i);
              cost (1, 1);
            }
          emit2 ("andi", "#0x%02x", ~mask & 0xff);
          cost (1, 1);
          if (bval)
            {
              /* the field bits are now zero: adding is an or */
              emit2 ("ldc", "#0");
              emit2 ("adc", "#0x%02x", bval);
              cost (2, 2);
            }
        }
      else
        {
          if (prepushed)
            popA ();
          else
            loadA (right->aop, i);
          for (int j = 0; j < bstr; j++)
            {
              emit2 ("shl", "");
              cost (1, 1);
            }
          emit2 ("andi", "#0x%02x", mask);
          cost (1, 1);
          pushA ();
          if (direct)
            loadA (&dir, i);
          else
            {
              emit2 ("ldax", "%d(ix)", i);
              cost (1, 1);
            }
          emit2 ("andi", "#0x%02x", ~mask & 0xff);
          emit2 ("or", "1(sp)");
          cost (2, 2);
          adjustStack (1);
        }
      if (direct)
        storeA (&dir, i);
      else
        {
          emit2 ("stax", "%d(ix)", i);
          cost (1, 1);
        }
    }
  if (direct)
    ixNoteStore (&dir, 0);
  else
    ixInvalidate ();
}

/*-----------------------------------------------------------------*/
/* genPointerSet - stores the value into a pointer                 */
/*-----------------------------------------------------------------*/
static void
genPointerSet (iCode *ic)
{
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int size;

  D (emit2 ("; genPointerSet", ""));

  aopOp (left, ic);
  aopOp (right, ic);

  size = right->aop->size;
  wassert (operandType (left)->next);
  bool bit_field = IS_BITVAR (operandType (left)->next);
  int blen = 0, bstr = 0;
  if (bit_field)
    {
      sym_link *btype = IS_BITVAR (getSpec (operandType (right))) ? getSpec (operandType (right)) : getSpec (operandType (left)->next);
      blen = SPEC_BLEN (btype);
      bstr = SPEC_BSTR (btype);
      size = (blen + 7) / 8;
      wassertl (size == 1 || !bstr, "Multi-byte bit-field not byte-aligned");
    }

  int ptype = ptrType (left);
  if (ptype == CPOINTER)
    {
      /* code space is read-only: undefined behaviour, store nothing */
      if (!regalloc_dry_run)
        werror (W_CONTINUE, "lisa: store through a pointer to code space has no effect");
      goto release;
    }

  if (bit_field && blen % 8)
    {
      genPointerSetBitField (left, right, size, blen, bstr);
      goto release;
    }

  /* constant address: direct store */
  if (litDirect (left->aop, size))
    {
      asmop dir;
      memset (&dir, 0, sizeof (dir));
      dir.type = AOP_DIR;
      dir.size = size;
      char buf[32];
      if (left->aop->type == AOP_LIT)
        {
          SNPRINTF (buf, sizeof (buf), "0x%04x", (unsigned) ulFromVal (left->aop->aopu.aop_lit) & 0xffff);
          dir.aopu.immd = buf;
          dir.aopu.immd_off = 0;
        }
      else
        {
          dir.aopu.immd = left->aop->aopu.immd;
          dir.aopu.immd_off = left->aop->aopu.immd_off;
        }
      for (int i = 0; i < size; i++)
        cheapMove (&dir, i, right->aop, i);
      ixNoteStore (&dir, 0);
      goto release;
    }

  /* the pointer in IX; the value may need IX too (code space reads) - then copy it to the stack first */
  if (right->aop->type == AOP_CODE || right->aop->type == AOP_STL)
    {
      for (int i = size - 1; i >= 0; i--)
        {
          loadA (right->aop, i);
          pushA ();
        }
      ixLoadPtr (left->aop);
      for (int i = 0; i < size; i++)
        {
          popA ();
          emit2 ("stax", "%d(ix)", i);
          cost (1, 1);
        }
      ixInvalidate ();
      goto release;
    }

  /* the value in A: keep it while the pointer goes through A into IX */
  if (aopInReg (right->aop, 0, A_IDX) && ixLoadPtrClobbersA (left->aop))
    {
      pushA ();
      ixLoadPtr (left->aop);
      popA ();
    }
  else
    ixLoadPtr (left->aop);
  for (int i = 0; i < size; i++)
    {
      loadA (right->aop, i);
      emit2 ("stax", "%d(ix)", i);
      cost (1, 1);
    }
  /* the store may have hit whatever IX was loaded from */
  if (G.ix.type == AOP_STK || G.ix.type == AOP_DIR)
    ixInvalidate ();

release:
  freeAsmop (right);
  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genAddrOf - generates code for address of                       */
/*-----------------------------------------------------------------*/
static void
genAddrOf (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  const symbol *sym;

  D (emit2 ("; genAddrOf", ""));

  wassert (IS_TRUE_SYMOP (left));
  sym = OP_SYMBOL_CONST (left);

  aopOp (result, ic);

  int off = right ? (int) operandLitValue (right) : 0;

  if (sym->onStack)
    {
      asmop stl;
      memset (&stl, 0, sizeof (stl));
      stl.type = AOP_STL;
      stl.size = 2;
      stl.aopu.stk_off = (sym->stack >= 0 ? sym->stack + G.stack.param_offset : sym->stack + 1 + G.stack.locals_base) + off;
      /* both bytes come out of one spix/txa/txau sequence */
      if (result->aop->type == AOP_STK || result->aop->type == AOP_DIR)
        {
          int n = stl.aopu.stk_off + G.stack.pushed;
          emit2 ("spix", "");
          emit2 ("txa", "");
          emit2 ("ldc", "#0");
          emit2 ("adc", "#0x%02x", n & 0xff);
          cost (4, 4);
          storeA (result->aop, 0);
          emit2 ("txau", "");
          emit2 ("andi", "#0x7f");
          emit2 ("adc", "#0x%02x", (n >> 8) & 0xff);
          cost (3, 3);
          storeA (result->aop, 1);
          ixInvalidate ();
        }
      else
        genMove (result->aop, &stl);
    }
  else
    {
      asmop immd;
      memset (&immd, 0, sizeof (immd));
      immd.type = AOP_IMMD;
      immd.size = 2;
      immd.aopu.immd = sym->rname;
      immd.aopu.immd_off = off;
      immd.aopu.code = IN_CODESPACE (SPEC_OCLS (sym->etype));
      immd.aopu.func = IS_FUNC (sym->type);
      genMove (result->aop, &immd);
    }

  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genCast - generate code for casting                             */
/*-----------------------------------------------------------------*/
static void
genCast (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *right = IC_RIGHT (ic);
  sym_link *rtype = operandType (right);
  sym_link *ctype = operandType (IC_LEFT (ic));

  D (emit2 ("; genCast", ""));

  aopOp (right, ic);
  aopOp (result, ic);

  /* to bool: result = (right != 0) */
  if (IS_BOOL (ctype) || IS_BOOL (operandType (result)))
    {
      loadTruth (right);
      emit2 ("ldac", "ne");
      cost (1, 1);
      storeA (result->aop, 0);
      for (int i = 1; i < result->aop->size; i++)
        {
          emit2 ("ldi", "#0x00");
          cost (1, 1);
          storeA (result->aop, i);
        }
      goto release;
    }

  if (result->aop->size <= right->aop->size)
    {
      genMove_o (result->aop, 0, right->aop, 0, result->aop->size);
      /* into a _BitInt with padding bits from a wider type: mask, and
         sign-fill the padding of a signed one */
      sym_link *restype = operandType (result);
      if (IS_BITINT (restype) && (SPEC_BITINTWIDTH (restype) % 8) && bitsForType (restype) < bitsForType (rtype))
        fixBitIntResult (ic, true);
      goto release;
    }

  /* widening */
  {
    bool sign = !IS_PTR (rtype) && IS_SPEC (rtype) && !SPEC_USIGN (rtype) && !IS_BOOL (rtype);
    int rsize = right->aop->size;
    genMove_o (result->aop, 0, right->aop, 0, rsize);
    if (sign)
      {
        loadA (right->aop, rsize - 1);
        emit2 ("shl", "");
        emit2 ("ifte", "c");
        emit2 ("ldi", "#0xff");
        emit2 ("ldi", "#0x00");
        cost (4, 4);
      }
    else
      {
        emit2 ("ldi", "#0x00");
        cost (1, 1);
      }
    for (int i = rsize; i < result->aop->size; i++)
      storeA (result->aop, i);
    /* a signed value widened into an unsigned _BitInt: clear the padding */
    if (sign)
      fixBitIntResult (ic, false);
  }

release:
  freeAsmop (right);
  freeAsmop (result);
}

/*-----------------------------------------------------------------*/
/* genJumpTab - generate code for jump table                       */
/*-----------------------------------------------------------------*/
static void
genJumpTab (const iCode *ic)
{
  operand *cond = IC_JTCOND (ic);
  symbol *tlbl = regalloc_dry_run ? 0 : newiTempLabel (0);

  D (emit2 ("; genJumpTab", ""));

  aopOp (cond, ic);

  loadA (cond->aop, 0);
  pushA ();
  if (!regalloc_dry_run)
    emit2 ("ldx", "#!tlabel", labelKey2num (tlbl->key));
  cost (2, 2);
  popA ();
  emit2 ("addax", "");
  emit2 ("jmp", "ix");
  cost (2, 3);
  ixInvalidate ();

  emitLbl (tlbl);
  for (symbol *jtab = setFirstItem (IC_JTLABELS (ic)); jtab; jtab = setNextItem (IC_JTLABELS (ic)))
    emitBranch ("br", jtab);

  freeAsmop (cond);
}

/*-----------------------------------------------------------------*/
/* genGetByte - extract a byte                                     */
/*-----------------------------------------------------------------*/
static void
genGetByte (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int offset;

  D (emit2 ("; genGetByte", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  offset = (int) ulFromVal (right->aop->aopu.aop_lit) / 8;
  cheapMove (result->aop, 0, left->aop, offset);

  freeAsmop (result);
  freeAsmop (right);
  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genGetWord - extract a word (a right shift by a multiple of 8   */
/* of a long, SDCCast's optimizeGetWord)                           */
/*-----------------------------------------------------------------*/
static void
genGetWord (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  int offset;

  D (emit2 ("; genGetWord", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  offset = (int) ulFromVal (right->aop->aopu.aop_lit) / 8;
  /* low byte first: over the source, byte 0 is read before it is written */
  cheapMove (result->aop, 0, left->aop, offset);
  cheapMove (result->aop, 1, left->aop, offset + 1);

  freeAsmop (result);
  freeAsmop (right);
  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genRot - rotate left by the literal s of SDCCast's optimizeROT: */
/* a byte by any count, a word by 1, 8 or 15, a long by 16 (what   */
/* hasExtBitOp claims)                                             */
/*-----------------------------------------------------------------*/
static void
genRot (const iCode *ic)
{
  operand *result = IC_RESULT (ic);
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);

  D (emit2 ("; genRot", ""));

  aopOp (left, ic);
  aopOp (right, ic);
  aopOp (result, ic);

  int size = left->aop->size;
  int s = (int) ulFromVal (right->aop->aopu.aop_lit) % (size * 8);
  bool same = aopSame (result->aop, 0, left->aop, 0, size);

  if (size == 1)
    {
      /* shl / shr rotate through C (amode 1).  Left by s: C cleared
         once, then shl and the carry added at the bottom s times (adc
         #0 never carries, bit 0 is clear).  Right by r: a shr for C =
         bit 0 and a shr of the value again, r times.  The shorter one. */
      int r = 8 - s;
      loadA (left->aop, 0);
      if (1 + 2 * s <= 4 * r)
        {
          emit2 ("ldc", "#0");
          cost (1, 1);
          for (int i = 0; i < s; i++)
            {
              emit2 ("shl", "");
              emit2 ("adc", "#0x00");
              cost (2, 2);
            }
        }
      else
        for (int i = 0; i < r; i++)
          {
            pushA ();
            emit2 ("shr", "");
            cost (1, 1);
            popA ();
            emit2 ("shr", "");
            cost (1, 1);
          }
      storeA (result->aop, 0);
    }
  else if (size == 2 && s == 1)
    {
      /* the low byte's top bit through C into the high byte, the high
         byte's into the low byte's bottom */
      emit2 ("ldc", "#0");
      cost (1, 1);
      loadA (left->aop, 0);
      emit2 ("shl", "");
      cost (1, 1);
      storeA (result->aop, 0);
      loadA (left->aop, 1);
      emit2 ("shl", "");
      cost (1, 1);
      storeA (result->aop, 1);
      loadA (result->aop, 0);
      emit2 ("adc", "#0x00");
      cost (1, 1);
      storeA (result->aop, 0);
    }
  else if (size == 2 && s == 15)
    {
      /* right by 1: C = the low byte's bit 0 (the shifted A is dropped),
         then the high byte and the low byte shifted right through it */
      loadA (left->aop, 0);
      emit2 ("shr", "");
      cost (1, 1);
      loadA (left->aop, 1);
      emit2 ("shr", "");
      cost (1, 1);
      storeA (result->aop, 1);
      loadA (left->aop, 0);
      emit2 ("shr", "");
      cost (1, 1);
      storeA (result->aop, 0);
    }
  else
    {
      /* by half the width: the halves swapped (through the stack when
         the result is over the source) */
      int half = size / 2;
      wassertl (s == half * 8, "genRot: unsupported rotation");
      if (same)
        {
          for (int i = 0; i < half; i++)
            {
              loadA (left->aop, i);
              pushA ();
            }
          for (int i = 0; i < half; i++)
            cheapMove (result->aop, i, left->aop, half + i);
          for (int i = half - 1; i >= 0; i--)
            {
              popA ();
              storeA (result->aop, half + i);
            }
        }
      else
        for (int i = 0; i < half; i++)
          {
            cheapMove (result->aop, i, left->aop, half + i);
            cheapMove (result->aop, half + i, left->aop, i);
          }
    }

  freeAsmop (result);
  freeAsmop (right);
  freeAsmop (left);
}

/*-----------------------------------------------------------------*/
/* genDummyRead - generate code for dummy read of volatiles        */
/*-----------------------------------------------------------------*/
static void
genDummyRead (const iCode *ic)
{
  operand *op;

  D (emit2 ("; genDummyRead", ""));

  if ((op = IC_RIGHT (ic)) && IS_SYMOP (op))
    {
      aopOp (op, ic);
      for (int i = 0; i < op->aop->size; i++)
        loadA (op->aop, i);
      freeAsmop (op);
    }

  if ((op = IC_LEFT (ic)) && IS_SYMOP (op))
    {
      aopOp (op, ic);
      for (int i = 0; i < op->aop->size; i++)
        loadA (op->aop, i);
      freeAsmop (op);
    }
}

/*-----------------------------------------------------------------*/
/* genCritical / genEndCritical - interrupt disable / enable       */
/*-----------------------------------------------------------------*/
static void
genCritical (const iCode *ic)
{
  D (emit2 ("; genCritical", ""));
  emit2 ("eidi", "0");
  cost (1, 1);
}

static void
genEndCritical (const iCode *ic)
{
  D (emit2 ("; genEndCritical", ""));
  emit2 ("eidi", "1");
  cost (1, 1);
}

/*-----------------------------------------------------------------*/
/* resultRemat - result is rematerializable                        */
/*-----------------------------------------------------------------*/
static bool
resultRemat (const iCode *ic)
{
  if (SKIP_IC (ic) || ic->op == IFX)
    return 0;

  if (IC_RESULT (ic) && IS_ITEMP (IC_RESULT (ic)))
    {
      const symbol *sym = OP_SYMBOL_CONST (IC_RESULT (ic));

      if (!sym->remat)
        return(false);

      bool completely_spilt = true;
      for (unsigned int i = 0; i < getSize (sym->type); i++)
        if (sym->regs[i])
          completely_spilt = false;

      if (completely_spilt)
        return(true);
    }

  return (false);
}

/*-----------------------------------------------------------------*/
/* A as a register.  The generators take their operands from memory  */
/* and use A as scratch: each loads its first operand with loadA (a  */
/* no-op for a byte that is in A already) and stores its result with */
/* storeA (likewise).  genNativeA says whether that is enough for    */
/* the operand placement of an iCode, including whether A has to     */
/* come out unchanged (the allocator keeps another live byte in it); */
/* where it is not, genLisaiCode parks A on the stack around the     */
/* generator and lets it work on that stack byte instead.            */
/*-----------------------------------------------------------------*/

/* The operands of an iCode that could be a one-byte temporary in A. */
static void
icOperands (const iCode *ic, operand **left, operand **right, operand **result)
{
  *left = *right = *result = NULL;
  switch (ic->op)
    {
    case IFX:
      *left = IC_COND (ic);
      break;
    case JUMPTABLE:
      *left = IC_JTCOND (ic);
      break;
    case LABEL:
    case GOTO:
    case FUNCTION:
    case ENDFUNCTION:
    case INLINEASM:
    case CRITICAL:
    case ENDCRITICAL:
      break;
    case RETURN:
    case IPUSH:
    case IPUSH_VALUE_AT_ADDRESS:
    case SET_VALUE_AT_ADDRESS:
    case DUMMY_READ_VOLATILE:
      *left = IC_LEFT (ic);
      *right = IC_RIGHT (ic);
      break;
    default:
      *left = IC_LEFT (ic);
      *right = IC_RIGHT (ic);
      *result = IC_RESULT (ic);
    }
}

static bool
genNativeA (const iCode *ic)
{
  operand *left, *right, *result;
  icOperands (ic, &left, &right, &result);
  bool l = opInA (left), r = opInA (right);
  bool surv = !regDead (A_IDX, ic);    /* A must come out unchanged */

  switch (ic->op)
    {
    /* no scratch use of A, or the branch / call / push sequences that
       cannot be wrapped: the allocator only places a byte in A where
       these generators can take it */
    case LABEL:
    case GOTO:
    case FUNCTION:
    case ENDFUNCTION:
    case INLINEASM:
    case CRITICAL:
    case ENDCRITICAL:
    case IFX:
    case JUMPTABLE:
    case RETURN:
    case IPUSH:
    case IPUSH_VALUE_AT_ADDRESS:
    case CALL:
    case PCALL:
      return (true);

    case '=':
      /* the byte in A goes out first; the zero extension of a wider result clobbers A */
      return (!surv || r && getSize (operandType (result)) == 1);

    case CAST:
      {
        sym_link *restype = operandType (result);
        bool clobbers = IS_BOOL (operandType (IC_LEFT (ic))) || IS_BOOL (restype) ||
                        getSize (restype) > getSize (operandType (right)) ||
                        IS_BITINT (restype) && (SPEC_BITINTWIDTH (restype) % 8);
        return (!surv || r && !clobbers);
      }

    case '+':
      return (!surv && !(l && r));

    case '-':
      /* a multi-byte subtraction tests the right byte (cpi #1) before loading the left one */
      return (!surv && !(l && r) && !(l && getSize (operandType (result)) > 1 && !IS_OP_LITERAL (right)));

    case '*':
      return (!surv && (getSize (operandType (result)) == 1 ? !(l && r) : !l && !r));

    case '/':
    case '%':
      /* the dividend goes into IX / RA first; a divisor in A would be gone by then */
      return (!surv && !r);

    case '!':
      return (!surv);

    case '~':
    case UNARYMINUS:
      /* 0xff - x, x as the memory operand */
      return (!surv && !l);

    case '^':
    case '|':
    case BITWISEAND:
    case EQ_OP:
    case NE_OP:
      /* fused with an ifx: the allocator has made sure of the placement */
      return (!surv && !(l && r) || IS_ITEMP (result) && OP_SYMBOL_CONST (result)->regType == REG_CND);

    case '<':
    case '>':
      {
        if (IS_ITEMP (result) && OP_SYMBOL_CONST (result)->regType == REG_CND)
          return (true);
        if (!l && !r)
          return (!surv);
        if (surv || l && r)
          return (false);
        /* the single-byte sequences: against a literal, or unsigned */
        const operand *other = l ? right : left;
        return (getSize (operandType (left)) == 1 && getSize (operandType (right)) == 1 &&
                (IS_OP_LITERAL (other) || isUnsignedOp (left) || isUnsignedOp (right)));
      }

    case LEFT_OP:
    case RIGHT_OP:
      if (surv)
        return (false);
      if (IS_OP_LITERAL (right))
        return (!l || getSize (operandType (result)) == 1);
      /* a variable count is read into A first, then the result is shifted in place */
      return (!l && !opInA (result));

    case SET_VALUE_AT_ADDRESS:
      {
        sym_link *btype = operandType (left)->next;
        if (btype && IS_BITVAR (btype) && SPEC_BLEN (btype) % 8)
          return (!surv && !r);
        return (!surv || r);
      }

    default:
      return (!surv);
    }
}

/* The operand's byte is the one A was pushed to, at entry-relative stack offset stk. */
static void
parkOperand (operand *op, int stk)
{
  if (op->aop)
    return;
  asmop *aop = newAsmop (AOP_STK);
  aop->size = 1;
  aop->aopu.bytes[0].byteu.stk = stk;
  op->aop = aop;
}

static void genLisaiCodeOp (iCode *ic);

/*-----------------------------------------------------------------*/
/* genLisaiCode - generate code for LISA based on the iCode        */
/*-----------------------------------------------------------------*/
static void
genLisaiCode (iCode *ic)
{
  genLine.lineElement.ic = ic;

  if (resultRemat (ic))
    {
      if (!regalloc_dry_run)
        D (emit2 ("; skipping iCode since result will be rematerialized", ""));
      return;
    }

  if (ic->generated)
    {
      D (emit2 ("; skipping generated iCode", ""));
      return;
    }

  operand *left, *right, *result;
  icOperands (ic, &left, &right, &result);
  bool l = opInA (left), r = opInA (right), res = opInA (result);

  if ((l || r || res || !regDead (A_IDX, ic)) && !genNativeA (ic))
    {
      D (emit2 ("; A parked", ""));
      pushA ();
      int stk = 1 - G.stack.pushed;
      if (l)
        parkOperand (left, stk);
      if (r)
        parkOperand (right, stk);
      if (res)
        parkOperand (result, stk);
      genLisaiCodeOp (ic);
      popA ();
      return;
    }

  genLisaiCodeOp (ic);
}

static void
genLisaiCodeOp (iCode *ic)
{
  switch (ic->op)
    {
    case '!':
      genNot (ic);
      break;

    case '~':
      genCpl (ic);
      break;

    case UNARYMINUS:
      genUminus (ic);
      break;

    case IPUSH:
      genIpush (ic);
      break;

    case IPUSH_VALUE_AT_ADDRESS:
      genPointerPush (ic);
      break;

    case CALL:
    case PCALL:
      genCall (ic);
      break;

    case FUNCTION:
      genFunction (ic);
      break;

    case ENDFUNCTION:
      genEndFunction (ic);
      break;

    case RETURN:
      genReturn (ic);
      break;

    case LABEL:
      genLabel (ic);
      break;

    case GOTO:
      genGoto (ic);
      break;

    case '+':
      genPlus (ic);
      break;

    case '-':
      genMinus (ic);
      break;

    case '*':
      genMult (ic);
      break;

    case '/':
    case '%':
      genDivMod (ic);
      break;

    case '>':
    case '<':
      genCmp (ic, ifxForOp (IC_RESULT (ic), ic));
      break;

    case LE_OP:
    case GE_OP:
      wassertl (0, "Unimplemented iCode: <= / >= (should have been transformed)");
      break;

    case NE_OP:
    case EQ_OP:
      genCmpEQorNE (ic, ifxForOp (IC_RESULT (ic), ic));
      break;

    case AND_OP:
    case OR_OP:
      wassertl (0, "Unimplemented iCode: && / || (should have been transformed)");
      break;

    case '^':
      genXor (ic);
      break;

    case '|':
      genOr (ic);
      break;

    case BITWISEAND:
      genAnd (ic, ifxForOp (IC_RESULT (ic), ic));
      break;

    case INLINEASM:
      genInline (ic);
      ixInvalidate ();
      aInvalidate ();
      break;

    case GETABIT:
      wassertl (0, "Unimplemented iCode: GETABIT");
      break;

    case GETBYTE:
      genGetByte (ic);
      break;

    case GETWORD:
      genGetWord (ic);
      break;

    case ROT:
      genRot (ic);
      break;

    case LEFT_OP:
      genLeftShift (ic);
      break;

    case RIGHT_OP:
      genRightShift (ic);
      break;

    case GET_VALUE_AT_ADDRESS:
      genPointerGet (ic);
      break;

    case SET_VALUE_AT_ADDRESS:
      genPointerSet (ic);
      break;

    case '=':
      wassert (!POINTER_SET (ic));
      genAssign (ic);
      break;

    case IFX:
      genIfx (ic);
      break;

    case ADDRESS_OF:
      genAddrOf (ic);
      break;

    case JUMPTABLE:
      genJumpTab (ic);
      break;

    case CAST:
      genCast (ic);
      break;

    case RECEIVE:
      genReceive (ic);
      break;

    case SEND:
      genSend (ic);
      break;
      break;

    case DUMMY_READ_VOLATILE:
      genDummyRead (ic);
      break;

    case CRITICAL:
      genCritical (ic);
      break;

    case ENDCRITICAL:
      genEndCritical (ic);
      break;

    default:
      fprintf (stderr, "iCode op %d:\n", ic->op);
      wassertl (0, "Unknown iCode");
    }
}

/*-----------------------------------------------------------------*/
/* Branch relaxation.  br/bz/bnz reach +-1024 words.  After the       */
/* peephole optimizer the word distance of every branch to its label */
/* is computed along the line list, and the ones out of range are    */
/* rewritten into ldx #label ; jmp ix forms.                         */
/*-----------------------------------------------------------------*/

/* Size in words of one line (a relaxed branch holds several newline-separated lines). */
static int
lineWords (const lineNode *pl)
{
  const char *p;
  int words = 0;

  if (!pl->line || pl->isComment || pl->isLabel || pl->isDebug)
    return 0;
  for (p = pl->line; *p; )
    {
      const char *e = strchr (p, '\n');
      const char *q = p;
      size_t len = e ? (size_t) (e - p) : strlen (p);
      while (q < p + len && isspace ((unsigned char) *q))
        q++;
      if (q < p + len && *q != ';' && *q != '.' && !(p[len - 1] == ':' && !strchr (q, '\t')))
        {
          /* two words: ldx, lddiv, and div / rem with a stack operand */
          if (!strncmp (q, "ldx", 3) && strncmp (q, "ldxx", 4) || !strncmp (q, "lddiv", 5) ||
              (!strncmp (q, "div", 3) || !strncmp (q, "rem", 3)) && memchr (q, ',', len - (q - p)))
            words += 2;
          else
            words += 1;
        }
      p = e ? e + 1 : p + len;
    }
  return words;
}

/* The label a branch line targets, or NULL. */
static const char *
branchTarget (const lineNode *pl, const char **mnemonic_end)
{
  const char *p = pl->line;
  if (!p || pl->isComment || pl->isLabel || pl->isDebug || pl->isInline)
    return NULL;
  if (strncmp (p, "br", 2) && strncmp (p, "bz", 2) && strncmp (p, "bnz", 3))
    return NULL;
  const char *t = strchr (p, '\t');
  if (!t || strchr (p, '\n'))
    return NULL;
  *mnemonic_end = t;
  return t + 1;
}

struct labelAddr { const char *name; long addr; };

static void
relaxBranches (lineNode *head)
{
  bool changed;
  int iterations = 0;
  int nlabels = 0, maxlabels = 64;
  struct labelAddr *labels = Safe_alloc (maxlabels * sizeof (struct labelAddr));

  do
    {
      lineNode *pl;
      long addr;

      changed = false;
      nlabels = 0;
      /* label addresses in words */
      for (pl = head, addr = 0; pl; pl = pl->next)
        {
          if (pl->isLabel && pl->line)
            {
              char *lbl = Safe_strdup (pl->line);
              char *c = strchr (lbl, ':');
              if (c)
                *c = 0;
              if (nlabels == maxlabels)
                {
                  maxlabels *= 2;
                  labels = Safe_realloc (labels, maxlabels * sizeof (struct labelAddr));
                }
              labels[nlabels].name = lbl;
              labels[nlabels].addr = addr;
              nlabels++;
            }
          addr += lineWords (pl);
        }
      /* branches */
      for (pl = head, addr = 0; pl; pl = pl->next)
        {
          const char *mend;
          const char *target = branchTarget (pl, &mend);
          int w = lineWords (pl);
          if (target)
            {
              long taddr = -1;
              for (int i = 0; i < nlabels; i++)
                if (!strcmp (labels[i].name, target))
                  {
                    taddr = labels[i].addr;
                    break;
                  }
              if (taddr >= 0)
                {
                  long dist = taddr - addr;
                  if (dist < -1000 || dist > 1000)
                    {
                      struct dbuf_s dbuf;
                      symbol *skip = newiTempLabel (0);
                      char mn[8];
                      size_t ml = mend - pl->line;
                      if (ml >= sizeof (mn))
                        ml = sizeof (mn) - 1;
                      memcpy (mn, pl->line, ml);
                      mn[ml] = 0;
                      dbuf_init (&dbuf, 128);
                      if (!strcmp (mn, "br"))
                        dbuf_printf (&dbuf, "ldx\t#%s\n\tjmp\tix", target);
                      else if (!strcmp (mn, "bz"))
                        dbuf_printf (&dbuf, "bnz\t%05d$\n\tldx\t#%s\n\tjmp\tix\n%05d$:", labelKey2num (skip->key), target, labelKey2num (skip->key));
                      else if (!strcmp (mn, "bnz"))
                        dbuf_printf (&dbuf, "bz\t%05d$\n\tldx\t#%s\n\tjmp\tix\n%05d$:", labelKey2num (skip->key), target, labelKey2num (skip->key));
                      else if (!strcmp (mn, "br.p"))
                        {
                          /* predicated: invert the condition of the if before it when possible */
                          lineNode *pif = pl->prev;
                          while (pif && (pif->isComment || pif->isDebug))
                            pif = pif->prev;
                          if (pif && pif->line && !strncmp (pif->line, "if\t", 3) && condInverse (pif->line + 3))
                            {
                              char *nif = Safe_alloc (16);
                              SNPRINTF (nif, 16, "if\t%s", condInverse (pif->line + 3));
                              pif->line = nif;
                              dbuf_printf (&dbuf, "br.p\t%05d$\n\tldx\t#%s\n\tjmp\tix\n%05d$:", labelKey2num (skip->key), target, labelKey2num (skip->key));
                            }
                          else
                            {
                              symbol *take = newiTempLabel (0);
                              dbuf_printf (&dbuf, "br.p\t%05d$\n\tbr\t%05d$\n%05d$:\n\tldx\t#%s\n\tjmp\tix\n%05d$:",
                                           labelKey2num (take->key), labelKey2num (skip->key), labelKey2num (take->key), target, labelKey2num (skip->key));
                            }
                        }
                      if (dbuf_get_length (&dbuf))
                        {
                          pl->line = dbuf_detach_c_str (&dbuf);
                          changed = true;
                        }
                      else
                        dbuf_destroy (&dbuf);
                    }
                }
            }
          addr += w;
        }
    }
  while (changed && ++iterations < 8);
  Safe_free (labels);
}

/*-----------------------------------------------------------------*/
/* dryLisaiCode - the cost of an iCode for the register allocator: */
/* generate it without emitting, with the operands placed as the   */
/* allocator is considering.  The generator state is put back      */
/* afterwards (an isolated call drops arguments it never pushed).  */
/*-----------------------------------------------------------------*/
float
dryLisaiCode (iCode *ic)
{
  struct genState saved = G;

  regalloc_dry_run = true;
  regalloc_dry_run_cost_words = 0;
  regalloc_dry_run_cost_cycles = 0;

  initGenLineElement ();
  ixInvalidate ();
  aInvalidate ();

  genLisaiCode (ic);

  G = saved;
  destroy_line_list ();
  regalloc_dry_run = false;

  const unsigned int word_cost_weight = 2 << (optimize.codeSize * 3 + !optimize.codeSpeed * 3);

  return (regalloc_dry_run_cost_words * word_cost_weight + regalloc_dry_run_cost_cycles * ic->count);
}

/*-----------------------------------------------------------------*/
/* genLisaCode - generate code for LISA                            */
/*-----------------------------------------------------------------*/
void
genLisaCode (iCode *lic)
{
  int clevel = 0;
  int cblock = 0;
  int cln = 0;

  regalloc_dry_run = false;

  /* if debug information required */
  if (options.debug && currFunc && !regalloc_dry_run)
    debugFile->writeFunction (currFunc, lic);

  if (options.debug && !regalloc_dry_run)
    debugFile->writeFrameAddress (NULL, NULL, 0); /* have no idea where frame is now */

  for (iCode *ic = lic; ic; ic = ic->next)
    {
      initGenLineElement ();
      genLine.lineElement.ic = ic;

      if (ic->level != clevel || ic->block != cblock)
        {
          if (options.debug)
            debugFile->writeScope (ic);
          clevel = ic->level;
          cblock = ic->block;
        }

      if (ic->lineno && cln != ic->lineno)
        {
          if (options.debug)
            debugFile->writeCLine (ic);

          if (!options.noCcodeInAsm)
            emit2 (";", "%s: %d: %s", ic->filename, ic->lineno, printCLine (ic->filename, ic->lineno));
          cln = ic->lineno;
        }

      regalloc_dry_run_cost_words = 0;
      regalloc_dry_run_cost_cycles = 0;

      if (options.iCodeInAsm)
        {
          const char *iLine = printILine (ic);
          emit2 ("; ic:", "%d: %s", ic->key, iLine);
          dbuf_free (iLine);
        }

      genLisaiCode (ic);
    }

  if (options.debug)
    debugFile->writeFrameAddress (NULL, NULL, 0); /* have no idea where frame is now */

  /* now we are ready to call the
     peephole optimizer */
  if (!options.nopeep)
    peepHole (&genLine.lineHead);

  relaxBranches (genLine.lineHead);

  /* now do the actual printing */
  printLine (genLine.lineHead, codeOutBuf);

  /* destroy the line list */
  destroy_line_list ();
}

/*-----------------------------------------------------------------*/
/* lisaNotUsed - peephole support: is the flag "z" or "c" dead     */
/* after the line endPl?  Scans forward until something reads it   */
/* (false) or rewrites it unconditionally (true); labels, branches */
/* and calls are treated as a use.                                 */
/*-----------------------------------------------------------------*/
bool
lisaNotUsed (const char *what, lineNode *endPl, lineNode *head)
{
  bool z = !strcmp (what, "z"), c = !strcmp (what, "c");
  if (!z && !c)
    return false;

  for (lineNode *pl = endPl->next; pl; pl = pl->next)
    {
      const char *l = pl->line;
      if (pl->isComment || pl->isDebug || !l)
        continue;
      while (*l == ' ' || *l == '\t')
        l++;
      if (!*l || *l == ';')
        continue;
      if (pl->isLabel || strchr (l, ':'))
        return false;

      char op[16], arg[32];
      int n = 0;
      while (l[n] && l[n] != ' ' && l[n] != '\t' && l[n] != '.' && n < 15)
        {
          op[n] = l[n];
          n++;
        }
      op[n] = 0;
      bool predicated = (l[n] == '.');
      const char *a = l + n;
      while (*a && *a != ' ' && *a != '\t')
        a++;
      while (*a == ' ' || *a == '\t')
        a++;
      n = 0;
      while (*a && *a != ' ' && *a != '\t' && *a != ',' && *a != ';' && n < 31)
        arg[n++] = *a++;
      arg[n] = 0;

      /* control flow: give up */
      if (!strcmp (op, "br") || !strcmp (op, "jal") || !strcmp (op, "jmp") || !strcmp (op, "call") ||
          !strcmp (op, "ret") || !strcmp (op, "rets") || !strcmp (op, "brk"))
        return false;

      /* condition consumers */
      if (!strcmp (op, "bz") || !strcmp (op, "bnz") || !strcmp (op, "rz") || !strcmp (op, "rc"))
        {
          if (z && strcmp (op, "rc") || c && !strcmp (op, "rc"))
            return false;
          continue;
        }
      if (!strcmp (op, "if") || !strcmp (op, "iftt") || !strcmp (op, "ifte") || !strcmp (op, "ldac"))
        {
          bool uses_z = strcmp (arg, "c") && strcmp (arg, "nc");          /* eq/ne/z/nz and the ordered ones */
          bool uses_c = strcmp (arg, "eq") && strcmp (arg, "ne") && strcmp (arg, "z") && strcmp (arg, "nz");
          if (z && uses_z || c && uses_c)
            return false;
          if (!strcmp (op, "ldac"))
            {
              if (z && !predicated)
                return true;           /* loads A: Z rewritten */
            }
          continue;
        }
      if (!strcmp (op, "ldz"))
        {
          if (z && !strcmp (arg, "notz") || c && !strcmp (arg, "c"))
            return false;
          if (z && !predicated)
            return true;
          continue;
        }

      /* readers of C */
      if (c && (!strcmp (op, "adc") || !strcmp (op, "sub") || !strcmp (op, "savec") || !strcmp (op, "shl") ||
                !strcmp (op, "shr") || !strcmp (op, "shl16") || !strcmp (op, "shr16") || !strcmp (op, "addax") ||
                !strcmp (op, "subax") || !strcmp (op, "lddiv") || !strcmp (op, "div") || !strcmp (op, "rem")))
        return false;

      if (predicated)
        continue;                /* may not execute: no guaranteed rewrite */

      /* unconditional writers */
      if (z && (!strcmp (op, "ldi") || !strcmp (op, "lda") || !strcmp (op, "ldax") || !strcmp (op, "pop") && !strcmp (arg, "a") ||
                !strcmp (op, "txa") || !strcmp (op, "txau") || !strcmp (op, "add") || !strcmp (op, "adc") ||
                !strcmp (op, "sub") || !strcmp (op, "and") || !strcmp (op, "andi") || !strcmp (op, "or") ||
                !strcmp (op, "xor") || !strcmp (op, "mul") || !strcmp (op, "mulu") || !strcmp (op, "swap") ||
                !strcmp (op, "swapi") || !strcmp (op, "shl") || !strcmp (op, "shr") || !strcmp (op, "cpi") ||
                !strcmp (op, "cmp") || !strcmp (op, "btst") || !strcmp (op, "inx") || !strcmp (op, "dcx") ||
                !strcmp (op, "cpx") || !strcmp (op, "notz") || !strcmp (op, "tfa") || !strcmp (op, "ldirq")))
        return true;
      if (c && (!strcmp (op, "ldi") || !strcmp (op, "ldc") || !strcmp (op, "add") || !strcmp (op, "cpi") ||
                !strcmp (op, "cmp") || !strcmp (op, "inx") || !strcmp (op, "dcx") || !strcmp (op, "cpx") ||
                !strcmp (op, "restc")))
        return true;
      /* anything else: neither reads nor writes the flag */
    }
  return false;                  /* end of the function: be safe */
}

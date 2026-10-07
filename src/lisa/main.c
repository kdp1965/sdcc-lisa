/*-------------------------------------------------------------------------
  main.c - LISA (Little ISA) specific definitions.

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

   In other words, you are welcome to use, share and improve this program.
   You are forbidden to forbid anyone else to use, share and improve
   what you give them.   Help stamp out software-hoarding!
-------------------------------------------------------------------------*/

#include "common.h"
#include "dbuf_string.h"

#include "ralloc.h"
#include "gen.h"

static char lisa_defaultRules[] = {
#include "peeph.rul"
  ""
};

static char *lisa_keywords[] = {
  "at",
  "code",
  "critical",
  "data",
  "interrupt",
  "naked",
  "near",
  "reentrant",
  "sfr",
  NULL
};

static void
lisa_genAssemblerStart (FILE *of)
{
  if (!options.noOptsdccInAsm)
    fprintf (of, "\t.optsdcc -m%s\n", port->target);

  fprintf (of, "\n; default segment ordering in RAM for linker\n");
  tfprintf (of, "\t!area\n", DATA_NAME);
  tfprintf (of, "\t!area\n", OVERLAY_NAME);
  tfprintf (of, "\t!area\n", port->mem.initialized_name);
  tfprintf (of, "\t!area\n", XDATA_NAME);
  tfprintf (of, "\t!area\n", XIDATA_NAME);
  fprintf (of, "\n");
}

static void
lisa_genAssemblerEnd (FILE *of)
{
  /* declared in every module so that the linker always defines s_/l_
     for the startup copy loop; last, so that it stays behind the code */
  tfprintf (of, "\t!area\n", "FINITIALIZER (CODE,CDATA)");
}

/* The generic code uses this only as a flag (xidata/xinit exist); the
   copy FINITIALIZER -> FINITIALIZED is part of lisa_genInitStartup. */
static void
lisa_genXINIT (FILE *of)
{
}

/*
 * Vector table, at the start of HOME (which the linker puts at code
 * address 0): word 0 is the reset vector, words 1..8 the eight interrupt
 * sources (LSB = highest priority), word 9 catches a request that is not
 * one-hot.  Each slot is one jal: the core's isr_jump suppresses the RA
 * write of the first jump after an interrupt, so the handler finds the
 * interrupted function's RA intact and saves it itself (genFunction).
 * The glue has already switched to HOME.
 */
int
lisa_genIVT (struct dbuf_s *oBuf, symbol **intTable, int intCount)
{
  int i;

  dbuf_tprintf (oBuf, "\tjal\t__sdcc_gsinit_startup\n");
  for (i = 1; i <= 9; i++)
    {
      if (i < intCount && intTable[i])
        dbuf_tprintf (oBuf, "\tjal\t%s\n", intTable[i]->rname);
      else
        dbuf_tprintf (oBuf, "\trets\n");
    }
  return true;
}

/*
 * Startup: set up the stack, zero DATA, copy the initializers of the
 * initialized data from code space (INITIALIZER, ldi/ret pairs) to RAM
 * (INITIALIZED), then go to main.  The linker provides s_<area> and
 * l_<area>; two linker symbols cannot be added in one expression, so
 * the loops count down a copy of l_<area> kept on the stack.
 */
static void
lisa_genInitStartup (FILE *of)
{
  tfprintf (of, "\t!area\n", STATIC_NAME);
  fprintf (of, "__sdcc_gsinit_startup::\n");

  fprintf (of, "\tldx\t#0x%04x\n", options.stack_loc >= 0 ? options.stack_loc : 0x007f);
  fprintf (of, "\txchg\tsp\n");
  fprintf (of, "\tamode\t1\n");

  fprintf (of, "\tjal\t___sdcc_external_startup\n");
  fprintf (of, "\tcpi\t#0\n");
  fprintf (of, "\tif\tne\n");
  fprintf (of, "\tjal\t__sdcc_program_startup\n");

  /* Zero DATA: IX = s_DATA, 2(sp):1(sp) = l_DATA counting down */
  fprintf (of, "\tldi\t#>l_DATA\n");
  fprintf (of, "\tpush\ta\n");
  fprintf (of, "\tldi\t#<l_DATA\n");
  fprintf (of, "\tpush\ta\n");
  fprintf (of, "\tldx\t#s_DATA\n");
  fprintf (of, "00001$:\n");
  fprintf (of, "\tldax\t1(sp)\n");
  fprintf (of, "\tor\t2(sp)\n");
  fprintf (of, "\tbz\t00002$\n");
  fprintf (of, "\tldi\t#0\n");
  fprintf (of, "\tstax\t0(ix)\n");
  fprintf (of, "\tadx\t#1\n");
  fprintf (of, "\tcmp\t2(sp)\n");          /* TT07: the count's lines into the cache before the dcx */
  fprintf (of, "\tcmp\t1(sp)\n");
  fprintf (of, "\tdcx\t1(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tdcx\t2(sp)\n");
  fprintf (of, "\tbr\t00001$\n");
  fprintf (of, "00002$:\n");

  /* Zero FDATA the same way, reusing the count slots */
  fprintf (of, "\tldi\t#>l_FDATA\n");
  fprintf (of, "\tstax\t2(sp)\n");
  fprintf (of, "\tldi\t#<l_FDATA\n");
  fprintf (of, "\tstax\t1(sp)\n");
  fprintf (of, "\tldx\t#s_FDATA\n");
  fprintf (of, "00005$:\n");
  fprintf (of, "\tldax\t1(sp)\n");
  fprintf (of, "\tor\t2(sp)\n");
  fprintf (of, "\tbz\t00006$\n");
  fprintf (of, "\tldi\t#0\n");
  fprintf (of, "\tstax\t0(ix)\n");
  fprintf (of, "\tadx\t#1\n");
  fprintf (of, "\tcmp\t2(sp)\n");          /* TT07: the count's lines into the cache before the dcx */
  fprintf (of, "\tcmp\t1(sp)\n");
  fprintf (of, "\tdcx\t1(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tdcx\t2(sp)\n");
  fprintf (of, "\tbr\t00005$\n");
  fprintf (of, "00006$:\n");

  /* Copy INITIALIZER -> INITIALIZED: source in IX (code, call ix reads a
     byte), destination pointer in 4(sp):3(sp), count in 2(sp):1(sp). */
  fprintf (of, "\tldi\t#>s_INITIALIZED\n");
  fprintf (of, "\tpush\ta\n");
  fprintf (of, "\tldi\t#<s_INITIALIZED\n");
  fprintf (of, "\tpush\ta\n");
  fprintf (of, "\tldi\t#>l_INITIALIZED\n");
  fprintf (of, "\tstax\t4(sp)\n");
  fprintf (of, "\tldi\t#<l_INITIALIZED\n");
  fprintf (of, "\tstax\t3(sp)\n");
  fprintf (of, "\tldx\t#s_INITIALIZER\n");
  fprintf (of, "00003$:\n");
  fprintf (of, "\tldax\t3(sp)\n");
  fprintf (of, "\tor\t4(sp)\n");
  fprintf (of, "\tbz\t00004$\n");
  fprintf (of, "\tcall\tix\n");
  fprintf (of, "\tadx\t#1\n");
  fprintf (of, "\tpush\tix\n");
  fprintf (of, "\tldxx\t3(sp)\n");
  fprintf (of, "\tstax\t0(ix)\n");
  fprintf (of, "\tcmp\t4(sp)\n");          /* TT07: the pointer's lines into the cache before the inx */
  fprintf (of, "\tcmp\t3(sp)\n");
  fprintf (of, "\tinx\t3(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tinx\t4(sp)\n");
  fprintf (of, "\tpop\tix\n");
  fprintf (of, "\tcmp\t4(sp)\n");
  fprintf (of, "\tcmp\t3(sp)\n");
  fprintf (of, "\tdcx\t3(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tdcx\t4(sp)\n");
  fprintf (of, "\tbr\t00003$\n");
  fprintf (of, "00004$:\n");

  /* the same for FINITIALIZER -> FINITIALIZED, reusing the slots */
  fprintf (of, "\tldi\t#>s_FINITIALIZED\n");
  fprintf (of, "\tstax\t2(sp)\n");
  fprintf (of, "\tldi\t#<s_FINITIALIZED\n");
  fprintf (of, "\tstax\t1(sp)\n");
  fprintf (of, "\tldi\t#>l_FINITIALIZED\n");
  fprintf (of, "\tstax\t4(sp)\n");
  fprintf (of, "\tldi\t#<l_FINITIALIZED\n");
  fprintf (of, "\tstax\t3(sp)\n");
  fprintf (of, "\tldx\t#s_FINITIALIZER\n");
  fprintf (of, "00007$:\n");
  fprintf (of, "\tldax\t3(sp)\n");
  fprintf (of, "\tor\t4(sp)\n");
  fprintf (of, "\tbz\t00008$\n");
  fprintf (of, "\tcall\tix\n");
  fprintf (of, "\tadx\t#1\n");
  fprintf (of, "\tpush\tix\n");
  fprintf (of, "\tldxx\t3(sp)\n");
  fprintf (of, "\tstax\t0(ix)\n");
  fprintf (of, "\tcmp\t4(sp)\n");          /* TT07: the pointer's lines into the cache before the inx */
  fprintf (of, "\tcmp\t3(sp)\n");
  fprintf (of, "\tinx\t3(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tinx\t4(sp)\n");
  fprintf (of, "\tpop\tix\n");
  fprintf (of, "\tcmp\t4(sp)\n");
  fprintf (of, "\tcmp\t3(sp)\n");
  fprintf (of, "\tdcx\t3(sp)\n");
  fprintf (of, "\tif\tc\n");
  fprintf (of, "\tdcx\t4(sp)\n");
  fprintf (of, "\tbr\t00007$\n");
  fprintf (of, "00008$:\n");
  fprintf (of, "\tads\t#4\n");
}

static void
lisa_init (void)
{
  asm_addTree (&asm_asxxxx_mapping);
  lisa_init_asmops ();
}

static void
lisa_reset_regparm (struct sym_link *funcType)
{
}

static int
lisa_reg_parm (sym_link *l, bool reentrant)
{
  return (0);
}

/* --bf16-float: float arithmetic on the core's bfloat16 unit.  The type
   keeps its 32-bit storage and calling convention; __fsadd / __fssub /
   __fsmul / __fsdiv and the 8- and 16-bit integer conversions come from
   bf16fs.rel / bf16fsc.rel (device/lib/lisa/bf16fs.s, bf16fsc.c), linked
   as objects ahead of the libraries so that lisa.lib's generic ones are
   never looked for, and round every result to bf16 (8-bit mantissa).
   __SDCC_BF16_FLOAT is defined for the source. */
#define OPTION_BF16_FLOAT "--bf16-float"

/* --tt07-cache: the data space is the 32K SRAM behind the TT07 data cache,
   which folds bit 14 of the address (lisa_isa.md, data_cache8.v): X and
   X ^ 0x4000 share storage, so 16K is usable.  The stack goes to the top
   of the upper half (--stack-loc 0x7fff unless given), with --stack-size
   bytes (2K unless given) reserved, and the data areas must end below
   the stack's alias in the lower half: the linker gets that limit as the
   data RAM size (-X) and refuses a layout beyond it. */
#define OPTION_TT07_CACHE "--tt07-cache"

#define OPTION_STACK_SIZE "--stack-size"

static OPTION lisa_options[] = {
  {0, OPTION_BF16_FLOAT, NULL, "float arithmetic on the bfloat16 unit (8-bit mantissa, see README-lisa.md)"},
  {0, OPTION_TT07_CACHE, NULL, "data in the 32K SRAM behind the TT07 data cache: stack at 0x7fff, data limited to 16K minus --stack-size"},
  {0, OPTION_STACK_SIZE, NULL, "<nnnn> bytes reserved for the stack (--tt07-cache: 2K unless given)"},
  {0, NULL}
};

static bool lisa_bf16_float = false;
static bool lisa_tt07_cache = false;

static bool
lisa_parseOptions (int *pargc, char **argv, int *i)
{
  if (!strcmp (argv[*i], OPTION_BF16_FLOAT))
    {
      lisa_bf16_float = true;
      return TRUE;
    }
  if (!strcmp (argv[*i], OPTION_TT07_CACHE))
    {
      lisa_tt07_cache = true;
      return TRUE;
    }
  if (!strncmp (argv[*i], OPTION_STACK_SIZE, strlen (OPTION_STACK_SIZE)))
    {
      const char *v = argv[*i] + strlen (OPTION_STACK_SIZE);
      if (*v == '=')
        v++;
      else if (!*v && *i + 1 < *pargc)
        v = argv[++*i];
      else
        return FALSE;
      options.stack_size = (int) strtol (v, NULL, 0);
      return TRUE;
    }
  return FALSE;
}

static void
lisa_finaliseOptions (void)
{
  port->mem.default_local_map = data;
  port->mem.default_globl_map = data;
  if (lisa_tt07_cache)
    {
      if (options.stack_loc < 0)
        options.stack_loc = 0x7fff;
      if (!options.stack_size)
        options.stack_size = 0x800;
      if (!options.xram_size_set)
        {
          /* the stack's alias in the lower half is where the data must stop */
          int limit = (options.stack_loc & 0x3fff) - options.stack_size + 1;
          if (limit > 0x4000)
            limit = 0x4000;
          if (limit <= 0)
            {
              fprintf (stderr, "--tt07-cache: the stack reserve 0x%x does not fit below the stack at 0x%x\n", options.stack_size, options.stack_loc);
              exit (EXIT_FAILURE);
            }
          options.xram_size = limit;
          options.xram_size_set = 1;
        }
    }
  if (lisa_bf16_float)
    {
      addSet (&preArgvSet, Safe_strdup ("-D__SDCC_BF16_FLOAT"));
      if (!options.nostdlib)
        {
          /* the objects, from the first library directory that has them */
          static const char *const objs[] = { "bf16fs.rel", "bf16fsc.rel", NULL };
          const char *dir;
          bool found = false;
          for (dir = setFirstItem (libDirsSet); dir && !found; dir = setNextItem (libDirsSet))
            {
              struct dbuf_s dbuf;
              dbuf_init (&dbuf, PATH_MAX);
              dbuf_printf (&dbuf, "%s%c%s", dir, DIR_SEPARATOR_CHAR, objs[0]);
              if (pathExists (dbuf_c_str (&dbuf)))
                {
                  for (const char *const *o = objs; *o; o++)
                    {
                      struct dbuf_s dbuf2;
                      dbuf_init (&dbuf2, PATH_MAX);
                      dbuf_printf (&dbuf2, "%s%c%s", dir, DIR_SEPARATOR_CHAR, *o);
                      addSet (&relFilesSet, dbuf_detach_c_str (&dbuf2));
                    }
                  found = true;
                }
              dbuf_destroy (&dbuf);
            }
          if (!found)
            addSet (&libFilesSet, Safe_strdup ("bf16float"));
        }
    }
}

static void
lisa_setDefaultOptions (void)
{
  options.out_fmt = 'i';        /* Default output format is ihx */
  options.data_loc = 0x0000;
  options.xdata_loc = 0;        /* FDATA follows the DATA areas (0: linker places it) */
  options.code_loc = 0x0000;
  options.stack_loc = -1;       /* default: below the end of the DATA areas (s_SSEG) */
  options.nopeep = 0;
  options.stackAuto = 1;        /* everything lives on the stack */
}

static const char *
lisa_getRegName (const struct reg_info *reg)
{
  if (reg)
    return reg->name;
  return "err";
}

static bool
_hasNativeMulFor (iCode *ic, sym_link *left, sym_link *right)
{
  int result_size = IS_SYMOP (IC_RESULT (ic)) ? getSize (OP_SYM_TYPE (IC_RESULT (ic))) : 4;

  if (IS_BITINT (OP_SYM_TYPE (IC_RESULT (ic))) && SPEC_BITINTWIDTH (OP_SYM_TYPE (IC_RESULT (ic))) % 8)
    return false;

  /* the hardware divider does a 16-bit unsigned division (amode[1], the
     signed mode, stays off: it cannot be restored by an interrupt
     handler); the signed helpers in the library work on magnitudes */
  if (ic->op == '/' || ic->op == '%')
    {
      if (result_size > 2 || getSize (left) > 2 || getSize (right) > 2 || !IS_SPEC (left) || !IS_SPEC (right))
        return (false);
      /* two bytes with a byte result: genDivMod calls the library's
         byte-returning helpers for the signed and mixed pairs (no
         return slot); otherwise signed means the int-returning ones */
      if (!SPEC_USIGN (left) || !SPEC_USIGN (right))
        return (result_size == 1 && getSize (left) == 1 && getSize (right) == 1);
      /* a 16-bit result's high byte comes from RA, 15 bits: a quotient
         by 1 (the TT07 silicon leaves its high byte 0 there) and a
         remainder of 0x8000 or more (by a divisor above 0x8000) cannot
         be read from it, so with a divisor that could be either - not a
         literal, nor a byte for the remainder - the library's __divuint
         / __moduint do it, with the checks */
      if (result_size == 2 && getSize (left) == 2)
        {
          if (IS_OP_LITERAL (IC_RIGHT (ic)))
            return (ic->op == '/' || operandLitValue (IC_RIGHT (ic)) < 0x8000);
          return (ic->op == '%' && getSize (right) == 1);
        }
      return (true);
    }

  if (ic->op != '*')
    return (false);

  /* mul / mulu give an 8x8 -> 16 product in one instruction each; a
     16-bit product of two 8-bit operands needs them to agree in
     signedness (otherwise one of them must be extended first) */
  if (result_size > 2 || getSize (left) > 2 || getSize (right) > 2)
    return (false);
  if (result_size == 2 && getSize (left) == 1 && getSize (right) == 1 &&
      !!SPEC_USIGN (left) != !!SPEC_USIGN (right))
    return (false);
  return (true);
}

/* Indicate which extended bit operations this backend supports */
static bool
hasExtBitOp (int op, sym_link *left, int right)
{
  int size = getSize (left);

  switch (op)
    {
    case GETBYTE:
    case GETWORD:
      return (true);
    case ROT:
      /* genRot: a byte by any count (shl / shr rotate through C), a
         word by 1, 8 or 15, a long by 16 (the halves swapped) */
      if (bitsForType (left) % 8)
        return (false);
      if (size == 1)
        return (true);
      if (size == 2)
        return (right % 16 == 1 || right % 16 == 8 || right % 16 == 15);
      if (size == 4)
        return (right % 32 == 16);
      return (false);
    }
  return (false);
}

static const char *
get_model (void)
{
  return "lisa";
}

/** $1 is always the basename.
    $2 is always the output file.
    $3 varies
    $l is the list of extra options that should be there somewhere...
    $L is the list of extra options that should be passed on the command line...
    MUST be terminated with a NULL.
*/
static const char *_linkCmd[] =
{
  "sdldlisa", "-nf", "\"$1\"", "$L", NULL
};

/* $3 is replaced by assembler.debug_opts resp. port->assembler.plain_opts */
static const char *lisaAsmCmd[] =
{
  "sdaslisa", "$l", "$3", "\"$1.asm\"", NULL
};

static const char *const _libs_lisa[] = { "lisa", NULL, };

PORT lisa_port =
{
  TARGET_ID_LISA,
  "lisa",
  "LISA (Little ISA)",           /* Target name */
  0,                             /* Processor name */
  {
    glue,
    true,
    NO_MODEL,
    NO_MODEL,
    &get_model,
  },
  {                             /* Assembler */
    lisaAsmCmd,
    0,
    "-plosgffwy",               /* Options with debug */
    "-plosgffw",                /* Options without debug */
    0,
    ".asm"
  },
  {                             /* Linker */
    _linkCmd,
    0,                          //LINKCMD,
    0,
    ".rel",
    1,
    0,                          /* crt */
    _libs_lisa,                 /* libs */
  },
  {                             /* Peephole optimizer */
    lisa_defaultRules,
    0,
    0,
    0,
    0,
    lisaNotUsed,
    0,
    0,
    0,
  },
  /* Sizes: char, short, int, long, long long, ptr, fptr, gptr, bit, float, max */
  {
    1,                          /* char */
    2,                          /* short */
    2,                          /* int */
    4,                          /* long */
    8,                          /* long long */
    2,                          /* near ptr */
    2,                          /* far ptr */
    2,                          /* generic ptr */
    2,                          /* func ptr */
    0,                          /* banked func ptr */
    1,                          /* bit */
    4,                          /* float */
    64,                         /* bit-precise integer types up to _BitInt (64) */
  },
  /* tags for generic pointers: bit 15 of the pointer is set for code space */
  { 0x00, 0x00, 0x00, 0x80 },   /* far, near, xstack, code */
  {
    "XSEG",
    "SSEG",
    "CODE (CODE)",              /* code */
    "DATA",                     /* data */
    NULL,                       /* idata */
    NULL,                       /* pdata */
    "FDATA",                    /* xdata: data beyond the 9-bit direct range, through IX */
    NULL,                       /* bit */
    "RSEG (ABS)",               /* reg */
    "GSINIT (CODE)",            /* static initialization */
    "OSEG (OVR,DATA)",          /* overlay */
    "GSFINAL (CODE)",           /* gsfinal */
    "HOME (CODE)",              /* home */
    "FINITIALIZED",             /* xidata: big initialized objects, through IX */
    "FINITIALIZER (CODE,CDATA)", /* xinit: their initial values in code space */
    "CONST (CODE,CDATA)",       /* const_name */
    "CABS (ABS,CODE,CDATA)",    /* cabs_name */
    "DABS (ABS)",               /* xabs_name */
    0,                          /* iabs_name */
    "INITIALIZED",              /* name of segment for initialized variables */
    "INITIALIZER (CODE,CDATA)", /* name of segment for copies of initialized variables in code space */
    0,
    0,
    1,                          /* CODE  is read-only */
    false,                      /* unqualified pointers cannot point to __sfr */
    1                           /* No fancy alignments supported. */
  },
  { 0, 0 },
  0,                            /* ABI revision */
  {                             /* stack information */
    -1,                         /* direction: stack grows down */
     0,
     0,                         /* isr overhead */
     0,                         /* call overhead: the return address stays in RA */
     0,
     0,
     1,                         /* sp points to next free stack location */
  },
  { -1, false, false },         /* Neither int x int -> long nor unsigned long x unsigned char -> unsigned long long multiplication support routine. */
  { lisa_emitDebuggerSymbol,
    {
      0,
      0,                        /* cfiSame */
      0,                        /* cfiUndef */
      0,                        /* addressSize */
      0,                        /* regNumRet */
      0,                        /* regNumSP */
      0,                        /* regNumBP */
      0,                        /* offsetSP */
    },
  },
  {
    256,                        /* maxCount */
    1,                          /* sizeofElement */
    {2, 0, 0},                  /* sizeofMatchJump[] - jump tables only for 8-bit operands */
    {4, 0, 0},                  /* sizeofRangeCompare[] */
    1,                          /* sizeofSubtract */
    2,                          /* sizeofDispatch */
  },
  "_",
  lisa_init,
  lisa_parseOptions,
  lisa_options,
  0,
  lisa_finaliseOptions,         /* finaliseOptions */
  lisa_setDefaultOptions,       /* setDefaultOptions */
  lisa_assignRegisters,
  lisa_getRegName,
  0,
  0,
  lisa_keywords,
  lisa_genAssemblerStart,
  lisa_genAssemblerEnd,
  lisa_genIVT,
  lisa_genXINIT,                /* genXINIT: only a flag here, the startup copies FINITIALIZER */
  lisa_genInitStartup,          /* genInitStartup */
  lisa_reset_regparm,
  lisa_reg_parm,
  0,                            /* process_pragma */
  0,                            /* getMangledFunctionName */
  _hasNativeMulFor,             /* hasNativeMulFor */
  hasExtBitOp,                  /* hasExtBitOp */
  0,                            /* oclsExpense */
  true,                         /* use .dw for 16-bit initializers (the linker tags code addresses) */
  true,                         /* little endian */
  0,                            /* leave lt */
  0,                            /* leave gt */
  1,                            /* transform <= to ! > */
  1,                            /* transform >= to ! < */
  1,                            /* transform != to !(a == b) */
  0,                            /* leave == */
  false,                        /* Array initializer support. */
  0,                            /* no CSE cost estimation yet */
  0,                            /* builtin functions */
  GPOINTER,                     /* treat unqualified pointers as "generic" pointers */
  1,                            /* reset labelKey to 1 */
  1,                            /* globals & local statics allowed */
  1,                            /* Number of registers handled in the tree-decomposition-based register allocator in SDCCralloc.hpp */
  PORT_MAGIC
};

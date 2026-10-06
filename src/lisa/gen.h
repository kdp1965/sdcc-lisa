/*-------------------------------------------------------------------------
  gen.h - header file for code generation for LISA

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

#ifndef LISAGEN_H
#define LISAGEN_H 1

typedef enum
{
  AOP_INVALID,
  /* Is a literal */
  AOP_LIT = 1,
  /* Is in a register (only A) */
  AOP_REG,
  /* Is on the stack: ldax/stax n(sp) */
  AOP_STK,
  /* Is the address of a stack location */
  AOP_STL,
  /* Is an immediate value (the address of a symbol) */
  AOP_IMMD,
  /* Is in data space, directly addressable: lda/sta */
  AOP_DIR,
  /* Peripheral register: lda/sta with the periph bit */
  AOP_SFR,
  /* Is in code space (ldi/ret pairs, read with call ix) */
  AOP_CODE,
  /* Read undefined, discard writes */
  AOP_DUMMY,
  /* Implicit condition operand */
  AOP_CND,
}
AOP_TYPE;

/* asmop_byte: A type for the location a single byte
   of an operand can be in */
typedef struct asmop_byte
{
  bool in_reg;
  union
  {
    reg_info *reg;    /* Register this byte is in. */
    int stk;          /* Stack offset of this byte, relative to the SP at function entry. */
  } byteu;
} asmop_byte;

/* asmop: A homogenised type for all the different
   spaces an operand can be in */
typedef struct asmop
{
  AOP_TYPE type;
  short size;
  union
  {
    value *aop_lit;
    struct
      {
        char *immd;     /* symbol name (AOP_IMMD, AOP_DIR, AOP_SFR, AOP_CODE) */
        int immd_off;   /* byte offset */
        bool code;      /* in code space */
        bool func;      /* function address */
      };
    int stk_off;        /* AOP_STL: entry-SP-relative offset of the object */
    asmop_byte bytes[8];
  } aopu;
  struct valinfo valinfo;
}
asmop;

void genLisaCode (iCode *);
void lisa_emitDebuggerSymbol (const char *);
void lisa_init_asmops (void);

#endif

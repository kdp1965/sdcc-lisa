/*-------------------------------------------------------------------------
  ralloc.h - register allocation for the LISA port

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

#ifndef LISARALLOC_H
#define LISARALLOC_H 1

#include "common.h"

enum
{
  A_IDX = 0, // The accumulator
  C_IDX,     // Implicit condition operand.
  SP_IDX     // SP - for use with debug info.
};

enum
{
  REG_GPR = 2,
  REG_CND = 4,
};

typedef struct reg_info
{
  short type;                   /* REG_GPR or REG_CND */
  short rIdx;                   /* index into register table */
  char *name;                   /* name */
} reg_info;

extern reg_info lisa_regs[];

void lisa_assignRegisters (ebbIndex *);
void lisaSpillThis (symbol *sym);

#endif

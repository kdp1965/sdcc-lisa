/*-------------------------------------------------------------------------
   bf16fs.c - the float support routines on the bfloat16 unit, for
              sdcc -mlisa --bf16-float (bf16float.lib)

   A float keeps its 32-bit storage and calling convention, but every
   arithmetic result is rounded to bfloat16 (8 significant bits, the
   exponent range of a float): the operands' top halves go through the
   routines of <lisa/bf16.h> and the result comes back with a zero low
   half.  Multiplication and division are the hardware (fmul truncates,
   fdiv rounds to nearest within an ulp); addition and subtraction are
   the software bf16_add, since the core's adder is defective (see the
   header).  The 8- and 16-bit integer conversions are the hardware itof
   / ftoi.  Everything else - the comparisons, the long conversions, the
   math functions - is the generic library, exact on the 32-bit values.

   Copyright (C) 2026

   This library is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this library; see the file COPYING. If not, write to the
   Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.

   As a special exception, if you link this library with other files,
   some of which are compiled with SDCC, to produce an executable,
   this library does not by itself cause the resulting executable to
   be covered by the GNU General Public License. This exception does
   not however invalidate any other reasons why the executable file
   might be covered by the GNU General Public License.
-------------------------------------------------------------------------*/

#include <lisa/bf16.h>

union f32 { float f; unsigned long u; };

/* the top half as it is: toward zero, which is what a conversion to an
   integer wants (2.9999 must not become 3) */
static bf16_t trunc16(float f)
{
  union f32 v;
  v.f = f;
  return (bf16_t)(v.u >> 16);
}

float __fsadd(float a, float b)
{
  return bf16_to_float(bf16_add(bf16_from_float(a), bf16_from_float(b)));
}

float __fssub(float a, float b)
{
  return bf16_to_float(bf16_sub(bf16_from_float(a), bf16_from_float(b)));
}

float __fsmul(float a, float b)
{
  return bf16_to_float(bf16_mul(bf16_from_float(a), bf16_from_float(b)));
}

float __fsdiv(float a, float b)
{
  return bf16_to_float(bf16_div(bf16_from_float(a), bf16_from_float(b)));
}

float __sint2fs(signed int x)
{
  return bf16_to_float(bf16_from_int(x));
}

float __uint2fs(unsigned int x)
{
  return bf16_to_float(bf16_from_uint(x));
}

float __schar2fs(signed char x)
{
  return bf16_to_float(bf16_from_int(x));
}

float __uchar2fs(unsigned char x)
{
  return bf16_to_float(bf16_from_uint(x));
}

signed int __fs2sint(float f)
{
  return bf16_to_int(trunc16(f));
}

unsigned int __fs2uint(float f)
{
  return bf16_to_uint(trunc16(f));
}

signed char __fs2schar(float f)
{
  return (signed char)bf16_to_int(trunc16(f));
}

unsigned char __fs2uchar(float f)
{
  return (unsigned char)bf16_to_uint(trunc16(f));
}

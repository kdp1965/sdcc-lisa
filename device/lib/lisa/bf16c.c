/*-------------------------------------------------------------------------
   bf16c.c - the bfloat16 conversions that are not a hardware instruction
             (device/include/lisa/bf16.h); the rest is in bf16.s

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

/* bf16_add is bf16add.s */

/* the hardware converts unsigned: the sign goes around it */
bf16_t bf16_from_int(int x)
{
  if (x < 0)
    return bf16_neg(bf16_from_uint((unsigned int)(-x)));
  return bf16_from_uint((unsigned int)x);
}

int bf16_to_int(bf16_t a)
{
  unsigned int m = bf16_to_uint(bf16_abs(a));
  if (m > 32767u)
    m = 32767u;
  return (a & 0x8000u) ? -(int)m : (int)m;
}

/* a float is the bf16 pattern followed by 16 more fraction bits */
union f32 { float f; unsigned long u; };

bf16_t bf16_from_float(float f)
{
  union f32 v;
  v.f = f;
  /* NaN: keep it a NaN (the rounding could carry into the exponent) */
  if ((v.u & 0x7f800000ul) == 0x7f800000ul && (v.u & 0x007ffffful))
    return (bf16_t)((v.u >> 16) | 0x0040u);
  /* round to nearest, ties to even */
  v.u += 0x7ffful + ((v.u >> 16) & 1);
  return (bf16_t)(v.u >> 16);
}

float bf16_to_float(bf16_t a)
{
  union f32 v;
  v.u = (unsigned long)a << 16;
  return v.f;
}

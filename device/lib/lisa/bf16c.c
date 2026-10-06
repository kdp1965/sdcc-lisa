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

/* Addition in software.  The core's adder (fops.v fadd) has two defects
   on the TT07 silicon that cannot be corrected from its result: when the
   significands carry out, the fraction comes back shifted right by one
   bit (1.25 + 1.25 = 2.25), and when the rounding carries out of the
   fraction the mantissa wraps to 0 with the exponent unchanged (half the
   value).  This is the usual algorithm: align, add or subtract the
   significands with a sticky bit, normalize, round to nearest even.
   Denormals are treated as zero, the result flushes to zero below the
   normal range. */
bf16_t bf16_add(bf16_t a, bf16_t b)
{
  unsigned char ea = (a >> 7) & 0xff, eb = (b >> 7) & 0xff;
  unsigned int ma, mb, s;
  unsigned char d;
  int e;
  bf16_t sign;

  if (ea == 0xff)
    {
      if (eb == 0xff && (a ^ b) == 0x8000u)
        return BF16_NAN;                      /* inf - inf */
      return a;                               /* inf or nan */
    }
  if (eb == 0xff)
    return b;
  if (ea == 0)
    return eb == 0 ? (bf16_t)(a & b & 0x8000u) : b;   /* a is zero (-0 + -0 = -0) */
  if (eb == 0)
    return a;

  /* the larger magnitude first */
  if ((a & 0x7fffu) < (b & 0x7fffu))
    {
      bf16_t t = a; a = b; b = t;
      ea = (a >> 7) & 0xff; eb = (b >> 7) & 0xff;
    }
  d = ea - eb;
  if (d > 9)
    return a;                                 /* b is below half an ulp of a */
  sign = a & 0x8000u;
  e = ea;
  /* significands with the hidden bit at bit 14, 7 guard bits below */
  ma = ((a & 0x7fu) | 0x80u) << 7;
  mb = ((b & 0x7fu) | 0x80u) << 7;
  if (d)
    {
      unsigned int lost = mb & ((1u << d) - 1);
      mb >>= d;
      if (lost)
        mb |= 1;                              /* sticky */
    }
  if ((a ^ b) & 0x8000u)
    {
      s = ma - mb;
      if (s == 0)
        return BF16_ZERO;
      while (!(s & 0x4000u))
        {
          s <<= 1;
          e--;
        }
    }
  else
    {
      s = ma + mb;
      if (s & 0x8000u)
        {
          s = (s >> 1) | (s & 1);
          e++;
        }
    }
  /* round to nearest even: the fraction is bits 13..7, the guard bit 6 */
  if ((s & 0x40u) && ((s & 0x3fu) || (s & 0x80u)))
    {
      s += 0x80u;
      if (s & 0x8000u)
        {
          s >>= 1;
          e++;
        }
    }
  if (e >= 0xff)
    return sign | BF16_INF;
  if (e <= 0)
    return sign;                              /* underflow: zero */
  return sign | ((bf16_t)e << 7) | ((s >> 7) & 0x7fu);
}

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

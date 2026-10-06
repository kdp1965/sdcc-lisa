/*-------------------------------------------------------------------------
   bf16.h - the bfloat16 unit of the LISA core from C, for sdcc -mlisa

   The core has a 16-bit floating point accumulator (facc) and four
   registers (f0..f3) with add, multiply, divide, compare and integer
   conversions in hardware.  bfloat16 is the top half of an IEEE single:
   sign, 8-bit exponent, 7-bit fraction - about 2.4 significant decimal
   digits, the range of a float.

   bf16_t is the bit pattern (an unsigned short), so values can be
   stored, passed and compared for identity like integers; the arithmetic
   goes through the routines below (device/lib/lisa/bf16.s), each a few
   instructions around the hardware.  The C type float stays the 32-bit
   software one; bf16_from_float / bf16_to_float convert.

   What the TT07 silicon does (probed on the chip, 2026-10-06; lisa_sim
   models it bit for bit): fmul truncates toward zero, fdiv rounds to
   nearest (amode[2] stays 0) with a one-ulp slip now and then, fcmp
   compares the bit patterns (so -0.0 and 0.0 are not equal), itof
   rounds half up, ftoi truncates toward zero and saturates.  The adder
   is not usable: when the significands carry out it returns the
   fraction shifted one bit right (1.25 + 1.25 = 2.25), and when the
   rounding carries out of the fraction it returns half the value - so
   bf16_add / bf16_sub are done in software here (round to nearest even,
   denormals as zero), and bf16_fadd_raw is the hardware as it is.  The
   integer conversions are unsigned in the hardware (amode[1] stays 0);
   the signed ones here handle the sign around them.

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

#ifndef __LISA_BF16_H
#define __LISA_BF16_H 1

typedef unsigned short bf16_t;

/* ---- constants (bit patterns) ---------------------------------------- */
#define BF16_ZERO       0x0000u
#define BF16_NEG_ZERO   0x8000u
#define BF16_HALF       0x3f00u
#define BF16_ONE        0x3f80u
#define BF16_TWO        0x4000u
#define BF16_TEN        0x4120u
#define BF16_PI         0x4049u     /* 3.140625 */
#define BF16_E          0x402eu     /* 2.71875 */
#define BF16_MAX        0x7f7fu     /* 3.39e38 */
#define BF16_MIN_NORMAL 0x0080u     /* 1.18e-38 */
#define BF16_INF        0x7f80u
#define BF16_NEG_INF    0xff80u
#define BF16_NAN        0x7fc0u

/* ---- arithmetic ------------------------------------------------------ */
bf16_t bf16_add(bf16_t a, bf16_t b);                /* software, round to nearest even */
#define bf16_sub(a, b) bf16_add((a), bf16_neg(b))
bf16_t bf16_mul(bf16_t a, bf16_t b);                /* hardware, truncates */
bf16_t bf16_div(bf16_t a, bf16_t b);                /* hardware, nearest */
bf16_t bf16_fadd_raw(bf16_t a, bf16_t b);           /* the hardware adder, defects and all */

#define bf16_neg(a)     ((bf16_t)((a) ^ 0x8000u))
#define bf16_abs(a)     ((bf16_t)((a) & 0x7fffu))
#define bf16_signbit(a) (((a) >> 15) & 1)
#define bf16_isnan(a)   ((((a) & 0x7f80u) == 0x7f80u) && ((a) & 0x007fu))
#define bf16_isinf(a)   (((a) & 0x7fffu) == 0x7f80u)
#define bf16_iszero(a)  (((a) & 0x7fffu) == 0)

/* ---- comparison (hardware fcmp) -------------------------------------- */
unsigned char bf16_gt(bf16_t a, bf16_t b);          /* a > b */
unsigned char bf16_eq(bf16_t a, bf16_t b);          /* a == b (bit patterns) */
signed char   bf16_cmp(bf16_t a, bf16_t b);         /* -1, 0, 1 */
#define bf16_lt(a, b) bf16_gt((b), (a))
#define bf16_ge(a, b) (!bf16_gt((b), (a)))
#define bf16_le(a, b) (!bf16_gt((a), (b)))

/* ---- conversions ----------------------------------------------------- */
bf16_t       bf16_from_uint(unsigned int x);        /* itof: rounds to nearest */
bf16_t       bf16_from_int(int x);
unsigned int bf16_to_uint(bf16_t a);                /* ftoi: truncates, saturates at 65535, negatives give 0 */
int          bf16_to_int(bf16_t a);                 /* truncates, saturates at +-32767 */
bf16_t       bf16_from_float(float f);              /* rounds to nearest even */
float        bf16_to_float(bf16_t a);               /* exact */

#endif

;--------------------------------------------------------------------------
;  bf16.s - the bfloat16 unit of the LISA core from C (device/include/
;  lisa/bf16.h)
;
;  Copyright (C) 2026
;
;  This library is free software; you can redistribute it and/or modify it
;  under the terms of the GNU General Public License as published by the
;  Free Software Foundation; either version 2, or (at your option) any
;  later version.
;
;  This library is distributed in the hope that it will be useful,
;  but WITHOUT ANY WARRANTY; without even the implied warranty of
;  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
;  GNU General Public License for more details.
;
;  You should have received a copy of the GNU General Public License
;  along with this library; see the file COPYING. If not, write to the
;  Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
;  MA 02110-1301, USA.
;
;  As a special exception, if you link this library with other files,
;  some of which are compiled with SDCC, to produce an executable,
;  this library does not by itself cause the resulting executable to
;  be covered by the GNU General Public License. This exception does
;  not however invalidate any other reasons why the executable file
;  might be covered by the GNU General Public License.
;--------------------------------------------------------------------------
;
; The unit: facc (the accumulator) and f0..f3.  taf 0 / taf 1 load the
; halves of facc from A, tfa 0 / tfa 1 read them; fswap fn exchanges facc
; and fn; fadd / fmul / fdiv fn combine facc with fn into facc; fcmp fn
; sets Z (equal bit patterns) and C (facc > fn); itof / ftoi convert
; facc in place (unsigned: amode[1] stays 0).  The unit's registers are
; scratch: nothing is kept in them between calls.
;
; Frames (no RA to save: leaf functions, the unit does not touch RA or
; IX): bf16_t f(bf16_t a, bf16_t b) has its return slot at 1(sp),2(sp),
; a at 3(sp),4(sp), b at 5(sp),6(sp); unsigned char f(bf16_t a, bf16_t b)
; returns in A, so a is at 1(sp),2(sp) and b at 3(sp),4(sp).

	.area CODE (CODE)

; facc <- b, f0 <- a
.macro	LOAD2
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	fswap	f0
	ldax	5(sp)
	taf	0
	ldax	6(sp)
	taf	1
.endm

; return slot <- facc
.macro	STORE
	tfa	0
	stax	1(sp)
	tfa	1
	stax	2(sp)
.endm

	.globl _bf16_mul
_bf16_mul:
	LOAD2
	fmul	f0			; b * a
	STORE
	ret

; bf16_t bf16_fadd_raw(bf16_t a, bf16_t b): the core's adder as it is
; (bf16c.c has the software bf16_add; see the header)
	.globl _bf16_fadd_raw
_bf16_fadd_raw:
	LOAD2
	fadd	f0			; b + a
	STORE
	ret

	.globl _bf16_div
_bf16_div:
	LOAD2
	fswap	f0			; facc = a, f0 = b
	fdiv	f0			; a / b
	STORE
	ret

; unsigned char bf16_gt(bf16_t a, bf16_t b): a at 1,2(sp), b at 3,4(sp)
	.globl _bf16_gt
_bf16_gt:
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	fswap	f0			; f0 = b
	ldax	1(sp)
	taf	0
	ldax	2(sp)
	taf	1			; facc = a
	fcmp	f0			; C = a > b
	ldac	c
	ret

	.globl _bf16_eq
_bf16_eq:
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	fswap	f0
	ldax	1(sp)
	taf	0
	ldax	2(sp)
	taf	1
	fcmp	f0			; Z = a == b
	ldac	eq
	ret

; signed char bf16_cmp(bf16_t a, bf16_t b): -1, 0, 1
	.globl _bf16_cmp
_bf16_cmp:
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	fswap	f0
	ldax	1(sp)
	taf	0
	ldax	2(sp)
	taf	1
	fcmp	f0			; Z = equal, C = a > b
	ldac	eq			; A = equal; C kept
	bnz	1$			; equal: 0
	ldac	c			; A = 1 if a > b, else 0
	cpi	#0x00
	ifte	z
	ldi	#0xff			; a < b: -1
	ldi	#0x01
	ret
1$:
	ldi	#0x00
	ret

; bf16_t bf16_from_uint(unsigned int x): x at 3,4(sp)
	.globl _bf16_from_uint
_bf16_from_uint:
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	itof
	STORE
	ret

; unsigned int bf16_to_uint(bf16_t a): a at 3,4(sp)
	.globl _bf16_to_uint
_bf16_to_uint:
	ldax	3(sp)
	taf	0
	ldax	4(sp)
	taf	1
	ftoi
	STORE
	ret

;--------------------------------------------------------------------------
;  divu.s - _divuint() / _moduint() / _divsint() / _modsint() and the
;  byte helpers (_divschar() ... _modsuchar(), and the byte-returning
;  _divschar8() ... _modsuchar8()) on the hardware divider, for the
;  LISA port
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
; The compiler emits the divider inline for unsigned operands up to 16
; bits; these are for the callers it cannot inline for, and for signed
; operands.  A two-byte result comes back in IX: each entry first
; reserves a pair for it (ads #-2, before sra) where the caller's return
; slot used to be, so that the bodies below keep their frames, and the
; exit loads IX from the pair.  unsigned int f(unsigned int a, unsigned
; int b): after the pair, sra and the push of the flag byte the result
; pair is at T+3(sp),T+4(sp), a at T+5(sp),T+6(sp), b at T+7(sp),T+8(sp).  lddiv
; loads {RA[7:0], IX[7:0]} with the dividend, div / rem take the divisor
; from {T+1(sp), A} and leave the result's low byte in A and all 16 bits
; in RA (ra_cond set), which is where the high byte is read from - 15
; bits: a quotient by 1 has no high byte there (the silicon leaves 0)
; and a remainder of 0x8000 or more does not fit, so these check for
; those: on the TT07 silicon the stage-two store of the high byte never
; lands, and a div / rem whose second word has bit 1 clear (an offset of
; 0, 1, 4, 5 ...) writes the low byte to that offset's address - hence
; the divisor's high byte pushed twice, to sit at T+1(sp).  RA is saved
; around all of it.
;
; The signed entries make a and b non-negative in place, run the same
; code and negate the result once when the signs call for it (a
; quotient when they differ, a remainder when the dividend's is set);
; the flag byte at T(sp) has that in bit 7 and "remainder" in bit 0.

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl __divuint, __moduint, __divsint, __modsint

__divuint:
	ads	#-2			; the result's pair
	sra
	ldi	#0x00
	push	a			; T(sp): bit 7 negate the result, bit 0 remainder
	br	udiv16
__moduint:
	ads	#-2
	sra
	ldi	#0x01
	push	a
	br	umod16
__divsint:
	ads	#-2
	sra
	ldax	T+5(sp)			; the signs differ: a negative quotient
	xor	T+7(sp)
	andi	#0x80
	br	sdiv16
__modsint:
	ads	#-2
	sra
	ldax	T+5(sp)			; the dividend's sign is the remainder's
	andi	#0x80
	ldc	#0
	adc	#0x01
sdiv16:
	push	a
	ldax	T+6(sp)
	btst	7
	bnz	1$
	spix
	adx	#T+5
	jal	neg2			; a = -a
1$:	ldax	T+8(sp)
	btst	7
	bnz	2$
	spix
	adx	#T+7
	jal	neg2			; b = -b
2$:	ldax	T(sp)
	btst	0
	bz	umod16			; Z = bit 0
udiv16:
	ldax	T+5(sp)
	lddiv	T+6(sp)
	ldax	T+8(sp)			; b high
	push	a
	push	a			; T(sp), T+1(sp): b high; result pair 6,7; a 8,9; b 10,11
	ldax	T+9(sp)
	div	0, T+1(sp)		; A = q low, RA = q
	stax	T+5(sp)
	xchg	ra
	txau
	andi	#0x7f			; q high - except that the silicon leaves 0 there for b == 1
	push	a			; T(sp); b high T+2(sp); result pair 7,8; a 9,10; b 11,12
	ldax	T+10(sp)
	cpi	#0x01
	bnz	3$
	ldax	T+2(sp)
	cpi	#0x00
	bnz	3$
	ldax	T+9(sp)			; b == 1: q = a
	stax	T(sp)
3$:
	pop	a
	stax	T+6(sp)
	ads	#2
	br	done16
umod16:
	ldax	T+5(sp)
	lddiv	T+6(sp)
	ldax	T+8(sp)			; b high
	push	a
	push	a			; T(sp), T+1(sp): b high; result pair 6,7; a 8,9; b 10,11
	ldax	T+9(sp)
	rem	0, T+1(sp)		; A = r low, RA = r
	stax	T+5(sp)
	ldax	T+1(sp)
	btst	7			; Z = bit 7 of b high
	bz	4$			; b >= 0x8000: r may not fit the 15 bits of RA
	xchg	ra
	txau
	andi	#0x7f
	stax	T+6(sp)
	ads	#2
	br	done16
4$:
	; q = a / b is 0 or 1, so r high = a high - q * b high - (a low < r low)
	ldax	T+7(sp)
	lddiv	T+8(sp)
	ldax	T+9(sp)
	div	0, T+1(sp)		; A = q
	mul	T+1(sp)			; q * b high
	push	a			; T(sp); b high 2,3; result pair 7,8; a 9,10; b 11,12
	ldax	T+6(sp)			; r low
	cpi	#0x01			; C = (r low == 0): cmp would report a borrow against 0
	ldax	T+8(sp)			; a low
	ifte	nc
	cmp	T+6(sp)			; C = a low < r low, the borrow of the low byte
	ldc	#0
	ldax	T+9(sp)			; a high
	sub	T(sp)			; - q * b high - borrow
	stax	T+7(sp)
	ads	#3
done16:
	ldax	T(sp)
	btst	7
	bnz	5$
	spix
	adx	#T+3
	jal	neg2			; the result negated in place
5$:	ads	#1
	lra
	ldxx	T(sp)			; the result, in IX
	ads	#2
	ret

; ---- bytes ---------------------------------------------------------------
;
; signed char a / b and a % b, and the mixed ones (one operand unsigned
; - "su": a unsigned, "us": b unsigned - as C's int arithmetic has them:
; the result takes the sign of the signed operand).  SDCC has the byte
; helpers return int, since -128 / -1 is 128 and 255 / -1 is -255: the
; result is |a| / |b| (|a| and |b| taken in place, div 3 / rem 3), at
; most 255, or its 16-bit negation when the signs say so.  The unsigned
; pair is here for the shape the compiler cannot inline (a _BitInt).
; a comes in A (the first byte parameter, lisa_reg_parm), b on the
; stack, the int result goes back in IX.  Frame after the result's
; pair (ads #-2), sra, the push of a and the flag push: T(sp) the flag
; - bit 7 negate the result, bit 0 remainder; a T+1(sp); RA 3,4; the
; result pair 5,6; b T+6(sp).

	.globl __divschar, __modschar, __divuchar, __moduchar
	.globl __divsuchar, __modsuchar, __divuschar, __moduschar

__divschar:
	ads	#-2			; the result's pair
	sra
	push	a			; a T(sp); RA 2,3; pair 4,5; b T+5(sp)
	xor	T+5(sp)			; the signs differ: a negative quotient
	andi	#0x80
	push	a
	jal	absa
	jal	absb
	br	div8
__modschar:
	ads	#-2
	sra
	push	a
	andi	#0x80			; the dividend's sign is the remainder's
	ldc	#0
	adc	#0x01
	push	a
	jal	absa
	jal	absb
	br	div8
__divuschar:				; a signed, b unsigned
	ads	#-2
	sra
	push	a
	andi	#0x80
	push	a
	jal	absa
	br	div8
__moduschar:
	ads	#-2
	sra
	push	a
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa
	br	div8
__divsuchar:				; a unsigned, b signed
	ads	#-2
	sra
	push	a
	ldax	T+5(sp)
	andi	#0x80
	push	a
	jal	absb
	br	div8
__modsuchar:				; a non-negative dividend: a non-negative remainder
	ads	#-2
	sra
	push	a
	ldi	#0x01
	push	a
	jal	absb
	br	div8
__divuchar:
	ads	#-2
	sra
	push	a
	ldi	#0x00
	push	a
	br	div8
__moduchar:
	ads	#-2
	sra
	push	a
	ldi	#0x01
	push	a
div8:
	ldax	T+1(sp)
	tax				; the 8-bit dividend
	ldax	T(sp)
	btst	0
	bz	1$			; Z = bit 0: remainder
	ldax	T+6(sp)
	div	3
	br	2$
1$:	ldax	T+6(sp)
	rem	3
2$:	stax	T+4(sp)			; the result, 0..255, as an int
	ldi	#0x00
	stax	T+5(sp)
	ldax	T(sp)
	btst	7
	bnz	3$
	spix
	adx	#T+4
	jal	neg2			; negated as 16 bits
3$:	ads	#2
	lra
	ldxx	T(sp)			; the result, in IX
	ads	#2
	ret

; a = |a|, b = |b| (the frame above; a jal moves no stack)
absa:
	ldax	T+1(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	T+1(sp)
	ldc	#1
	adc	#0x00
	stax	T+1(sp)
9$:	ret
absb:
	ldax	T+6(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	T+6(sp)
	ldc	#1
	adc	#0x00
	stax	T+6(sp)
9$:	ret

; ---- bytes, a byte back in A -------------------------------------------
;
; The same when the compiler only wants a byte (genDivMod: a / b of two
; chars with a char result, a in A and b pushed, no return slot): the
; result in A, 128 is 0x80.  Frame after sra, the push of a and the
; flag push: T(sp) the flag; a T+1(sp); RA 3,4; b T+4(sp).

	.globl __divschar8, __modschar8, __divuschar8, __moduschar8
	.globl __divsuchar8, __modsuchar8

__divschar8:
	sra
	push	a			; a T(sp); RA 2,3; b T+3(sp)
	xor	T+3(sp)
	andi	#0x80
	push	a
	jal	absa8
	jal	absb8
	br	div8b
__modschar8:
	sra
	push	a
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa8
	jal	absb8
	br	div8b
__divuschar8:				; a signed, b unsigned
	sra
	push	a
	andi	#0x80
	push	a
	jal	absa8
	br	div8b
__moduschar8:
	sra
	push	a
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa8
	br	div8b
__divsuchar8:				; a unsigned, b signed
	sra
	push	a
	ldax	T+3(sp)
	andi	#0x80
	push	a
	jal	absb8
	br	div8b
__modsuchar8:				; a non-negative dividend: a non-negative remainder
	sra
	push	a
	ldi	#0x01
	push	a
	jal	absb8
div8b:
	ldax	T+1(sp)
	tax				; the 8-bit dividend
	ldax	T(sp)
	btst	0
	bz	1$			; Z = bit 0: remainder
	ldax	T+4(sp)
	div	3
	br	2$
1$:	ldax	T+4(sp)
	rem	3
2$:	stax	T+1(sp)			; the result, negated in place when the signs say so
	ldax	T(sp)
	btst	7
	bnz	3$
	ldi	#0xff
	sub	T+1(sp)
	ldc	#1
	adc	#0x00
	br	4$
3$:	ldax	T+1(sp)
4$:	ads	#2
	lra
	ret

absa8:
	ldax	T+1(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	T+1(sp)
	ldc	#1
	adc	#0x00
	stax	T+1(sp)
9$:	ret
absb8:
	ldax	T+4(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	T+4(sp)
	ldc	#1
	adc	#0x00
	stax	T+4(sp)
9$:	ret

; the two bytes at IX negated: ~x + 1 with the carry through adc #0 (ldi
; clears C for the sub, whose own C the TT07 gets wrong; stax keeps it)
neg2:
	ldi	#0xff
	sub	0(ix)
	ldc	#1
	adc	#0x00
	stax	0(ix)
	savec
	ldi	#0xff
	sub	1(ix)
	restc
	adc	#0x00
	stax	1(ix)
	ret

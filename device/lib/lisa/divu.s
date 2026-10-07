;--------------------------------------------------------------------------
;  divu.s - _divuint() / _moduint() / _divsint() / _modsint() and the
;  eight byte helpers (_divschar() ... _modsuchar()) on the hardware
;  divider, for the LISA port
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
; operands.  unsigned int f(unsigned int a, unsigned int b): after sra
; and the push of the flag byte the return slot is at 4(sp),5(sp), a at
; 6(sp),7(sp), b at 8(sp),9(sp).  lddiv loads {RA[7:0], IX[7:0]} with the
; dividend, div / rem take the divisor from {2(sp), A} and leave the
; result's low byte in A and all 16 bits in RA (ra_cond set), which is
; where the high byte is read from - 15 bits: a quotient by 1 has no
; high byte there (the silicon leaves 0) and a remainder of 0x8000 or
; more does not fit, so these check for those: on the TT07 silicon the
; stage-two store of the high byte never lands, and a div / rem whose
; second word has bit 1 clear (an offset of 0, 1, 4, 5 ...) writes the
; low byte to that offset's address - hence the divisor's high byte
; pushed twice, to sit at 2(sp).  RA is saved around all of it.
;
; The signed entries make a and b non-negative in place, run the same
; code and negate the result once when the signs call for it (a
; quotient when they differ, a remainder when the dividend's is set);
; the flag byte at 1(sp) has that in bit 7 and "remainder" in bit 0.

	.area CODE (CODE)

	.globl __divuint, __moduint, __divsint, __modsint

__divuint:
	sra
	ldi	#0x00
	push	a			; 1(sp): bit 7 negate the result, bit 0 remainder
	br	udiv16
__moduint:
	sra
	ldi	#0x01
	push	a
	br	umod16
__divsint:
	sra
	ldax	6(sp)			; the signs differ: a negative quotient
	xor	8(sp)
	andi	#0x80
	br	sdiv16
__modsint:
	sra
	ldax	6(sp)			; the dividend's sign is the remainder's
	andi	#0x80
	ldc	#0
	adc	#0x01
sdiv16:
	push	a
	ldax	7(sp)
	btst	7
	bnz	1$
	spix
	adx	#6
	jal	neg2			; a = -a
1$:	ldax	9(sp)
	btst	7
	bnz	2$
	spix
	adx	#8
	jal	neg2			; b = -b
2$:	ldax	1(sp)
	btst	0
	bz	umod16			; Z = bit 0
udiv16:
	ldax	6(sp)
	lddiv	7(sp)
	ldax	9(sp)			; b high
	push	a
	push	a			; 1(sp), 2(sp): b high; return slot 6,7; a 8,9; b 10,11
	ldax	10(sp)
	div	0, 2(sp)		; A = q low, RA = q
	stax	6(sp)
	xchg	ra
	txau
	andi	#0x7f			; q high - except that the silicon leaves 0 there for b == 1
	push	a			; 1(sp); b high 3(sp); return slot 7,8; a 9,10; b 11,12
	ldax	11(sp)
	cpi	#0x01
	bnz	3$
	ldax	3(sp)
	cpi	#0x00
	bnz	3$
	ldax	10(sp)			; b == 1: q = a
	stax	1(sp)
3$:
	pop	a
	stax	7(sp)
	ads	#2
	br	done16
umod16:
	ldax	6(sp)
	lddiv	7(sp)
	ldax	9(sp)			; b high
	push	a
	push	a			; 1(sp), 2(sp): b high; return slot 6,7; a 8,9; b 10,11
	ldax	10(sp)
	rem	0, 2(sp)		; A = r low, RA = r
	stax	6(sp)
	ldax	2(sp)
	btst	7			; Z = bit 7 of b high
	bz	4$			; b >= 0x8000: r may not fit the 15 bits of RA
	xchg	ra
	txau
	andi	#0x7f
	stax	7(sp)
	ads	#2
	br	done16
4$:
	; q = a / b is 0 or 1, so r high = a high - q * b high - (a low < r low)
	ldax	8(sp)
	lddiv	9(sp)
	ldax	10(sp)
	div	0, 2(sp)		; A = q
	mul	2(sp)			; q * b high
	push	a			; 1(sp); b high 2,3; return slot 7,8; a 9,10; b 11,12
	ldax	7(sp)			; r low
	cpi	#0x01			; C = (r low == 0): cmp would report a borrow against 0
	ldax	9(sp)			; a low
	ifte	nc
	cmp	7(sp)			; C = a low < r low, the borrow of the low byte
	ldc	#0
	ldax	10(sp)			; a high
	sub	1(sp)			; - q * b high - borrow
	stax	8(sp)
	ads	#3
done16:
	ldax	1(sp)
	btst	7
	bnz	5$
	spix
	adx	#4
	jal	neg2			; the result negated in place
5$:	ads	#1
	lra
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
; Frame after sra and the flag push: 1(sp) the flag - bit 7 negate the
; result, bit 0 remainder; RA 2,3; the return slot 4,5; a 6(sp); b 7(sp).

	.globl __divschar, __modschar, __divuchar, __moduchar
	.globl __divsuchar, __modsuchar, __divuschar, __moduschar

__divschar:
	sra
	ldax	5(sp)			; the signs differ: a negative quotient
	xor	6(sp)
	andi	#0x80
	push	a
	jal	absa
	jal	absb
	br	div8
__modschar:
	sra
	ldax	5(sp)			; the dividend's sign is the remainder's
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa
	jal	absb
	br	div8
__divuschar:				; a signed, b unsigned
	sra
	ldax	5(sp)
	andi	#0x80
	push	a
	jal	absa
	br	div8
__moduschar:
	sra
	ldax	5(sp)
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa
	br	div8
__divsuchar:				; a unsigned, b signed
	sra
	ldax	6(sp)
	andi	#0x80
	push	a
	jal	absb
	br	div8
__modsuchar:				; a non-negative dividend: a non-negative remainder
	sra
	ldi	#0x01
	push	a
	jal	absb
	br	div8
__divuchar:
	sra
	ldi	#0x00
	push	a
	br	div8
__moduchar:
	sra
	ldi	#0x01
	push	a
div8:
	ldax	6(sp)
	tax				; the 8-bit dividend
	ldax	1(sp)
	btst	0
	bz	1$			; Z = bit 0: remainder
	ldax	7(sp)
	div	3
	br	2$
1$:	ldax	7(sp)
	rem	3
2$:	stax	4(sp)			; the result, 0..255, as an int
	ldi	#0x00
	stax	5(sp)
	ldax	1(sp)
	btst	7
	bnz	3$
	spix
	adx	#4
	jal	neg2			; negated as 16 bits
3$:	ads	#1
	lra
	ret

; a = |a|, b = |b| (the frame above; a jal moves no stack)
absa:
	ldax	6(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	6(sp)
	ldc	#1
	adc	#0x00
	stax	6(sp)
9$:	ret
absb:
	ldax	7(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	7(sp)
	ldc	#1
	adc	#0x00
	stax	7(sp)
9$:	ret

; ---- bytes, a byte back in A -------------------------------------------
;
; The same when the compiler only wants a byte (genDivMod: a / b of two
; chars with a char result): no return slot, the result in A, 128 is
; 0x80.  Frame after sra and the flag push: 1(sp) the flag; RA 2,3; a
; 4(sp); b 5(sp).

	.globl __divschar8, __modschar8, __divuschar8, __moduschar8
	.globl __divsuchar8, __modsuchar8

__divschar8:
	sra
	ldax	3(sp)
	xor	4(sp)
	andi	#0x80
	push	a
	jal	absa8
	jal	absb8
	br	div8b
__modschar8:
	sra
	ldax	3(sp)
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa8
	jal	absb8
	br	div8b
__divuschar8:				; a signed, b unsigned
	sra
	ldax	3(sp)
	andi	#0x80
	push	a
	jal	absa8
	br	div8b
__moduschar8:
	sra
	ldax	3(sp)
	andi	#0x80
	ldc	#0
	adc	#0x01
	push	a
	jal	absa8
	br	div8b
__divsuchar8:				; a unsigned, b signed
	sra
	ldax	4(sp)
	andi	#0x80
	push	a
	jal	absb8
	br	div8b
__modsuchar8:
	sra
	ldi	#0x01
	push	a
	jal	absb8
div8b:
	ldax	4(sp)
	tax
	ldax	1(sp)
	btst	0
	bz	1$			; Z = bit 0: remainder
	ldax	5(sp)
	div	3
	br	2$
1$:	ldax	5(sp)
	rem	3
2$:	stax	4(sp)			; the result, negated in place when the signs say so
	ldax	1(sp)
	btst	7
	bnz	3$
	ldi	#0xff
	sub	4(sp)
	ldc	#1
	adc	#0x00
	br	4$
3$:	ldax	4(sp)
4$:	ads	#1
	lra
	ret

absa8:
	ldax	4(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	4(sp)
	ldc	#1
	adc	#0x00
	stax	4(sp)
9$:	ret
absb8:
	ldax	5(sp)
	btst	7
	bnz	9$
	ldi	#0xff
	sub	5(sp)
	ldc	#1
	adc	#0x00
	stax	5(sp)
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

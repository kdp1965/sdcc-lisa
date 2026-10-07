;--------------------------------------------------------------------------
;  divu.s - _divuint() / _moduint() / _divsint() / _modsint() on the
;  hardware divider, for the LISA port
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

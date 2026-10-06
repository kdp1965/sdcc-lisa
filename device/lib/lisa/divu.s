;--------------------------------------------------------------------------
;  divu.s - _divuint() / _moduint() on the hardware divider, for the
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
;
;
; The compiler emits the divider inline for unsigned operands up to 16
; bits; these are for the callers it cannot inline for.  unsigned int
; f(unsigned int a, unsigned int b): after sra the return slot is at
; 3(sp),4(sp), a at 5(sp),6(sp), b at 7(sp),8(sp).  lddiv loads {RA[7:0],
; IX[7:0]} with the dividend, div / rem take the divisor from {2(sp), A}
; and leave the result's low byte in A and all 16 bits in RA (ra_cond
; set), which is where the high byte is read from - 15 bits: a quotient
; by 1 has no high byte there (the silicon leaves 0) and a remainder of
; 0x8000 or more does not fit, so these check for those: on the TT07 silicon the
; stage-two store of the high byte never lands, and a div / rem whose
; second word has bit 1 clear (an offset of 0, 1, 4, 5 ...) writes the
; low byte to that offset's address - hence the divisor's high byte
; pushed twice, to sit at 2(sp).  RA is saved around all of it.

	.area CODE (CODE)

	.globl __divuint
__divuint:
	sra
	ldax	5(sp)
	lddiv	6(sp)
	ldax	8(sp)			; b high
	push	a
	push	a			; 1(sp), 2(sp): b high; a 7,8; b 9,10; return slot 5,6
	ldax	9(sp)
	div	0, 2(sp)		; A = q low, RA = q
	stax	5(sp)
	xchg	ra
	txau
	andi	#0x7f			; q high - except that the silicon leaves 0 there for b == 1
	push	a			; 1(sp); b high 3(sp); return slot 6,7; a 8,9; b 10,11
	ldax	10(sp)
	cpi	#0x01
	bnz	1$
	ldax	3(sp)
	cpi	#0x00
	bnz	1$
	ldax	9(sp)			; b == 1: q = a
	stax	1(sp)
1$:
	pop	a
	stax	6(sp)
	ads	#2
	lra
	ret

	.globl __moduint
__moduint:
	sra
	ldax	5(sp)
	lddiv	6(sp)
	ldax	8(sp)			; b high
	push	a
	push	a			; 1(sp), 2(sp): b high; a 7,8; b 9,10; return slot 5,6
	ldax	9(sp)
	rem	0, 2(sp)		; A = r low, RA = r
	stax	5(sp)
	ldax	2(sp)
	btst	7			; Z = bit 7 of b high
	bz	1$			; b >= 0x8000: r may not fit the 15 bits of RA
	xchg	ra
	txau
	andi	#0x7f
	stax	6(sp)
	ads	#2
	lra
	ret
1$:
	; q = a / b is 0 or 1, so r high = a high - q * b high - (a low < r low)
	ldax	7(sp)
	lddiv	8(sp)
	ldax	9(sp)
	div	0, 2(sp)		; A = q
	mul	2(sp)			; q * b high
	push	a			; 1(sp); b high 2,3; return slot 6,7; a 8,9; b 10,11
	ldax	6(sp)			; r low
	cpi	#0x01			; C = (r low == 0): cmp would report a borrow against 0
	ldax	8(sp)			; a low
	ifte	nc
	cmp	6(sp)			; C = a low < r low, the borrow of the low byte
	ldc	#0
	ldax	9(sp)			; a high
	sub	1(sp)			; - q * b high - borrow
	stax	7(sp)
	ads	#3
	lra
	ret

;--------------------------------------------------------------------------
;  divull.s - _divulonglong() / _modulonglong() on the chained divide
;  step of the second chip (ttsky26d RESPIN 27), for the LISA port's
;  lisa-rev2 and lisa-rev2k libraries (replaces _divulonglong.c and
;  _modulonglong.c there)
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
;
; A divisor below 65536 - printf's bases, a clock's 1000, a size's 1024 -
; takes the fast path: a division of 0 by it latches it in the divider
; and clears IX, then one divc per byte of the dividend, most significant
; first, gives a byte of the quotient in A with the remainder carried in
; IX: some 60 instructions where SDCC's bit-serial loop takes 20K.  A
; wider divisor still goes to that loop (_divull_bits / _modull_bits,
; the generic routines renamed, divullb.c / modullb.c).  The latched
; divisor is state that any division replaces, so the chain runs with
; interrupts off (ldamode's bit 3 says whether to turn them back on).
; The signed routines (_divslonglong.c, _modslonglong.c) call these.
;
; Frame at entry (sdcc -mlisa --rev2): 0..7(sp) the result's slot,
; 8..15(sp) the dividend x, 16..23(sp) the divisor y, little-endian.
; After sra and the two pushes (the interrupt state, the divisor's high
; byte): the slot at 4..11, x at 12..19, y at 20..27.

	.include "lisamodel.inc"

	.area CODE (CODE)

	.if LISA_REV2
	.globl __divulonglong, __modulonglong
	.globl __divull_bits, __modull_bits

__divulonglong:
	ldax	18(sp)			; y[2..7]: zero for the fast path
	or	19(sp)
	or	20(sp)
	or	21(sp)
	or	22(sp)
	or	23(sp)
	bnz	1$
	sra
	ldamode				; bit 3: interrupts enabled
	push	a
	di
	ldax	20(sp)			; y[1]
	push	a			; the divisor's high byte at 0(sp)
	ldi	#0
	tax
	taxu				; IX = 0: the remainder so far
	ldax	20(sp)			; y[0]
	div	2, 0(sp)		; 0 / {y[1], y[0]}: the divisor latched
	ldax	19(sp)			; x[7]
	divc
	stax	11(sp)			; q[7]
	ldax	18(sp)			; x[6]
	divc
	stax	10(sp)			; q[6]
	ldax	17(sp)			; x[5]
	divc
	stax	9(sp)			; q[5]
	ldax	16(sp)			; x[4]
	divc
	stax	8(sp)			; q[4]
	ldax	15(sp)			; x[3]
	divc
	stax	7(sp)			; q[3]
	ldax	14(sp)			; x[2]
	divc
	stax	6(sp)			; q[2]
	ldax	13(sp)			; x[1]
	divc
	stax	5(sp)			; q[1]
	ldax	12(sp)			; x[0]
	divc
	stax	4(sp)			; q[0]
	br	done
1$:	ldx	#__divull_bits		; the bit-serial loop, with this frame
	jmp	ix

__modulonglong:
	ldax	18(sp)
	or	19(sp)
	or	20(sp)
	or	21(sp)
	or	22(sp)
	or	23(sp)
	bnz	2$
	sra
	ldamode
	push	a
	di
	ldax	20(sp)
	push	a
	ldi	#0
	tax
	taxu
	ldax	20(sp)
	div	2, 0(sp)
	ldax	19(sp)			; x[7]
	divc
	ldax	18(sp)			; x[6]
	divc
	ldax	17(sp)			; x[5]
	divc
	ldax	16(sp)			; x[4]
	divc
	ldax	15(sp)			; x[3]
	divc
	ldax	14(sp)			; x[2]
	divc
	ldax	13(sp)			; x[1]
	divc
	ldax	12(sp)			; x[0]
	divc
	txa				; the remainder: IX
	stax	4(sp)
	txau
	stax	5(sp)
	ldi	#0
	stax	6(sp)
	stax	7(sp)
	stax	8(sp)
	stax	9(sp)
	stax	10(sp)
	stax	11(sp)
	br	done
2$:	ldx	#__modull_bits
	jmp	ix

done:	ads	#1			; the divisor's high byte
	pop	a			; the interrupt state
	andi	#0x08
	bz	3$
	ei
3$:	lra
	ret
	.endif

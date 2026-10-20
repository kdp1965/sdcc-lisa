;--------------------------------------------------------------------------
;  divul.s - long division and remainder, unsigned and signed, on the
;  16-bit hardware divider, for the LISA port (replaces _divulong.c,
;  _modulong.c, _divslong.c and _modslong.c)
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
; The divider takes a 16-bit dividend ({n(sp), A} through lddiv) and a
; 16-bit divisor ({T+1(sp), A}: the slot the TT07 silicon handles) and
; gives the low byte of the quotient or remainder in A.  Every division
; here has an 8-bit divisor and a result that fits a byte, so nothing
; is read from RA (which the divider clobbers, as it does IX).
;
; A divisor below 256 is long division a byte at a time: four
; divisions.  Otherwise Knuth's algorithm D in base 256: the divisor
; normalized (shifted left until its top digit is 0x80 or more, V = 2,
; 3 or 4 digits), the dividend shifted the same into five digits, and
; 5 - V quotient digits each estimated from the top two digits of the
; window over the top digit of the divisor, refined by the second
; digit, then the product subtracted and, on a borrow, the divisor
; added back once.  The shifts are multiplications by 2^s with mul /
; mulu, which the silicon's flag bugs do not touch.  All four routines
; run the same code; the flag picks what goes to the return slot.  The
; signed ones first make the operands non-negative in place and negate
; the result at the end when the signs call for it (a quotient when
; they differ, a remainder when the dividend's is set) - one negation
; per value, where the C wrappers negate operands and result again
; around a call of the unsigned routine.
;
; Frame after sra and ads #-30 (slot n is T+n-1(sp); the scratch slot
; below is also called T, the two are unrelated):
;   1 T        scratch                 11..17 U[0..6]  dividend / remainder
;   2 ZERO     the divisor's high byte 18..21 V[0..3]  normalized divisor
;   3 FLAG     0 quotient, 1 remainder 22..26 T0 T1 T2 HI LO  the window's top
;   4 S        shift (0..7)                   three digits, qhat * VT2; then
;   5 J        window offset                  P[0..4] = qhat * V
;   6 QHAT     7 RHAT                  27 VN           digits of V (2..4)
;   8 VT       9 VT2   (V's top two)   28 NEG          the result's sign (bit 7)
;  10 CY / CNT / 2^s                   29 VN == 2   30 VN <= 3  (the digits
;                                      of P above VN are 0: skipped)
;  31,32 RA  33..36 the result, holding Q[0..3] meanwhile  37..40 a  41..44 b

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl __divulong, __modulong, __divslong, __modslong

__divulong:
	sra
	ads	#-30
	ldi	#0x00
	stax	T+2(sp)			; FLAG: quotient
	stax	T+27(sp)			; NEG: the result stays
	br	udiv32
__modulong:
	sra
	ads	#-30
	ldi	#0x01
	stax	T+2(sp)			; FLAG: remainder
	ldi	#0x00
	stax	T+27(sp)
	br	udiv32
__divslong:
	sra
	ads	#-30
	ldi	#0x00
	stax	T+2(sp)
	ldax	T+R+43(sp)			; the signs differ: a negative quotient
	xor	T+R+39(sp)
	br	sdiv32
__modslong:
	sra
	ads	#-30
	ldi	#0x01
	stax	T+2(sp)
	ldax	T+R+39(sp)			; the dividend's sign is the remainder's
sdiv32:
	andi	#0x80
	stax	T+27(sp)			; NEG
	ldax	T+R+39(sp)
	btst	7
	bnz	1$
	spix
	adx	#T+R+36
	jal	neg4			; a = -a
1$:	ldax	T+R+43(sp)
	btst	7
	bnz	udiv32
	spix
	adx	#T+R+40
	jal	neg4			; b = -b
udiv32:
	ldi	#0x00
	stax	T+1(sp)			; ZERO
	stax	T+3(sp)			; S
	stax	T+14(sp)			; U[4..6]
	stax	T+15(sp)
	stax	T+16(sp)
	stax	T+R+32(sp)			; Q[0..3]
	stax	T+R+33(sp)
	stax	T+R+34(sp)
	stax	T+R+35(sp)
	; b below 256?
	ldax	T+R+41(sp)
	or	T+R+42(sp)
	or	T+R+43(sp)
	bnz	knuth

	; ---- a byte at a time: {r, a[i]} / b0, r = {r, a[i]} % b0 -------
	stax	T(sp)			; r = 0
	ldax	T+R+39(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	div	0, T+1(sp)
	stax	T+R+35(sp)			; Q[3]
	ldax	T+R+39(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	rem	0, T+1(sp)
	stax	T(sp)
	ldax	T+R+38(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	div	0, T+1(sp)
	stax	T+R+34(sp)			; Q[2]
	ldax	T+R+38(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	rem	0, T+1(sp)
	stax	T(sp)
	ldax	T+R+37(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	div	0, T+1(sp)
	stax	T+R+33(sp)			; Q[1]
	ldax	T+R+37(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	rem	0, T+1(sp)
	stax	T(sp)
	ldax	T+R+36(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	div	0, T+1(sp)
	stax	T+R+32(sp)			; Q[0]
	ldax	T+R+36(sp)
	lddiv	T(sp)
	ldax	T+R+40(sp)
	rem	0, T+1(sp)
	stax	T+10(sp)			; the remainder, as U[0..3] with S = 0
	ldi	#0x00
	stax	T+11(sp)
	stax	T+12(sp)
	stax	T+13(sp)
	br	result

	; ---- Knuth D ----------------------------------------------------
knuth:
	; VN and the top digit
	ldi	#0x00
	stax	T+28(sp)
	stax	T+29(sp)
	ldi	#0x04
	stax	T+26(sp)
	ldax	T+R+43(sp)
	bnz	topdig
	ldi	#0x03
	stax	T+26(sp)
	ldi	#0x01
	stax	T+29(sp)
	ldax	T+R+42(sp)
	bnz	topdig
	ldi	#0x02
	stax	T+26(sp)
	ldi	#0x01
	stax	T+28(sp)
	ldax	T+R+41(sp)
topdig:
	; S = its leading zeros, T = 2^S
	stax	T(sp)			; the top digit
	ldi	#0x01
	stax	T+9(sp)			; 2^s so far
	ldax	T(sp)
clz:
	btst	7
	bz	clzdone			; Z = bit 7
	ldc	#0
	shl
	stax	T(sp)
	cmp	T+3(sp)		; TT07: a hit for the inx
	inx	T+3(sp)			; S
	ldax	T+9(sp)
	ldc	#0
	shl
	stax	T+9(sp)			; 2^s doubled
	ldax	T(sp)
	br	clz
clzdone:
	ldax	T+9(sp)
	stax	T(sp)			; T = 2^s
	; V = b * 2^s: V[k] = mul(b[k]) | mulu(b[k-1]); U = a * 2^s the same, U[4] = mulu(a[3])
	ldax	T+R+40(sp)
	mul	T(sp)
	stax	T+17(sp)
	ldax	T+R+40(sp)
	mulu	T(sp)
	stax	T+18(sp)
	ldax	T+R+41(sp)
	mul	T(sp)
	or	T+18(sp)
	stax	T+18(sp)
	ldax	T+R+41(sp)
	mulu	T(sp)
	stax	T+19(sp)
	ldax	T+R+42(sp)
	mul	T(sp)
	or	T+19(sp)
	stax	T+19(sp)
	ldax	T+R+42(sp)
	mulu	T(sp)
	stax	T+20(sp)
	ldax	T+R+43(sp)
	mul	T(sp)
	or	T+20(sp)
	stax	T+20(sp)
	ldax	T+R+36(sp)
	mul	T(sp)
	stax	T+10(sp)
	ldax	T+R+36(sp)
	mulu	T(sp)
	stax	T+11(sp)
	ldax	T+R+37(sp)
	mul	T(sp)
	or	T+11(sp)
	stax	T+11(sp)
	ldax	T+R+37(sp)
	mulu	T(sp)
	stax	T+12(sp)
	ldax	T+R+38(sp)
	mul	T(sp)
	or	T+12(sp)
	stax	T+12(sp)
	ldax	T+R+38(sp)
	mulu	T(sp)
	stax	T+13(sp)
	ldax	T+R+39(sp)
	mul	T(sp)
	or	T+13(sp)
	stax	T+13(sp)
	ldax	T+R+39(sp)
	mulu	T(sp)
	stax	T+14(sp)
	; VT, VT2 = V[VN-1], V[VN-2]; J = 4 - VN
	spix
	adx	#T+15			; &V[0] - 2
	ldax	T+26(sp)
	addax
	ldax	1(ix)
	stax	T+7(sp)
	ldax	0(ix)
	stax	T+8(sp)
	ldi	#0x04
	ldc	#0
	sub	T+26(sp)			; sub takes M + C off
	stax	T+4(sp)

step:
	; the window's top three digits U[J+VN-2 .. J+VN]
	spix
	adx	#T+8			; &U[0] - 2
	ldax	T+4(sp)
	addax
	ldax	T+26(sp)
	addax
	ldax	0(ix)
	stax	T+21(sp)			; T0
	ldax	1(ix)
	stax	T+22(sp)			; T1
	ldax	2(ix)
	stax	T+23(sp)			; T2
	; QHAT, RHAT = {T2, T1} / VT, % VT - or 255 and T1 + VT when T2 == VT
	cmp	T+7(sp)
	bz	clamp
	ldax	T+22(sp)
	lddiv	T+23(sp)
	ldax	T+7(sp)
	div	0, T+1(sp)
	stax	T+5(sp)
	ldax	T+22(sp)
	lddiv	T+23(sp)
	ldax	T+7(sp)
	rem	0, T+1(sp)
	stax	T+6(sp)
	br	refine
clamp:
	ldi	#0xff
	stax	T+5(sp)
	ldax	T+22(sp)
	add	T+7(sp)
	stax	T+6(sp)
	if	c
	br.p	mulsub			; RHAT is 256 or more: QHAT stands
refine:
	; while QHAT * VT2 > {RHAT, T0}: QHAT--, RHAT += VT (stop when it carries)
	ldax	T+5(sp)
	mulu	T+8(sp)
	stax	T+24(sp)			; HI
	ldax	T+5(sp)
	mul	T+8(sp)
	stax	T+25(sp)			; LO
	ldax	T+24(sp)
	cpi	#0x01			; C = HI == 0 (cmp would report a borrow against 0)
	ldax	T+6(sp)
	ifte	nc
	cmp	T+24(sp)			; C = RHAT < HI, Z = equal
	ldc	#0
	if	c
	br.p	toobig
	bnz	mulsub			; RHAT > HI
	ldax	T+25(sp)
	cpi	#0x01
	ldax	T+21(sp)
	ifte	nc
	cmp	T+25(sp)			; C = T0 < LO
	ldc	#0
	if	nc
	br.p	mulsub
toobig:
	cmp	T+5(sp)		; TT07: a hit for the dcx
	dcx	T+5(sp)
	ldax	T+6(sp)
	add	T+7(sp)
	stax	T+6(sp)
	if	nc
	br.p	refine

mulsub:
	; P = QHAT * V, five digits
	ldi	#0x00
	stax	T+9(sp)			; CY
	ldax	T+5(sp)
	mulu	T+17(sp)
	stax	T(sp)
	ldax	T+5(sp)
	mul	T+17(sp)
	add	T+9(sp)
	stax	T+21(sp)
	ldax	T(sp)
	adc	#0x00
	stax	T+9(sp)
	ldax	T+5(sp)
	mulu	T+18(sp)
	stax	T(sp)
	ldax	T+5(sp)
	mul	T+18(sp)
	add	T+9(sp)
	stax	T+22(sp)
	ldax	T(sp)
	adc	#0x00
	stax	T+9(sp)
	ldax	T+28(sp)
	bnz	6$			; VN == 2: P[2] is the carry
	ldax	T+5(sp)
	mulu	T+19(sp)
	stax	T(sp)
	ldax	T+5(sp)
	mul	T+19(sp)
	add	T+9(sp)
	stax	T+23(sp)
	ldax	T(sp)
	adc	#0x00
	stax	T+9(sp)
	ldax	T+29(sp)
	bnz	7$			; VN == 3: P[3] is the carry
	ldax	T+5(sp)
	mulu	T+20(sp)
	stax	T(sp)
	ldax	T+5(sp)
	mul	T+20(sp)
	add	T+9(sp)
	stax	T+24(sp)
	ldax	T(sp)
	adc	#0x00
	stax	T+25(sp)
	br	sub5
6$:
	ldax	T+9(sp)
	stax	T+23(sp)
	br	sub5
7$:
	ldax	T+9(sp)
	stax	T+24(sp)
sub5:
	; U[J..J+4] -= P: the borrow folded into the next P byte first, so
	; that the silicon's sub (wrong for M == 0 without a borrow in) is
	; guarded by cpi #1 on a byte that includes it
	spix
	adx	#T+10
	ldax	T+4(sp)
	addax				; IX = &U[J]
	ldax	T+21(sp)
	cpi	#0x01			; C = P[0] == 0
	ldax	0(ix)
	ifte	nc
	sub	T+21(sp)
	ldc	#0
	stax	0(ix)
	ldax	T+22(sp)
	adc	#0x00			; P[1] + borrow; C = it was 0xff with a borrow: the byte stays, borrow out
	stax	T(sp)
	if	c
	br.p	1$
	cpi	#0x01
	ldax	1(ix)
	ifte	nc
	sub	T(sp)
	ldc	#0
	stax	1(ix)
1$:
	ldax	T+23(sp)
	adc	#0x00
	stax	T(sp)
	if	c
	br.p	2$
	cpi	#0x01
	ldax	2(ix)
	ifte	nc
	sub	T(sp)
	ldc	#0
	stax	2(ix)
2$:
	ldax	T+28(sp)
	bnz	4$			; VN == 2: nothing above
	ldax	T+24(sp)
	adc	#0x00
	stax	T(sp)
	if	c
	br.p	3$
	cpi	#0x01
	ldax	3(ix)
	ifte	nc
	sub	T(sp)
	ldc	#0
	stax	3(ix)
3$:
	ldax	T+29(sp)
	bnz	4$			; VN == 3
	ldax	T+25(sp)
	adc	#0x00
	stax	T(sp)
	if	c
	br.p	4$
	cpi	#0x01
	ldax	4(ix)
	ifte	nc
	sub	T(sp)
	ldc	#0
	stax	4(ix)
4$:
	if	nc
	br.p	storeq
	; a borrow out: QHAT was one too big, add V back (the carry out drops)
	cmp	T+5(sp)		; TT07: a hit for the dcx
	dcx	T+5(sp)
	ldax	0(ix)
	add	T+17(sp)
	stax	0(ix)
	ldax	1(ix)
	adc	#0x00
	savec
	add	T+18(sp)
	if	nc
	restc
	stax	1(ix)
	ldax	2(ix)
	adc	#0x00
	savec
	add	T+19(sp)
	if	nc
	restc
	stax	2(ix)
	ldax	T+28(sp)
	bnz	storeq
	ldax	3(ix)
	adc	#0x00
	savec
	add	T+20(sp)
	if	nc
	restc
	stax	3(ix)
	ldax	T+29(sp)
	bnz	storeq
	ldax	4(ix)
	adc	#0x00
	stax	4(ix)
storeq:
	spix
	adx	#T+R+32
	ldax	T+4(sp)
	addax				; IX = &Q[J], in the return slot
	ldax	T+5(sp)
	stax	0(ix)
	cmp	T+4(sp)		; TT07: a hit for the dcx
	dcx	T+4(sp)			; C = J was 0
	if	nc
	br.p	step

result:
	ldax	T+2(sp)
	bnz	remainder
	br	done			; the quotient is in the return slot
remainder:
	; U[0..3] >> S: U[k] = mulu(U[k], 2^(8-S)) | mul(U[k+1], 2^(8-S)), unless S == 0
	ldax	T+3(sp)
	bz	unshifted
	ldi	#0x01
	stax	T(sp)
	ldi	#0x08
	ldc	#0
	sub	T+3(sp)			; 8 - S
	stax	T+9(sp)			; CNT
5$:
	ldax	T(sp)
	ldc	#0
	shl
	stax	T(sp)
	cmp	T+9(sp)		; TT07: a hit for the dcx
	dcx	T+9(sp)
	ldax	T+9(sp)
	bnz	5$
	ldax	T+10(sp)
	mulu	T(sp)
	stax	T+R+32(sp)
	ldax	T+11(sp)
	mul	T(sp)
	or	T+R+32(sp)
	stax	T+R+32(sp)
	ldax	T+11(sp)
	mulu	T(sp)
	stax	T+R+33(sp)
	ldax	T+12(sp)
	mul	T(sp)
	or	T+R+33(sp)
	stax	T+R+33(sp)
	ldax	T+12(sp)
	mulu	T(sp)
	stax	T+R+34(sp)
	ldax	T+13(sp)
	mul	T(sp)
	or	T+R+34(sp)
	stax	T+R+34(sp)
	ldax	T+13(sp)
	mulu	T(sp)
	stax	T+R+35(sp)
	br	done
unshifted:
	ldax	T+10(sp)
	stax	T+R+32(sp)
	ldax	T+11(sp)
	stax	T+R+33(sp)
	ldax	T+12(sp)
	stax	T+R+34(sp)
	ldax	T+13(sp)
	stax	T+R+35(sp)
done:
	ldax	T+27(sp)
	btst	7
	bnz	2$
	spix
	adx	#T+R+32
	jal	neg4			; the result negated in place
2$:	ads	#30
	lra
	ret

; the four bytes at IX negated: ~x + 1 with the carry through adc #0 (ldi
; clears C for the sub, whose own C the TT07 gets wrong; stax keeps it)
neg4:
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
	savec
	ldi	#0xff
	sub	2(ix)
	restc
	adc	#0x00
	stax	2(ix)
	savec
	ldi	#0xff
	sub	3(ix)
	restc
	adc	#0x00
	stax	3(ix)
	ret

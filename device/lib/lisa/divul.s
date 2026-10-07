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
; 16-bit divisor ({2(sp), A}: the slot the TT07 silicon handles) and
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
; Frame after sra and ads #-30:
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

	.area CODE (CODE)

	.globl __divulong, __modulong, __divslong, __modslong

__divulong:
	sra
	ads	#-30
	ldi	#0x00
	stax	3(sp)			; FLAG: quotient
	stax	28(sp)			; NEG: the result stays
	br	udiv32
__modulong:
	sra
	ads	#-30
	ldi	#0x01
	stax	3(sp)			; FLAG: remainder
	ldi	#0x00
	stax	28(sp)
	br	udiv32
__divslong:
	sra
	ads	#-30
	ldi	#0x00
	stax	3(sp)
	ldax	44(sp)			; the signs differ: a negative quotient
	xor	40(sp)
	br	sdiv32
__modslong:
	sra
	ads	#-30
	ldi	#0x01
	stax	3(sp)
	ldax	40(sp)			; the dividend's sign is the remainder's
sdiv32:
	andi	#0x80
	stax	28(sp)			; NEG
	ldax	40(sp)
	btst	7
	bnz	1$
	spix
	adx	#37
	jal	neg4			; a = -a
1$:	ldax	44(sp)
	btst	7
	bnz	udiv32
	spix
	adx	#41
	jal	neg4			; b = -b
udiv32:
	ldi	#0x00
	stax	2(sp)			; ZERO
	stax	4(sp)			; S
	stax	15(sp)			; U[4..6]
	stax	16(sp)
	stax	17(sp)
	stax	33(sp)			; Q[0..3]
	stax	34(sp)
	stax	35(sp)
	stax	36(sp)
	; b below 256?
	ldax	42(sp)
	or	43(sp)
	or	44(sp)
	bnz	knuth

	; ---- a byte at a time: {r, a[i]} / b0, r = {r, a[i]} % b0 -------
	stax	1(sp)			; r = 0
	ldax	40(sp)
	lddiv	1(sp)
	ldax	41(sp)
	div	0, 2(sp)
	stax	36(sp)			; Q[3]
	ldax	40(sp)
	lddiv	1(sp)
	ldax	41(sp)
	rem	0, 2(sp)
	stax	1(sp)
	ldax	39(sp)
	lddiv	1(sp)
	ldax	41(sp)
	div	0, 2(sp)
	stax	35(sp)			; Q[2]
	ldax	39(sp)
	lddiv	1(sp)
	ldax	41(sp)
	rem	0, 2(sp)
	stax	1(sp)
	ldax	38(sp)
	lddiv	1(sp)
	ldax	41(sp)
	div	0, 2(sp)
	stax	34(sp)			; Q[1]
	ldax	38(sp)
	lddiv	1(sp)
	ldax	41(sp)
	rem	0, 2(sp)
	stax	1(sp)
	ldax	37(sp)
	lddiv	1(sp)
	ldax	41(sp)
	div	0, 2(sp)
	stax	33(sp)			; Q[0]
	ldax	37(sp)
	lddiv	1(sp)
	ldax	41(sp)
	rem	0, 2(sp)
	stax	11(sp)			; the remainder, as U[0..3] with S = 0
	ldi	#0x00
	stax	12(sp)
	stax	13(sp)
	stax	14(sp)
	br	result

	; ---- Knuth D ----------------------------------------------------
knuth:
	; VN and the top digit
	ldi	#0x00
	stax	29(sp)
	stax	30(sp)
	ldi	#0x04
	stax	27(sp)
	ldax	44(sp)
	bnz	topdig
	ldi	#0x03
	stax	27(sp)
	ldi	#0x01
	stax	30(sp)
	ldax	43(sp)
	bnz	topdig
	ldi	#0x02
	stax	27(sp)
	ldi	#0x01
	stax	29(sp)
	ldax	42(sp)
topdig:
	; S = its leading zeros, T = 2^S
	stax	1(sp)			; the top digit
	ldi	#0x01
	stax	10(sp)			; 2^s so far
	ldax	1(sp)
clz:
	btst	7
	bz	clzdone			; Z = bit 7
	ldc	#0
	shl
	stax	1(sp)
	cmp	4(sp)		; TT07: a hit for the inx
	inx	4(sp)			; S
	ldax	10(sp)
	ldc	#0
	shl
	stax	10(sp)			; 2^s doubled
	ldax	1(sp)
	br	clz
clzdone:
	ldax	10(sp)
	stax	1(sp)			; T = 2^s
	; V = b * 2^s: V[k] = mul(b[k]) | mulu(b[k-1]); U = a * 2^s the same, U[4] = mulu(a[3])
	ldax	41(sp)
	mul	1(sp)
	stax	18(sp)
	ldax	41(sp)
	mulu	1(sp)
	stax	19(sp)
	ldax	42(sp)
	mul	1(sp)
	or	19(sp)
	stax	19(sp)
	ldax	42(sp)
	mulu	1(sp)
	stax	20(sp)
	ldax	43(sp)
	mul	1(sp)
	or	20(sp)
	stax	20(sp)
	ldax	43(sp)
	mulu	1(sp)
	stax	21(sp)
	ldax	44(sp)
	mul	1(sp)
	or	21(sp)
	stax	21(sp)
	ldax	37(sp)
	mul	1(sp)
	stax	11(sp)
	ldax	37(sp)
	mulu	1(sp)
	stax	12(sp)
	ldax	38(sp)
	mul	1(sp)
	or	12(sp)
	stax	12(sp)
	ldax	38(sp)
	mulu	1(sp)
	stax	13(sp)
	ldax	39(sp)
	mul	1(sp)
	or	13(sp)
	stax	13(sp)
	ldax	39(sp)
	mulu	1(sp)
	stax	14(sp)
	ldax	40(sp)
	mul	1(sp)
	or	14(sp)
	stax	14(sp)
	ldax	40(sp)
	mulu	1(sp)
	stax	15(sp)
	; VT, VT2 = V[VN-1], V[VN-2]; J = 4 - VN
	spix
	adx	#16			; &V[0] - 2
	ldax	27(sp)
	addax
	ldax	1(ix)
	stax	8(sp)
	ldax	0(ix)
	stax	9(sp)
	ldi	#0x04
	ldc	#0
	sub	27(sp)			; sub takes M + C off
	stax	5(sp)

step:
	; the window's top three digits U[J+VN-2 .. J+VN]
	spix
	adx	#9			; &U[0] - 2
	ldax	5(sp)
	addax
	ldax	27(sp)
	addax
	ldax	0(ix)
	stax	22(sp)			; T0
	ldax	1(ix)
	stax	23(sp)			; T1
	ldax	2(ix)
	stax	24(sp)			; T2
	; QHAT, RHAT = {T2, T1} / VT, % VT - or 255 and T1 + VT when T2 == VT
	cmp	8(sp)
	bz	clamp
	ldax	23(sp)
	lddiv	24(sp)
	ldax	8(sp)
	div	0, 2(sp)
	stax	6(sp)
	ldax	23(sp)
	lddiv	24(sp)
	ldax	8(sp)
	rem	0, 2(sp)
	stax	7(sp)
	br	refine
clamp:
	ldi	#0xff
	stax	6(sp)
	ldax	23(sp)
	add	8(sp)
	stax	7(sp)
	if	c
	br.p	mulsub			; RHAT is 256 or more: QHAT stands
refine:
	; while QHAT * VT2 > {RHAT, T0}: QHAT--, RHAT += VT (stop when it carries)
	ldax	6(sp)
	mulu	9(sp)
	stax	25(sp)			; HI
	ldax	6(sp)
	mul	9(sp)
	stax	26(sp)			; LO
	ldax	25(sp)
	cpi	#0x01			; C = HI == 0 (cmp would report a borrow against 0)
	ldax	7(sp)
	ifte	nc
	cmp	25(sp)			; C = RHAT < HI, Z = equal
	ldc	#0
	if	c
	br.p	toobig
	bnz	mulsub			; RHAT > HI
	ldax	26(sp)
	cpi	#0x01
	ldax	22(sp)
	ifte	nc
	cmp	26(sp)			; C = T0 < LO
	ldc	#0
	if	nc
	br.p	mulsub
toobig:
	cmp	6(sp)		; TT07: a hit for the dcx
	dcx	6(sp)
	ldax	7(sp)
	add	8(sp)
	stax	7(sp)
	if	nc
	br.p	refine

mulsub:
	; P = QHAT * V, five digits
	ldi	#0x00
	stax	10(sp)			; CY
	ldax	6(sp)
	mulu	18(sp)
	stax	1(sp)
	ldax	6(sp)
	mul	18(sp)
	add	10(sp)
	stax	22(sp)
	ldax	1(sp)
	adc	#0x00
	stax	10(sp)
	ldax	6(sp)
	mulu	19(sp)
	stax	1(sp)
	ldax	6(sp)
	mul	19(sp)
	add	10(sp)
	stax	23(sp)
	ldax	1(sp)
	adc	#0x00
	stax	10(sp)
	ldax	29(sp)
	bnz	6$			; VN == 2: P[2] is the carry
	ldax	6(sp)
	mulu	20(sp)
	stax	1(sp)
	ldax	6(sp)
	mul	20(sp)
	add	10(sp)
	stax	24(sp)
	ldax	1(sp)
	adc	#0x00
	stax	10(sp)
	ldax	30(sp)
	bnz	7$			; VN == 3: P[3] is the carry
	ldax	6(sp)
	mulu	21(sp)
	stax	1(sp)
	ldax	6(sp)
	mul	21(sp)
	add	10(sp)
	stax	25(sp)
	ldax	1(sp)
	adc	#0x00
	stax	26(sp)
	br	sub5
6$:
	ldax	10(sp)
	stax	24(sp)
	br	sub5
7$:
	ldax	10(sp)
	stax	25(sp)
sub5:
	; U[J..J+4] -= P: the borrow folded into the next P byte first, so
	; that the silicon's sub (wrong for M == 0 without a borrow in) is
	; guarded by cpi #1 on a byte that includes it
	spix
	adx	#11
	ldax	5(sp)
	addax				; IX = &U[J]
	ldax	22(sp)
	cpi	#0x01			; C = P[0] == 0
	ldax	0(ix)
	ifte	nc
	sub	22(sp)
	ldc	#0
	stax	0(ix)
	ldax	23(sp)
	adc	#0x00			; P[1] + borrow; C = it was 0xff with a borrow: the byte stays, borrow out
	stax	1(sp)
	if	c
	br.p	1$
	cpi	#0x01
	ldax	1(ix)
	ifte	nc
	sub	1(sp)
	ldc	#0
	stax	1(ix)
1$:
	ldax	24(sp)
	adc	#0x00
	stax	1(sp)
	if	c
	br.p	2$
	cpi	#0x01
	ldax	2(ix)
	ifte	nc
	sub	1(sp)
	ldc	#0
	stax	2(ix)
2$:
	ldax	29(sp)
	bnz	4$			; VN == 2: nothing above
	ldax	25(sp)
	adc	#0x00
	stax	1(sp)
	if	c
	br.p	3$
	cpi	#0x01
	ldax	3(ix)
	ifte	nc
	sub	1(sp)
	ldc	#0
	stax	3(ix)
3$:
	ldax	30(sp)
	bnz	4$			; VN == 3
	ldax	26(sp)
	adc	#0x00
	stax	1(sp)
	if	c
	br.p	4$
	cpi	#0x01
	ldax	4(ix)
	ifte	nc
	sub	1(sp)
	ldc	#0
	stax	4(ix)
4$:
	if	nc
	br.p	storeq
	; a borrow out: QHAT was one too big, add V back (the carry out drops)
	cmp	6(sp)		; TT07: a hit for the dcx
	dcx	6(sp)
	ldax	0(ix)
	add	18(sp)
	stax	0(ix)
	ldax	1(ix)
	adc	#0x00
	savec
	add	19(sp)
	if	nc
	restc
	stax	1(ix)
	ldax	2(ix)
	adc	#0x00
	savec
	add	20(sp)
	if	nc
	restc
	stax	2(ix)
	ldax	29(sp)
	bnz	storeq
	ldax	3(ix)
	adc	#0x00
	savec
	add	21(sp)
	if	nc
	restc
	stax	3(ix)
	ldax	30(sp)
	bnz	storeq
	ldax	4(ix)
	adc	#0x00
	stax	4(ix)
storeq:
	spix
	adx	#33
	ldax	5(sp)
	addax				; IX = &Q[J], in the return slot
	ldax	6(sp)
	stax	0(ix)
	cmp	5(sp)		; TT07: a hit for the dcx
	dcx	5(sp)			; C = J was 0
	if	nc
	br.p	step

result:
	ldax	3(sp)
	bnz	remainder
	br	done			; the quotient is in the return slot
remainder:
	; U[0..3] >> S: U[k] = mulu(U[k], 2^(8-S)) | mul(U[k+1], 2^(8-S)), unless S == 0
	ldax	4(sp)
	bz	unshifted
	ldi	#0x01
	stax	1(sp)
	ldi	#0x08
	ldc	#0
	sub	4(sp)			; 8 - S
	stax	10(sp)			; CNT
5$:
	ldax	1(sp)
	ldc	#0
	shl
	stax	1(sp)
	cmp	10(sp)		; TT07: a hit for the dcx
	dcx	10(sp)
	ldax	10(sp)
	bnz	5$
	ldax	11(sp)
	mulu	1(sp)
	stax	33(sp)
	ldax	12(sp)
	mul	1(sp)
	or	33(sp)
	stax	33(sp)
	ldax	12(sp)
	mulu	1(sp)
	stax	34(sp)
	ldax	13(sp)
	mul	1(sp)
	or	34(sp)
	stax	34(sp)
	ldax	13(sp)
	mulu	1(sp)
	stax	35(sp)
	ldax	14(sp)
	mul	1(sp)
	or	35(sp)
	stax	35(sp)
	ldax	14(sp)
	mulu	1(sp)
	stax	36(sp)
	br	done
unshifted:
	ldax	11(sp)
	stax	33(sp)
	ldax	12(sp)
	stax	34(sp)
	ldax	13(sp)
	stax	35(sp)
	ldax	14(sp)
	stax	36(sp)
done:
	ldax	28(sp)
	btst	7
	bnz	2$
	spix
	adx	#33
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

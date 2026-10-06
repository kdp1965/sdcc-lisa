;--------------------------------------------------------------------------
;  bf16add.s - bf16_t bf16_add(bf16_t a, bf16_t b) in software, for the
;  LISA port (the core's adder is defective; device/include/lisa/bf16.h)
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
; The usual algorithm, round to nearest even, denormals as zero, the
; result flushed to zero below the normal range.  The significand of the
; larger operand L is the pair {SH, SL} = {0x80 | fraction, 0x00}: the
; hidden bit at bit 15 and a whole byte of guard bits below the 8-bit
; mantissa, so the smaller one, shifted right by the exponent difference
; d <= 9, loses at most its lowest bit (d == 9: kept as the sticky), the
; low byte of L is 0 (no carry out of the low byte on an add), and the
; rounding reads one byte: up when SL > 0x80, or SL == 0x80 and SH odd.
;
; Frame after ads #-8 (a leaf: RA is not saved):
;   1 LLO  2 LHI   the larger operand (1 = its sign, 0x00/0x80, later)
;   3 SLO  4 SHI   the smaller (3 = its shifted significand, later)
;   5 SH   6 SL    the significand of the result    9,10  the return slot
;   7 E    8 D     its exponent, d / the swap flag  11,12 a (low, high)  13,14 b

	.area CODE (CODE)

	.globl _bf16_add
_bf16_add:
	ads	#-8
	; the exponents (e = hi << 1 | lo >> 7) and the special cases
	ldax	11(sp)
	shl
	ldax	12(sp)
	shl			; A = ea
	stax	7(sp)
	ldax	13(sp)
	shl
	ldax	14(sp)
	shl			; A = eb
	stax	8(sp)
	cpi	#0xff
	bz	ret_b_or_nan	; b is inf / nan (inf - inf is nan)
	ldax	7(sp)
	cpi	#0xff
	bz	ret_a		; a is inf / nan
	cpi	#0x00
	bz	a_zero		; a is 0 (or denormal): b, unless both are zero
	ldax	8(sp)
	cpi	#0x00
	bz	ret_a		; b is 0: a
	; the larger magnitude first: swap when ea < eb, or ea == eb and fa < fb
	ldax	7(sp)
	cmp	8(sp)		; C = ea < eb, Z = equal (eb is not 0)
	bz	eq_exp
	if	c
	br.p	swap
	br	noswap
eq_exp:
	ldax	11(sp)
	andi	#0x7f		; fa
	stax	3(sp)
	cpi	#0x01		; C = fa == 0 (cmp would report a borrow against 0)
	ldax	13(sp)
	andi	#0x7f		; fb, Z = fb == 0 (== fa when fa == 0)
	ifte	nc
	cmp	3(sp)		; C = fb < fa, Z = fb == fa
	ldc	#0
	bz	noswap		; equal magnitudes
	if	nc
	br.p	swap		; fa < fb
noswap:
	spix
	adx	#11		; IX = &a
	ldi	#0x02		; the smaller is 2 bytes up
	br	copy
swap:
	spix
	adx	#13		; IX = &b
	ldi	#0xfe		; the smaller is 2 bytes down
copy:
	stax	8(sp)
	ldax	0(ix)
	stax	1(sp)		; L.lo
	ldax	1(ix)
	stax	2(sp)		; L.hi
	ldax	8(sp)
	cpi	#0x02
	ifte	eq
	adx	#2
	adx	#-2
	ldax	0(ix)
	stax	3(sp)		; S.lo
	ldax	1(ix)
	stax	4(sp)		; S.hi
	; d = eL - eS; 10 or more: the smaller is below half an ulp
	ldax	3(sp)
	shl
	ldax	4(sp)
	shl			; A = eS
	stax	8(sp)
	ldax	1(sp)
	shl
	ldax	2(sp)
	shl			; A = eL
	stax	7(sp)
	ldc	#0
	sub	8(sp)		; A = d (eS is not 0: the value is right)
	cpi	#0x0a
	if	nc
	br.p	ret_L
	stax	8(sp)
	; the significand of L: {SH, SL} = {0x80 | fL, 0}
	ldax	1(sp)
	andi	#0x7f
	ldc	#0
	adc	#0x80
	stax	5(sp)
	; the sign of the result is L's (slot 1, L.lo being done with)
	ldax	2(sp)
	andi	#0x80
	stax	1(sp)
	; the significand of S, shifted right by d: {mS, 0} >> d, with the
	; bit a shift by 9 drops kept as a sticky bit
	ldax	3(sp)
	andi	#0x7f
	ldc	#0
	adc	#0x80		; A = mS
	stax	3(sp)
	ldax	8(sp)
	cpi	#0x09
	ldac	eq		; A = (d == 9)
	and	3(sp)		; A = mS & 1 when d == 9, else 0: the sticky
	stax	6(sp)		; (SL holds it for now)
	ldax	3(sp)
	push	a		; the pair {1(sp), A}; every offset below is one more
	ldi	#0x00
shift:
	dcx	9(sp)		; d--; C = it was 0
	if	c
	br.p	shifted
	ldc	#0
	shr16	1(sp)
	br	shift
shifted:
	or	7(sp)		; the sticky into the low byte
	stax	7(sp)		; SL = the low byte of S's significand
	pop	a		; A = its high byte
	stax	3(sp)		; (the smaller operand's low byte is done with)
	; same signs: add.  Different signs: subtract (L is the larger).
	ldax	2(sp)
	xor	4(sp)		; L.hi ^ S.hi, Z = bit 7 ...
	btst	7
	bz	subtract	; Z = bit 7 set = the signs differ
	ldax	5(sp)
	add	3(sp)		; SH = mL + S.hi, C = carry out
	stax	5(sp)
	if	nc
	br.p	round
	; a carry: {1, SH, SL} >> 1 with the bit shifted out kept as sticky, e++
	; (shr16 reaches 0..3(sp): the high byte goes through 1(sp))
	ldax	6(sp)
	andi	#0x01
	stax	3(sp)
	ldax	5(sp)
	push	a		; every offset below is one more
	ldax	7(sp)		; SL
	ldc	#1
	shr16	1(sp)		; 1(sp) = {1, SH[7:1]}, A = {SH[0], SL[7:1]}
	or	4(sp)		; the sticky
	stax	7(sp)		; SL
	pop	a
	stax	5(sp)		; SH
	inx	7(sp)
	br	round
subtract:
	; {SH, SL} = {mL, 0} - {S.hi, SL}: SL = -SL, SH = mL + ~S.hi + (SL == 0)
	ldi	#0xff
	sub	3(sp)		; A = ~S.hi
	push	a
	ldax	7(sp)		; SL
	cpi	#0x01		; C = SL == 0
	pop	a		; C kept
	adc	#0x00		; + C
	add	5(sp)		; + mL (the carry out is the no-borrow, 1)
	stax	5(sp)
	ldi	#0x00
	sub	6(sp)		; A = -SL (the value is right)
	stax	6(sp)
	or	5(sp)
	bz	ret_zero	; equal magnitudes
	; normalize: shift the pair left until the hidden bit is back, e--
	; each time (the high byte at 1(sp) for shl16, the low byte in A)
	ldax	5(sp)
	push	a		; every offset below is one more
	ldax	7(sp)		; A = SL
norm:
	swap	1(sp)		; A = SH, 1(sp) = SL
	btst	7
	bz	normalized	; Z = bit 7 set
	swap	1(sp)		; A = SL, 1(sp) = SH
	ldc	#0
	shl16	1(sp)		; 1(sp) = {SH[6:0], SL[7]}, A = {SL[6:0], 0}
	dcx	8(sp)		; e--, Z = it reached 0
	bnz	norm
	ads	#1
	br	ret_szero	; underflow
normalized:
	stax	6(sp)		; SH
	pop	a
	stax	6(sp)		; SL
round:
	; to nearest even: up when the guard bit is set and (a sticky bit, or the mantissa is odd)
	ldax	6(sp)
	btst	7
	bnz	pack		; Z = 0: no guard bit
	andi	#0x7f
	bnz	round_up
	ldax	5(sp)
	btst	0
	bnz	pack		; even: stays
round_up:
	inx	5(sp)		; C = it was 0xff: the mantissa wraps to 1.0, e++
	if	nc
	br.p	pack
	ldi	#0x80
	stax	5(sp)
	inx	7(sp)
pack:
	ldax	7(sp)
	cpi	#0xff
	bz	ret_inf		; overflow
	ldc	#0
	shr			; A = e >> 1, C = e & 1
	or	1(sp)		; the sign
	stax	10(sp)
	ldax	5(sp)
	andi	#0x7f
	if	c
	adc.p	#0x7f		; the low bit of the exponent into bit 7
	stax	9(sp)
	ads	#8
	ret
ret_L:
	ldax	1(sp)
	stax	9(sp)
	ldax	2(sp)
	stax	10(sp)
	ads	#8
	ret
ret_inf:
	ldi	#0x80
	stax	9(sp)
	ldax	1(sp)
	ldc	#0
	adc	#0x7f
	stax	10(sp)
	ads	#8
	ret
ret_szero:
	ldax	1(sp)
	stax	10(sp)
	ldi	#0x00
	stax	9(sp)
	ads	#8
	ret
ret_zero:
	ldi	#0x00
	stax	9(sp)
	stax	10(sp)
	ads	#8
	ret
ret_b_or_nan:
	; b is inf or nan: b, unless a is too - then a, or nan for inf - inf
	ldax	7(sp)
	cpi	#0xff
	bnz	ret_b
	ldax	11(sp)
	cmp	13(sp)		; the low bytes (both 0x80 for infinities)
	bnz	ret_a
	ldax	12(sp)
	xor	14(sp)
	cpi	#0x80
	bnz	ret_a
	ldi	#0xc0
	stax	9(sp)
	ldi	#0x7f
	stax	10(sp)
	ads	#8
	ret
a_zero:
	ldax	8(sp)
	cpi	#0x00
	bnz	ret_b
	; both zero (or denormal): -0 only when both are negative
	ldax	12(sp)
	and	14(sp)
	andi	#0x80
	stax	10(sp)
	ldi	#0x00
	stax	9(sp)
	ads	#8
	ret
ret_b:
	ldax	13(sp)
	stax	9(sp)
	ldax	14(sp)
	stax	10(sp)
	ads	#8
	ret
ret_a:
	ldax	11(sp)
	stax	9(sp)
	ldax	12(sp)
	stax	10(sp)
	ads	#8
	ret

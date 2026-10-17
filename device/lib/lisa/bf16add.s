;--------------------------------------------------------------------------
;  bf16add.s - bf16_t bf16_add(bf16_t a, bf16_t b) for the LISA port: in
;  software for the TT07 chip, whose adder is defective (device/include/
;  lisa/bf16.h), on the hardware adder in the --rev2 library (lisamodel.inc
;  from device/lib/lisa/Makefile.in says which this is)
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
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

.if LISA_REV2
; The second chip's adder rounds to nearest even (amode[2] clear) and
; keeps the subnormals.  facc <- b, f0 <- a, facc <- facc + f0, IX <- facc:
; a leaf, a at T(sp),T+1(sp) and b at T+2(sp),T+3(sp), as bf16.s's other calls.
	.area CODE (CODE)

	.globl _bf16_add
_bf16_add:
	ldax	T(sp)
	taf	0
	ldax	T+1(sp)
	taf	1
	fswap	f0
	ldax	T+2(sp)
	taf	0
	ldax	T+3(sp)
	taf	1
	fadd	f0
	tfa	0
	tax
	tfa	1
	taxu
	ret
.else
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
; Frame after ads #-10 (a leaf: RA is not saved); slot n is T+n-1(sp):
;   1 LLO  2 LHI   the larger operand (1 = its sign, 0x00/0x80, later)
;   3 SLO  4 SHI   the smaller (3 = its shifted significand, later)
;   5 SH   6 SL    the significand of the result    9,10  the result (IX on return)
;   7 E    8 D     its exponent, d / the swap flag  11,12 a (low, high)  13,14 b

	.area CODE (CODE)

	.globl _bf16_add
_bf16_add:
	ads	#-10
	; the exponents (e = hi << 1 | lo >> 7) and the special cases
	ldax	T+10(sp)
	shl
	ldax	T+11(sp)
	shl			; A = ea
	stax	T+6(sp)
	ldax	T+12(sp)
	shl
	ldax	T+13(sp)
	shl			; A = eb
	stax	T+7(sp)
	cpi	#0xff
	bz	ret_b_or_nan	; b is inf / nan (inf - inf is nan)
	ldax	T+6(sp)
	cpi	#0xff
	bz	ret_a		; a is inf / nan
	cpi	#0x00
	bz	a_zero		; a is 0 (or denormal): b, unless both are zero
	ldax	T+7(sp)
	cpi	#0x00
	bz	ret_a		; b is 0: a
	; the larger magnitude first: swap when ea < eb, or ea == eb and fa < fb
	ldax	T+6(sp)
	cmp	T+7(sp)		; C = ea < eb, Z = equal (eb is not 0)
	bz	eq_exp
	if	c
	br.p	swap
	br	noswap
eq_exp:
	ldax	T+10(sp)
	andi	#0x7f		; fa
	stax	T+2(sp)
	cpi	#0x01		; C = fa == 0 (cmp would report a borrow against 0)
	ldax	T+12(sp)
	andi	#0x7f		; fb, Z = fb == 0 (== fa when fa == 0)
	ifte	nc
	cmp	T+2(sp)		; C = fb < fa, Z = fb == fa
	ldc	#0
	bz	noswap		; equal magnitudes
	if	nc
	br.p	swap		; fa < fb
noswap:
	spix
	adx	#T+10		; IX = &a
	ldi	#0x02		; the smaller is 2 bytes up
	br	copy
swap:
	spix
	adx	#T+12		; IX = &b
	ldi	#0xfe		; the smaller is 2 bytes down
copy:
	stax	T+7(sp)
	ldax	0(ix)
	stax	T(sp)		; L.lo
	ldax	1(ix)
	stax	T+1(sp)		; L.hi
	ldax	T+7(sp)
	cpi	#0x02
	ifte	eq
	adx	#2
	adx	#-2
	ldax	0(ix)
	stax	T+2(sp)		; S.lo
	ldax	1(ix)
	stax	T+3(sp)		; S.hi
	; d = eL - eS; 10 or more: the smaller is below half an ulp
	ldax	T+2(sp)
	shl
	ldax	T+3(sp)
	shl			; A = eS
	stax	T+7(sp)
	ldax	T(sp)
	shl
	ldax	T+1(sp)
	shl			; A = eL
	stax	T+6(sp)
	ldc	#0
	sub	T+7(sp)		; A = d (eS is not 0: the value is right)
	cpi	#0x0a
	if	nc
	br.p	ret_L
	stax	T+7(sp)
	; the significand of L: {SH, SL} = {0x80 | fL, 0}
	ldax	T(sp)
	andi	#0x7f
	ldc	#0
	adc	#0x80
	stax	T+4(sp)
	; the sign of the result is L's (slot 1, L.lo being done with)
	ldax	T+1(sp)
	andi	#0x80
	stax	T(sp)
	; the significand of S, shifted right by d: {mS, 0} >> d, with the
	; bit a shift by 9 drops kept as a sticky bit
	ldax	T+2(sp)
	andi	#0x7f
	ldc	#0
	adc	#0x80		; A = mS
	stax	T+2(sp)
	ldax	T+7(sp)
	cpi	#0x09
	ldac	eq		; A = (d == 9)
	and	T+2(sp)		; A = mS & 1 when d == 9, else 0: the sticky
	stax	T+5(sp)		; (SL holds it for now)
	ldax	T+2(sp)
	push	a		; the pair {T(sp), A}; every offset below is one more
	ldi	#0x00
shift:
	cmp	T+8(sp)		; TT07: a hit for the dcx
	dcx	T+8(sp)		; d--; C = it was 0
	if	c
	br.p	shifted
	ldc	#0
	shr16	T(sp)
	br	shift
shifted:
	or	T+6(sp)		; the sticky into the low byte
	stax	T+6(sp)		; SL = the low byte of S's significand
	pop	a		; A = its high byte
	stax	T+2(sp)		; (the smaller operand's low byte is done with)
	; same signs: add.  Different signs: subtract (L is the larger).
	ldax	T+1(sp)
	xor	T+3(sp)		; L.hi ^ S.hi, Z = bit 7 ...
	btst	7
	bz	subtract	; Z = bit 7 set = the signs differ
	ldax	T+4(sp)
	add	T+2(sp)		; SH = mL + S.hi, C = carry out
	stax	T+4(sp)
	if	nc
	br.p	round
	; a carry: {1, SH, SL} >> 1 with the bit shifted out kept as sticky, e++
	; (shr16 reaches 0..T+2(sp): the high byte goes through T(sp))
	ldax	T+5(sp)
	andi	#0x01
	stax	T+2(sp)
	ldax	T+4(sp)
	push	a		; every offset below is one more
	ldax	T+6(sp)		; SL
	ldc	#1
	shr16	T(sp)		; T(sp) = {1, SH[7:1]}, A = {SH[0], SL[7:1]}
	or	T+3(sp)		; the sticky
	stax	T+6(sp)		; SL
	pop	a
	stax	T+4(sp)		; SH
	cmp	T+6(sp)		; TT07: a hit for the inx
	inx	T+6(sp)
	br	round
subtract:
	; {SH, SL} = {mL, 0} - {S.hi, SL}: SL = -SL, SH = mL + ~S.hi + (SL == 0)
	ldi	#0xff
	sub	T+2(sp)		; A = ~S.hi
	push	a
	ldax	T+6(sp)		; SL
	cpi	#0x01		; C = SL == 0
	pop	a		; C kept
	adc	#0x00		; + C
	add	T+4(sp)		; + mL (the carry out is the no-borrow, 1)
	stax	T+4(sp)
	ldi	#0x00
	sub	T+5(sp)		; A = -SL (the value is right)
	stax	T+5(sp)
	or	T+4(sp)
	bz	ret_zero	; equal magnitudes
	; normalize: shift the pair left until the hidden bit is back, e--
	; each time (the high byte at T(sp) for shl16, the low byte in A;
	; the test of SH's bit 7 goes through a push of SL, since swap n(sp)
	; addresses sp + n - 512 on the TT07 silicon)
	ldax	T+4(sp)
	push	a		; every offset below is one more
	ldax	T+6(sp)		; A = SL
norm:
	push	a		; SL, SH now at T+1(sp)
	ldax	T+1(sp)		; SH
	btst	7
	bz	normalized	; Z = bit 7 set
	pop	a		; A = SL
	ldc	#0
	shl16	T(sp)		; T(sp) = {SH[6:0], SL[7]}, A = {SL[6:0], 0}
	cmp	T+7(sp)		; TT07: a hit for the dcx
	dcx	T+7(sp)		; e--, Z = it reached 0
	bnz	norm
	ads	#1
	br	ret_szero	; underflow
normalized:
	pop	a		; A = SL
	stax	T+6(sp)		; SL
	pop	a
	stax	T+4(sp)		; SH
round:
	; to nearest even: up when the guard bit is set and (a sticky bit, or the mantissa is odd)
	ldax	T+5(sp)
	btst	7
	bnz	pack		; Z = 0: no guard bit
	andi	#0x7f
	bnz	round_up
	ldax	T+4(sp)
	btst	0
	bnz	pack		; even: stays
round_up:
	cmp	T+4(sp)		; TT07: a hit for the inx
	inx	T+4(sp)		; C = it was 0xff: the mantissa wraps to 1.0, e++
	if	nc
	br.p	pack
	ldi	#0x80
	stax	T+4(sp)
	cmp	T+6(sp)		; TT07: a hit for the inx
	inx	T+6(sp)
pack:
	ldax	T+6(sp)
	cpi	#0xff
	bz	ret_inf		; overflow
	ldc	#0
	shr			; A = e >> 1, C = e & 1
	or	T(sp)		; the sign
	stax	T+9(sp)
	ldax	T+4(sp)
	andi	#0x7f
	if	c
	adc.p	#0x7f		; the low bit of the exponent into bit 7
	stax	T+8(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_L:
	ldax	T(sp)
	stax	T+8(sp)
	ldax	T+1(sp)
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_inf:
	ldi	#0x80
	stax	T+8(sp)
	ldax	T(sp)
	ldc	#0
	adc	#0x7f
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_szero:
	ldax	T(sp)
	stax	T+9(sp)
	ldi	#0x00
	stax	T+8(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_zero:
	ldi	#0x00
	stax	T+8(sp)
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_b_or_nan:
	; b is inf or nan: b, unless a is too - then a, or nan for inf - inf
	ldax	T+6(sp)
	cpi	#0xff
	bnz	ret_b
	ldax	T+10(sp)
	cmp	T+12(sp)		; the low bytes (both 0x80 for infinities)
	bnz	ret_a
	ldax	T+11(sp)
	xor	T+13(sp)
	cpi	#0x80
	bnz	ret_a
	ldi	#0xc0
	stax	T+8(sp)
	ldi	#0x7f
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
a_zero:
	ldax	T+7(sp)
	cpi	#0x00
	bnz	ret_b
	; both zero (or denormal): -0 only when both are negative
	ldax	T+11(sp)
	and	T+13(sp)
	andi	#0x80
	stax	T+9(sp)
	ldi	#0x00
	stax	T+8(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_b:
	ldax	T+12(sp)
	stax	T+8(sp)
	ldax	T+13(sp)
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
ret_a:
	ldax	T+10(sp)
	stax	T+8(sp)
	ldax	T+11(sp)
	stax	T+9(sp)
	ldxx	T+8(sp)			; the result comes back in IX
	ads	#10
	ret
.endif

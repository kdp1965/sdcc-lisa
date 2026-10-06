;--------------------------------------------------------------------------
;  bf16fs.s - __fsadd / __fssub / __fsmul / __fsdiv on the bfloat16 unit,
;  for sdcc -mlisa --bf16-float (the conversions are in bf16fsc.c)
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
; float f(float a, float b): after sra the return slot is 3..6(sp), a is
; 7..10(sp) and b 11..14(sp), low byte first.  A float's bfloat16 is its
; top half, rounded to nearest even by the low half (fs_round); the two
; go on the stack as the arguments of the bf16_t routine, whose result
; becomes the top half of the returned float, the low half zero.

	.area CODE (CODE)

; fs_round: IX -> a float; IX <- its bfloat16, rounded to nearest even
; (a nan is taken as it is: rounding could carry into the exponent).
; The halves go in swapped - the low byte into the high half of IX, bit
; 7 through ix_cond - because push ix puts the high half at the lower
; address, which is where the low byte of a bf16_t argument belongs.
fs_round:
	ldax	3(ix)
	andi	#0x7f
	cpi	#0x7f
	bnz	fs_rnd
	ldax	2(ix)
	btst	7
	bz	fs_take		; the exponent is 0xff: inf or nan
fs_rnd:
	ldax	1(ix)
	cpi	#0x80
	if	c
	br.p	fs_take		; below half an ulp
	bnz	fs_up		; above
	ldax	0(ix)
	cpi	#0x00
	bnz	fs_up		; above
	ldax	2(ix)
	btst	0
	bnz	fs_take		; a tie, even: down
fs_up:
	ldax	2(ix)
	ldc	#1
	adc	#0x00		; + 1
	push	a
	ldax	3(ix)
	adc	#0x00		; + the carry (0x7f7f -> 0x7f80: inf)
	tax
	pop	a
	taxu
	ret
fs_take:
	ldax	2(ix)
	push	a
	ldax	3(ix)
	tax
	pop	a
	taxu
	ret

; the operands onto the stack: b, then a, then the return slot
.macro	ARGS
	sra
	spix
	adx	#11
	jal	fs_round
	push	ix		; b
	spix
	adx	#9
	jal	fs_round
	push	ix		; a
	ads	#-2
.endm

; the bf16_t result at 1,2(sp) into the float's top half (now 9..12(sp))
.macro	RESULT
	ldax	1(sp)
	stax	11(sp)
	ldax	2(sp)
	stax	12(sp)
	ldi	#0x00
	stax	9(sp)
	stax	10(sp)
	ads	#6
	lra
	ret
.endm

	.globl ___fsadd
___fsadd:
	ARGS
	jal	_bf16_add
	RESULT

	.globl ___fssub
___fssub:
	ARGS
	ldax	6(sp)		; b's sign
	ldc	#0
	adc	#0x80
	stax	6(sp)
	jal	_bf16_add
	RESULT

	.globl ___fsmul
___fsmul:
	ARGS
	jal	_bf16_mul
	RESULT

	.globl ___fsdiv
___fsdiv:
	ARGS
	jal	_bf16_div
	RESULT

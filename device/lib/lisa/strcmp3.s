;--------------------------------------------------------------------------
;  strcmp3.s - strcmp on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
;  for packed constants (sdcc -mlisa --rev2 --pack-const: lib/lisa-rev2k)
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

; int strcmp(const char *s1, const char *s2): s1 at 0..2(sp), s2 at 3..5(sp)
; (24-bit pointers, ttsky26d REV3.md); the result, *s1 - *s2 as unsigned
; bytes at the first difference, comes back in IX.  Both are generic
; pointers: each one's space (bit 23, txah's bit 7) is tested once and one
; of four loops runs, a string in RAM read with ldax, one in code space
; with lda (ix++) from its bank.  The cursors are the arguments
; themselves, swapped through IX (ldxx / stxx, adx bumping them).  With
; s2 in RAM *s1 is compared against it in place; with s2 in code space
; *s2 is read first, into a byte pushed for it.  The carry discipline of
; the older chips (C = 0 into every cmp / sub, the borrow of a 0 operand
; forced off) is kept: it costs nothing here.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _strcmp

_strcmp:
	sra				; RA 0..2; s1 3..5; s2 6..8
	ldi	#0x00
	push	a			; a byte for *s2 at 0; RA 1..3; s1 4..6; s2 7..9
	ldc	#0			; C = 0 up to the first cmp
	ldxx	7(sp)
	txah
	btst	7			; Z: s2 is in code space
	bz	s2code
	ldxx	4(sp)
	txah
	btst	7			; Z: s1 is in code space
	bz	cr
; s1 and s2 in RAM
rr:
	ldxx	4(sp)
	ldax	0(ix)			; A = *s1++
	adx	#1
	stxx	4(sp)
	ldxx	7(sp)
	cmp	0(ix)			; - *s2
	bnz	diffm			; A = *s1, IX -> *s2
	adx	#1			; s2++
	stxx	7(sp)
	cpi	#0x00			; the NUL?  (C = 0 again for the next cmp)
	bnz	rr
	br	equal
; s1 in code space, s2 in RAM
cr:
	ldxx	4(sp)
	lda	(ix++)			; A = the byte, IX the next one
	stxx	4(sp)
	ldxx	7(sp)
	cmp	0(ix)			; - *s2
	bnz	diffm
	adx	#1
	stxx	7(sp)
	cpi	#0x00
	bnz	cr
	br	equal
; s2 in code space: *s2 is read first, into the byte at 0(sp)
s2code:
	ldxx	4(sp)
	txah
	btst	7			; Z: s1 is in code space too
	bz	cc
rc:
	ldxx	7(sp)
	lda	(ix++)			; A = the byte, IX the next one
	stxx	7(sp)
	stax	0(sp)
	ldxx	4(sp)
	ldax	0(ix)			; A = *s1++
	adx	#1
	stxx	4(sp)
	cmp	0(sp)			; - *s2
	bnz	difft			; A = *s1, *s2 at 0(sp)
	cpi	#0x00
	bnz	rc
	br	equal
cc:
	ldxx	7(sp)
	lda	(ix++)			; A = the byte, IX the next one
	stxx	7(sp)
	stax	0(sp)
	ldxx	4(sp)
	lda	(ix++)			; A = the byte, IX the next one
	stxx	4(sp)
	cmp	0(sp)
	bnz	difft
	cpi	#0x00
	bnz	cc
equal:
	ldi	#0x00
	tax
	taxu				; IX = 0
	br	exit
; *s1 (A) and *s2 (at 0(ix)) differ: *s2 to 0(sp), where the loops with
; s2 in code space leave it
diffm:
	push	a
	ldax	0(ix)
	stax	1(sp)
	pop	a
difft:
	ldc	#0
	sub	0(sp)			; A = *s1 - *s2, C = the borrow
	tax				; the low byte of the result
	savec
	ldax	0(sp)
	cpi	#0x00			; *s2 == 0: *s1 - 0 cannot borrow (C = 0 here)
	bz	high
	restc				; the borrow
high:
	ifte	c
	ldi	#0xff			; negative: sign-extended
	ldi	#0x00
	taxu
exit:
	ads	#1
	lra
	ret

;--------------------------------------------------------------------------
;  strcmp.s - strcmp for the LISA port
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
; int strcmp(const char *s1, const char *s2): s1 at 1,2(sp), s2 at 3,4(sp);
; the result, *s1 - *s2 as unsigned bytes at the first difference, comes
; back in IX.  Both are generic pointers: each one's space (bit 15,
; ix_cond) is tested once and one of four loops runs, a string in RAM
; read with ldax, one in code space by calling its ldi/ret pairs
; (gptrget.s has the layout; the sra saves RA, so call ix and the jal of
; `pair` may clobber it).  The cursors are the arguments themselves,
; swapped through IX (ldxx / stxx, adx bumping them).  With s2 in RAM
; *s1 is compared against it in place; with s2 in code space *s2 is read
; first, into a byte pushed for it.  On the TT07 a cmp / sub subtracts
; the carry-in too, so C is 0 going into each one (ldc at the start, the
; cpi of the loop after that), and a 0 operand reports a borrow it did
; not make, so the sign of the result is forced positive when *s2 is the
; NUL.

	.area CODE (CODE)

	.globl _strcmp

_strcmp:
	sra				; RA 1,2; s1 3,4; s2 5,6
	ldi	#0x00
	push	a			; a byte for *s2 at 1; RA 2,3; s1 4,5; s2 6,7
	ldc	#0			; C = 0 up to the first cmp
	ldxx	6(sp)
	txau
	btst	7			; Z: s2 is in code space
	bz	s2code
	ldxx	4(sp)
	txau
	btst	7			; Z: s1 is in code space
	bz	s1code
; s1 and s2 in RAM
rr:
	ldxx	4(sp)
	ldax	0(ix)			; A = *s1++
	adx	#1
	stxx	4(sp)
	ldxx	6(sp)
	cmp	0(ix)			; - *s2
	bnz	diffm			; A = *s1, IX -> *s2
	adx	#1			; s2++
	stxx	6(sp)
	cpi	#0x00			; the NUL?  (C = 0 again for the next cmp)
	bnz	rr
	br	equal
; s1 in code space, s2 in RAM
s1code:
	jal	pair			; IX = the word of s1's first ldi/ret pair
	stxx	4(sp)
cr:
	ldxx	4(sp)
	call	ix			; A = *s1, IX the pair's ret
	adx	#1			; the next pair
	stxx	4(sp)
	ldxx	6(sp)
	cmp	0(ix)			; - *s2
	bnz	diffm
	adx	#1
	stxx	6(sp)
	cpi	#0x00
	bnz	cr
	br	equal
; s2 in code space: *s2 is read first, into the byte at 1(sp)
s2code:
	jal	pair			; IX = the word of s2's first ldi/ret pair
	stxx	6(sp)
	ldxx	4(sp)
	txau
	btst	7			; Z: s1 is in code space too
	bz	cc
rc:
	ldxx	6(sp)
	call	ix			; A = *s2, IX the pair's ret
	adx	#1			; the next pair
	stxx	6(sp)
	stax	1(sp)
	ldxx	4(sp)
	ldax	0(ix)			; A = *s1++
	adx	#1
	stxx	4(sp)
	cmp	1(sp)			; - *s2
	bnz	difft			; A = *s1, *s2 at 1(sp)
	cpi	#0x00
	bnz	rc
	br	equal
cc:
	jal	pair			; IX = the word of s1's first ldi/ret pair
	stxx	4(sp)
ccl:
	ldxx	6(sp)
	call	ix			; A = *s2
	adx	#1
	stxx	6(sp)
	stax	1(sp)
	ldxx	4(sp)
	call	ix			; A = *s1
	adx	#1
	stxx	4(sp)
	cmp	1(sp)
	bnz	difft
	cpi	#0x00
	bnz	ccl
equal:
	ldi	#0x00
	tax
	taxu				; IX = 0
	br	exit
; *s1 (A) and *s2 (at 0(ix)) differ: *s2 to 1(sp), where the loops with
; s2 in code space leave it
diffm:
	push	a
	ldax	0(ix)
	stax	2(sp)
	pop	a
difft:
	ldc	#0			; sub subtracts the carry-in too (TT07)
	sub	1(sp)			; A = *s1 - *s2, C = the borrow - spurious when *s2 == 0 (TT07)
	tax				; the low byte of the result
	savec
	ldax	1(sp)
	cpi	#0x00			; *s2 == 0: *s1 - 0 cannot borrow (C = 0 here)
	bz	high
	restc				; the borrow
high:
	ifte	c
	ldi	#0xff			; negative: sign-extended (ix_cond is bit 15 of the result)
	ldi	#0x00
	taxu
exit:
	ads	#1
	lra
	ret
; IX = the word of the first ldi/ret pair of the code pointer in IX
pair:
	txau
	andi	#0x7f
	addaxu
	txa
	addax
	ret

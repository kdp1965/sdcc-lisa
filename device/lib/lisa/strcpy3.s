;--------------------------------------------------------------------------
;  strcpy3.s - strcpy on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
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

; char *strcpy(char *d, const char *s): d at 0..2(sp), s at 3..5(sp) (24-bit
; pointers, ttsky26d REV3.md); d comes back in IX.  s is generic: its
; space (bit 23, txah's bit 7) is tested once and one of two loops runs,
; reading RAM or the packed constants of its bank with lda (ix++).  Both
; keep the cursors on the stack and swap them through IX (ldxx / stxx,
; adx bumping them), and copy the NUL before testing it.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _strcpy

_strcpy:
	sra				; RA 0..2; d 3..5; s 6..8
	ads	#-3
	ldxx	6(sp)
	stxx	0(sp)			; the destination cursor at 0..2; RA 3..5; d 6..8; s 9..11
	ldxx	9(sp)
	txah
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)			; A = *s++
	adx	#1
	stxx	9(sp)
	ldxx	0(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	0(sp)
	ldxx	9(sp)
	cpi	#0x00
	bnz	ram
	br	done
code:
	lda	(ix++)			; A = the byte, IX the next one
	stxx	9(sp)
	ldxx	0(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	0(sp)
	ldxx	9(sp)
	cpi	#0x00
	bnz	code
done:
	ldxx	6(sp)			; d
	ads	#3
	lra
	ret

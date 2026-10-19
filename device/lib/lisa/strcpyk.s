;--------------------------------------------------------------------------
;  strcpy.s - strcpy for the LISA port
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
; This is the packed-constant variant (sdcc --pack-const, lib/lisa-rev2k,
; ttsky26d RESPIN 26): a byte of constant data is a byte of the code word
; at {KB, p >> 1} in bank KB, read by lda (ix++), which leaves IX on the next
; one; a pointer to it is the byte offset, so there is no conversion.
;
; char *strcpy(char *d, const char *s): d at 1,T+1(sp), s at 3,T+3(sp); d
; comes back in IX.  s is generic: its space (bit 15, ix_cond) is tested
; once and one of two loops runs, reading RAM or calling the ldi/ret pairs
; that hold constant data in code space (gptrget.s has the layout; the sra
; saves RA, so call ix may clobber it).  Both keep the cursors on the
; stack and swap them through IX (ldxx / stxx, adx bumping them), and copy
; the NUL before testing it.

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _strcpy

_strcpy:
	sra				; RA 1,2; d 3,4; s 5,6
	ldax	T+3(sp)
	push	a
	ldax	T+3(sp)
	push	a			; the destination cursor at 1,2; RA 3,4; d 5,6; s 7,8
	ldxx	T+6(sp)
	txau
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)			; A = *s++
	adx	#1
	stxx	T+6(sp)
	ldxx	T(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	T(sp)
	ldxx	T+6(sp)
	cpi	#0x00
	bnz	ram
	br	done
code:
					; IX = the byte offset of s's first byte (packed)
codeloop:
	lda	(ix++)			; A = the byte, IX the next one
	stxx	T+6(sp)
	ldxx	T(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	T(sp)
	ldxx	T+6(sp)
	cpi	#0x00
	bnz	codeloop
done:
	ldxx	T+4(sp)			; d
	ads	#2
	lra
	ret

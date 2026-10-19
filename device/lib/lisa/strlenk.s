;--------------------------------------------------------------------------
;  strlen.s - strlen for the LISA port
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
; size_t strlen(const char *s): s at 1,T+1(sp), the length comes back in
; IX.  s is generic: its space (bit 15, ix_cond) is tested once.  A string
; in RAM is walked in IX alone and the length is the end minus s (subax /
; subaxu; IX arithmetic sets ix_cond, cleared again through taxu, as the
; caller stores IX with its tag as bit 15).  A string in code space is the
; ldi/ret pairs gptrget.s describes, called one by one (RA saved first);
; the length is half the words walked, shifted down on the stack (shr16,
; with the tag masked off and C cleared first so that neither amode shifts
; anything in).

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _strlen

_strlen:
	ldxx	T(sp)
	txau
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)
	adx	#1
	cpi	#0x00
	bnz	ram			; IX = s + len + 1
	ldax	T(sp)
	subax
	ldax	T+1(sp)
	subaxu
	adx	#-1			; IX = len, ix_cond set
	txau
	andi	#0x7f
	taxu				; ix_cond cleared
	ret
code:
	sra				; RA 1,2; s 3,4
					; IX = the byte offset of s's first byte (packed)
	push	ix			; at 1,2: the high byte (bit 7 its tag) above the low
codeloop:
	lda	(ix++)			; A = the byte, IX the next one
	cpi	#0x00
	bnz	codeloop		; IX = the first byte + len + 1
	ldax	T+1(sp)
	subax
	ldax	T(sp)
	andi	#0x7f
	subaxu
	adx	#-1			; IX = len, ix_cond set
	txau
	andi	#0x7f
	taxu				; ix_cond cleared
	ads	#2
	lra
	ret

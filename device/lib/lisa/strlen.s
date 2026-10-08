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
; size_t strlen(const char *s): s at 1,2(sp), the length comes back in
; IX.  s is generic: its space (bit 15, ix_cond) is tested once.  A string
; in RAM is walked in IX alone and the length is the end minus s (subax /
; subaxu; IX arithmetic sets ix_cond, cleared again through taxu, as the
; caller stores IX with its tag as bit 15).  A string in code space is the
; ldi/ret pairs gptrget.s describes, called one by one (RA saved first);
; the length is half the words walked, shifted down on the stack (shr16,
; with the tag masked off and C cleared first so that neither amode shifts
; anything in).

	.area CODE (CODE)

	.globl _strlen

_strlen:
	ldxx	1(sp)
	txau
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)
	adx	#1
	cpi	#0x00
	bnz	ram			; IX = s + len + 1
	ldax	1(sp)
	subax
	ldax	2(sp)
	subaxu
	adx	#-1			; IX = len, ix_cond set
	txau
	andi	#0x7f
	taxu				; ix_cond cleared
	ret
code:
	sra				; RA 1,2; s 3,4
	txau
	andi	#0x7f
	addaxu
	txa
	addax				; IX = the word of s's first ldi/ret pair
	push	ix			; at 1,2: the high byte (bit 7 its tag) above the low
codeloop:
	call	ix			; A = the byte, IX the pair's ret
	adx	#1			; the next pair
	cpi	#0x00
	bnz	codeloop		; IX = the first word + 2 * (len + 1)
	ldax	2(sp)
	subax
	ldax	1(sp)
	andi	#0x7f
	subaxu
	adx	#-2			; IX = 2 * len
	stxx	1(sp)			; the low byte at 1, the high (bit 7 ix_cond) at 2
	ldax	2(sp)
	andi	#0x7f
	stax	2(sp)
	ldc	#0
	ldax	1(sp)
	shr16	2(sp)			; {2(sp), A} >>= 1: len
	tax
	ldax	2(sp)
	taxu
	ads	#2
	lra
	ret

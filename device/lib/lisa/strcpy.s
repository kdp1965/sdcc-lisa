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
; char *strcpy(char *d, const char *s): d at 1,2(sp), s at 3,4(sp); d
; comes back in IX.  s is generic: its space (bit 15, ix_cond) is tested
; once and one of two loops runs, reading RAM or calling the ldi/ret pairs
; that hold constant data in code space (gptrget.s has the layout; the sra
; saves RA, so call ix may clobber it).  Both keep the cursors on the
; stack and swap them through IX (ldxx / stxx, adx bumping them), and copy
; the NUL before testing it.

	.area CODE (CODE)

	.globl _strcpy

_strcpy:
	sra				; RA 1,2; d 3,4; s 5,6
	ldax	4(sp)
	push	a
	ldax	4(sp)
	push	a			; the destination cursor at 1,2; RA 3,4; d 5,6; s 7,8
	ldxx	7(sp)
	txau
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)			; A = *s++
	adx	#1
	stxx	7(sp)
	ldxx	1(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	1(sp)
	ldxx	7(sp)
	cpi	#0x00
	bnz	ram
	br	done
code:
	txau
	andi	#0x7f
	addaxu
	txa
	addax				; IX = the word of s's first ldi/ret pair
codeloop:
	call	ix			; A = the byte, IX the pair's ret
	adx	#1			; the next pair
	stxx	7(sp)
	ldxx	1(sp)
	stax	0(ix)			; *d++ = A
	adx	#1
	stxx	1(sp)
	ldxx	7(sp)
	cpi	#0x00
	bnz	codeloop
done:
	ldxx	5(sp)			; d
	ads	#2
	lra
	ret

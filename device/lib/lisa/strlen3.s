;--------------------------------------------------------------------------
;  strlen3.s - strlen on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
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

; size_t strlen(const char *s): s at 0..2(sp) (a 24-bit pointer, ttsky26d
; REV3.md), the length comes back in IX (its low 16 bits).  s is generic:
; its space (bit 23, txah's bit 7) is tested once; a string in RAM is
; walked with ldax, one in code space with lda (ix++) from its bank -
; both a byte per byte, so the length is the end minus s either way
; (subax / subaxu on the low 16 bits; what is left above is not read).
; A leaf: nothing here touches RA.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _strlen

_strlen:
	ldxx	0(sp)
	txah
	btst	7			; Z: s is in code space
	bz	code
ram:
	ldax	0(ix)
	adx	#1
	cpi	#0x00
	bnz	ram			; IX = s + len + 1
	br	len
code:
	lda	(ix++)			; A = the byte, IX the next one
	cpi	#0x00
	bnz	code
len:
	ldax	0(sp)
	subax
	ldax	1(sp)
	subaxu
	adx	#-1			; IX = len
	ret

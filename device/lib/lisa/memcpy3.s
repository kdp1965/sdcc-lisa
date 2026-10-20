;--------------------------------------------------------------------------
;  memcpy3.s - memcpy / __memcpy on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
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

; void *__memcpy(void *dst, const void *src, size_t n) - and memcpy, the
; same code (the compiler calls __memcpy for a memcpy(); the name is for
; pointers to it).  The third chip (ttsky26d REV3.md): pointers are 24
; bits, three bytes little-endian on the stack, which ldxx / stxx move
; whole; a generic pointer's bit 23 (txah reads it as bit 7) marks
; constant data in code space, read by lda (ix++) from the pointer's own
; bank.  At entry dst is at 0..2(sp), src at 3..5(sp), n at 6..7(sp); dst
; comes back in IX.  The source's space is tested once and one of two
; loops runs; both keep the cursors on the stack, swapped through IX,
; and count n down in place - its low byte first, then 256 per high
; byte, which gets one more when the low byte's pass is a partial one.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl ___memcpy, _memcpy

_memcpy:
___memcpy:
	sra				; RA 0..2; dst 3..5; src 6..8; n 9..10
	ads	#-3
	ldxx	6(sp)
	stxx	0(sp)			; the destination cursor at 0..2; RA 3..5; dst 6..8; src 9..11; n 12..13
	ldax	12(sp)			; n's low byte
	bz	nlo0
	inx	13(sp)			; a partial pass of the low byte: one more of the high
	br	go
nlo0:
	or	13(sp)
	bz	done			; n == 0
go:
	ldxx	9(sp)
	txah
	btst	7			; Z: src is in code space
	bz	code
ram:
	ldax	0(ix)			; A = *src++
	adx	#1
	stxx	9(sp)
	ldxx	0(sp)
	stax	0(ix)			; *dst++ = A
	adx	#1
	stxx	0(sp)
	ldxx	9(sp)
	dcx	12(sp)
	bnz	ram
	dcx	13(sp)
	bnz	ram
	br	done
code:
	lda	(ix++)			; A = the byte, IX the next one
	stxx	9(sp)
	ldxx	0(sp)
	stax	0(ix)			; *dst++ = A
	adx	#1
	stxx	0(sp)
	ldxx	9(sp)
	dcx	12(sp)
	bnz	code
	dcx	13(sp)
	bnz	code
done:
	ldxx	6(sp)			; dst
	ads	#3
	lra
	ret

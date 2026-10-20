;--------------------------------------------------------------------------
;  memset3.s - memset on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
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

; void *memset(void *s, int c, size_t n): s at 0..2(sp) (a 24-bit pointer,
; ttsky26d REV3.md), c at 3..4(sp) (its low byte is the fill), n at
; 5..6(sp); s comes back in IX.  A leaf: RA is not saved.  The pointer
; stays in IX (adx) and the fill byte in A: the count comes down on the
; stack, where dcx leaves A alone - its low byte first, then 256 per high
; byte, which gets one more when the low byte's pass is a partial one.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl _memset

_memset:
	ldax	5(sp)			; n's low byte
	bz	nlo0
	inx	6(sp)			; a partial pass of the low byte: one more of the high
	br	go
nlo0:
	or	6(sp)
	bz	done			; n == 0
go:
	ldxx	0(sp)
	ldax	3(sp)			; the fill byte, in A throughout
loop:
	stax	0(ix)
	adx	#1
	dcx	5(sp)
	bnz	loop
	dcx	6(sp)
	bnz	loop
done:
	ldxx	0(sp)			; s
	ret

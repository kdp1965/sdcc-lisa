;--------------------------------------------------------------------------
;  memcpy.s - memcpy / __memcpy for the LISA port
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
; void *__memcpy(void *dst, const void *src, size_t n) - and memcpy, the
; same code (the compiler calls __memcpy for a memcpy(); the name is for
; pointers to it).  At entry dst is at 1,T+1(sp), src at 3,T+3(sp), n at
; 5,T+5(sp); dst comes back in IX.  src is a generic pointer: its space (bit
; 15, ix_cond) is tested once and one of two loops runs, reading RAM or
; calling the ldi/ret pairs that hold constant data in code space
; (gptrget.s has the layout; the sra saves RA, so call ix may clobber it).
; Both keep the cursors on the stack and swap them through IX (ldxx /
; stxx, adx bumping them), and count n down in place - its low byte first,
; then 256 per high byte, which gets one more when the low byte's pass is
; a partial one.  lisamodel.inc (device/lib/lisa/Makefile.in) says whether
; this is the --tt07-cache library, where each inx / dcx's byte is brought
; into the data cache with a cmp first (a read-modify-write on a miss
; works on stale data there).

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl ___memcpy, _memcpy

_memcpy:
___memcpy:
	sra				; RA 1,2; dst 3,4; src 5,6; n 7,8
	ldax	T+3(sp)
	push	a
	ldax	T+3(sp)
	push	a			; the destination cursor at 1,2; RA 3,4; dst 5,6; src 7,8; n 9,10
	ldax	T+8(sp)			; n's low byte
	bz	nlo0
	.if TT07_CACHE
	cmp	T+9(sp)
	.endif
	inx	T+9(sp)			; a partial pass of the low byte: one more of the high
	br	go
nlo0:
	or	T+9(sp)
	bz	done			; n == 0
go:
	ldxx	T+6(sp)
	txau
	btst	7			; Z: src is in code space
	bz	code
ram:
	ldax	0(ix)			; A = *src++
	adx	#1
	stxx	T+6(sp)
	ldxx	T(sp)
	stax	0(ix)			; *dst++ = A
	adx	#1
	stxx	T(sp)
	ldxx	T+6(sp)
	.if TT07_CACHE
	cmp	T+8(sp)
	.endif
	dcx	T+8(sp)
	bnz	ram
	.if TT07_CACHE
	cmp	T+9(sp)
	.endif
	dcx	T+9(sp)
	bnz	ram
	br	done
code:
	txau
	andi	#0x7f
	addaxu
	txa
	addax				; IX = the word of src's first ldi/ret pair
codeloop:
	call	ix			; A = the byte, IX the pair's ret
	adx	#1			; the next pair
	stxx	T+6(sp)
	ldxx	T(sp)
	stax	0(ix)			; *dst++ = A
	adx	#1
	stxx	T(sp)
	ldxx	T+6(sp)
	.if TT07_CACHE
	cmp	T+8(sp)
	.endif
	dcx	T+8(sp)
	bnz	codeloop
	.if TT07_CACHE
	cmp	T+9(sp)
	.endif
	dcx	T+9(sp)
	bnz	codeloop
done:
	ldxx	T+4(sp)			; dst
	ads	#2
	lra
	ret

;--------------------------------------------------------------------------
;  setjmp.s - setjmp() / longjmp() for the LISA port
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
; jmp_buf is 4 bytes: [0..1] the SP at entry to setjmp, [2..3] the return
; address RA (high byte carries ra_cond).
;
; Frame at entry to int __setjmp(jmp_buf buf), before any push (the int
; result comes back in IX):
;   T(sp),T+1(sp)  buf
; Frame at entry to void longjmp(jmp_buf buf, int val):
;   T(sp),T+1(sp)  buf
;   T+2(sp),T+3(sp)  val

;
; Stack offsets: T(sp) is the byte on top of the stack, T+n(sp) the nth
; below it; T comes from lisamodel.inc (1 on TT07, where a push writes at
; SP and then decrements it, 0 on the second chip, where it decrements
; first: sdcc --rev2, the lisa-rev2 library).
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl ___setjmp
___setjmp:
	spix				; IX = SP at entry
	txa
	push	a			; SP low
	txau
	andi	#0x7f
	push	a			; SP high
	xchg	ra			; IX = RA (with ra_cond in bit 15)
	txa
	push	a			; RA low
	txau
	push	a			; RA high + cond
	xchg	ra			; RA restored
	ldxx	T+4(sp)			; IX = buf (T(sp) + the 4 pushed bytes)
	pop	a
	stax	3(ix)			; RA high
	pop	a
	stax	2(ix)			; RA low
	pop	a
	stax	1(ix)			; SP high
	pop	a
	stax	0(ix)			; SP low
	ldi	#0x00			; return 0, in IX
	tax
	taxu
	ret

	.globl _longjmp
_longjmp:
	; restore RA first, while still on this stack
	ldxx	T(sp)			; IX = buf
	ldax	3(ix)
	push	a			; RA high + cond
	ldax	2(ix)
	push	a			; RA low: T(sp) low, T+1(sp) high, as ldxx wants
	ldxx	T(sp)
	xchg	ra			; RA = saved RA (with ra_cond), IX = junk
	ads	#2
	ldxx	T(sp)			; IX = buf
	; val goes into buf[2..3] (the RA bytes are no longer needed)
	ldax	T+2(sp)
	stax	2(ix)
	ldax	T+3(sp)
	stax	3(ix)
	; IX = saved SP
	ldax	0(ix)
	push	a
	ldax	1(ix)
	taxu
	pop	a
	tax
	xchg	sp			; SP = setjmp's entry SP (S1), IX = this frame's SP (S0)
	; this frame's buf pointer is at T(ix),T+1(ix) (the frame's T(sp) and
	; T+1(sp) through S0).  Either S0 == S1 (same frame) or S0 <= S1 - 3,
	; so the push below never lands on them.
	ldax	T+1(ix)
	push	a			; buf high
	ldax	T(ix)
	tax
	pop	a
	taxu				; IX = buf
	ldax	3(ix)
	push	a			; val high
	ldax	2(ix)
	or	3(ix)			; Z: val == 0
	ifte	z
	ldi.p	#0x01			; longjmp(buf, 0) makes setjmp return 1
	ldax.p	2(ix)
	tax				; setjmp returns val, in IX
	pop	a
	taxu
	ret

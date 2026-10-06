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
; Frame at entry to int __setjmp(jmp_buf buf), before any push:
;   1(sp),2(sp)  return slot (int, low byte first)
;   3(sp),4(sp)  buf
; Frame at entry to void longjmp(jmp_buf buf, int val):
;   1(sp),2(sp)  buf
;   3(sp),4(sp)  val

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
	ldxx	7(sp)			; IX = buf (3(sp) + the 4 pushed bytes)
	pop	a
	stax	3(ix)			; RA high
	pop	a
	stax	2(ix)			; RA low
	pop	a
	stax	1(ix)			; SP high
	pop	a
	stax	0(ix)			; SP low
	ldi	#0x00			; return 0
	stax	1(sp)
	stax	2(sp)
	ret

	.globl _longjmp
_longjmp:
	; restore RA first, while still on this stack
	ldxx	1(sp)			; IX = buf
	ldax	3(ix)
	push	a			; RA high + cond
	ldax	2(ix)
	push	a			; RA low: 1(sp) low, 2(sp) high, as ldxx wants
	ldxx	1(sp)
	xchg	ra			; RA = saved RA (with ra_cond), IX = junk
	ads	#2
	ldxx	1(sp)			; IX = buf
	; val goes into buf[2..3] (the RA bytes are no longer needed)
	ldax	3(sp)
	stax	2(ix)
	ldax	4(sp)
	stax	3(ix)
	; IX = saved SP
	ldax	0(ix)
	push	a
	ldax	1(ix)
	taxu
	pop	a
	tax
	xchg	sp			; SP = setjmp's entry SP (S1), IX = this frame's SP (S0)
	; this frame's buf pointer is at 1(ix),2(ix).  Either S0 == S1 (same
	; frame) or S0 <= S1 - 3, so the push below never lands on them.
	ldax	2(ix)
	push	a			; buf high
	ldax	1(ix)
	tax
	pop	a
	taxu				; IX = buf
	ldax	2(ix)
	stax	1(sp)			; setjmp's return slot = val
	ldax	3(ix)
	stax	2(sp)
	or	2(ix)
	bnz	1$
	ldi	#0x01			; longjmp(buf, 0) makes setjmp return 1
	stax	1(sp)
1$:
	ret

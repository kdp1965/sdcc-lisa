;--------------------------------------------------------------------------
;  setjmp3.s - setjmp / longjmp on the third chip (sdcc -mlisa --rev3: lib/lisa-rev3)
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

; The third chip (ttsky26d REV3.md): SP and RA are 24 bits, so jmp_buf is
; 8 bytes - [0..2] the SP at entry to setjmp, [3..5] the return address
; RA (its top byte carries the condition bit), [6..7] scratch for
; longjmp's val.  push ix / pop ix move three bytes with the top byte
; (the condition bit) at the lower address; the pushes below put the
; bytes in that order for pop ix.
;
; Frame at entry to int __setjmp(jmp_buf buf): 0..2(sp) buf (the int
; result comes back in IX).  Frame at entry to void longjmp(jmp_buf buf,
; int val): 0..2(sp) buf, 3..4(sp) val.
	.include "lisamodel.inc"

	.area CODE (CODE)

	.globl ___setjmp
___setjmp:
	spix				; IX = SP at entry
	push	ix			; its three bytes, the top one on top
	xchg	ra			; IX = RA
	push	ix
	xchg	ra			; RA restored
	ldxx	6(sp)			; IX = buf (0(sp) + the 6 pushed bytes)
	pop	a
	stax	5(ix)			; RA's top byte (the condition bit in bit 7)
	pop	a
	stax	4(ix)
	pop	a
	stax	3(ix)			; RA low
	pop	a
	stax	2(ix)			; SP's top byte
	pop	a
	stax	1(ix)
	pop	a
	stax	0(ix)			; SP low
	ldi	#0x00			; return 0, in IX
	tax
	taxu
	ret

	.globl _longjmp
_longjmp:
	ldxx	0(sp)			; IX = buf
	ldax	3(sp)
	or	4(sp)
	bnz	1$
	ldi	#0x01			; longjmp(buf, 0) makes setjmp return 1
	stax	3(sp)
1$:	ldax	3(sp)
	stax	6(ix)			; buf[6..7] = val: read again once the stack has switched
	ldax	4(sp)
	stax	7(ix)
	ldax	0(ix)			; the saved SP: low first, pop ix takes the top byte from the top
	push	a
	ldax	1(ix)
	push	a
	ldax	2(ix)
	push	a
	xchg	ra			; RA = buf (the return address is not needed)
	pop	ix			; IX = the saved SP
	xchg	sp			; SP = setjmp's entry SP; IX = this frame's, no longer needed
	xchg	ra			; IX = buf
	ldax	6(ix)			; val: low first
	push	a
	ldax	7(ix)
	push	a
	ldax	3(ix)			; the saved RA: low first, its top byte on top
	push	a
	ldax	4(ix)
	push	a
	ldax	5(ix)
	push	a
	pop	ix			; IX = the saved RA
	xchg	ra			; RA = it, the condition bit with it
	pop	a			; val high
	taxu
	pop	a			; val low
	tax				; IX = val: setjmp's result
	ret

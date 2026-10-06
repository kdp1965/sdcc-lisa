;--------------------------------------------------------------------------
;  gptrget.s - reading through generic and code pointers, out of line
;  (sdcc -mlisa emits these calls unless --opt-code-speed)
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
; A generic pointer has its code-space tag in bit 15, which is ix_cond
; once it is in IX (txau reads it as bit 7).  A byte of constant data in
; code space is an `ldi #byte ; ret` pair at word 2 * (p & 0x7fff), read
; by calling it.  No IX arithmetic keeps ix_cond, so after the first
; byte the pointer is kept raw - the data address, or the word of the
; next pair - and the space in C, which nothing between the calls of a
; multi-byte read touches (stax, sta, push ix / ldx / pop ix).
;
; All of them: A <- the byte, IX <- the raw address of the next one,
; C <- 1 for code space.  RA is the caller's return address (jal).

	.area CODE (CODE)

	.globl __gptrget, __gptrgeto, __gptrcode, __gptrword, __gptrnext

; IX = p + off (off in A, 0..127), p generic
__gptrgeto:
	push	a
	txau
	btst	7			; Z = bit 7: code space
	bz	1$
	pop	a
	addax
	br	__gptrdata
1$:	txau
	andi	#0x7f
	addaxu
	txa
	addax				; IX = the word of the first pair
	pop	a
	addax
	addax				; + 2 * off
	br	__gptrword

; IX = p, generic
__gptrget:
	txau
	btst	7
	bz	__gptrcode
__gptrdata:
	ldax	0(ix)
	adx	#1
	ldc	#0
	ret

; IX = p, a code pointer (the tag, if any, is dropped)
__gptrcode:
	txau
	andi	#0x7f
	addaxu
	txa
	addax
; IX = the word of an ldi/ret pair
__gptrword:
	sra
	call	ix			; A = the byte, IX = the ret of the pair
	lra
	adx	#1
	ldc	#1
	ret

; the byte after the last one read: IX raw and C from that read
__gptrnext:
	if	c
	br.p	__gptrword
	ldax	0(ix)
	adx	#1
	ret

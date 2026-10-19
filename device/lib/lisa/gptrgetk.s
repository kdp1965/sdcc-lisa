;--------------------------------------------------------------------------
;  gptrgetk.s - reading through generic and code pointers, out of line,
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
;--------------------------------------------------------------------------
;
; The second chip's lda (ix++) (ttsky26d RESPIN 26) reads byte IX[0] of
; the code word at {KB, IX[14:1]}: constant data is packed two bytes to a
; word in the bank KB (0x21F, set by the startup from k_CONST), a pointer
; to it the byte offset in that bank with the code-space tag in bit 15 -
; which is ix_cond once it is in IX (txau reads it as bit 7) and which
; the instruction ignores.  No conversion, no call: the byte comes with
; IX already on the next one.
;
; All of them: A <- the byte, IX <- the address of the next one, C <- 1
; for code space.  RA is the caller's return address (jal).
	.area CODE (CODE)
	.globl __gptrget, __gptrgeto, __gptrcode, __gptrcodeo, __gptrword, __gptrnext, __gptrprev
; IX = p + off (off in A, 0..127), p generic
__gptrgeto:
	push	a
	txau
	btst	7			; Z = bit 7: code space
	bz	gptrcodeoff
	pop	a
	addax
	br	__gptrdata
; IX = p + off (off in A, 0..127), p a code pointer
__gptrcodeo:
	push	a
gptrcodeoff:
	pop	a
	addax				; IX = p + off (the tag stays in ix_cond)
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

; IX = p, a code pointer
__gptrcode:
; IX = the byte's address in bank KB
__gptrword:
	lda	(ix++)			; A = the byte, IX = the next one
	ldc	#1
	ret

; the byte after the last one read: IX raw and C from that read
__gptrnext:
	if	c
	br.p	__gptrword
	ldax	0(ix)
	adx	#1
	ret

; the byte before the last one read (a struct pushed high byte first):
; IX raw and C from that read
__gptrprev:
	adx	#-2			; back over the byte just read and the one before
	if	c
	br.p	__gptrword
	ldax	0(ix)
	adx	#1
	ret

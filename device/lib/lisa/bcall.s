;--------------------------------------------------------------------------
;  bcall.s - ___sdcc_bcall(): the call into a code bank of the second LISA
;  chip (ttsky26d-um-lisa-ttlc RESPIN.md item 22, lisa_isa.md "Bank
;  registers"; nothing on TT07).  The compiler calls a __banked function as
;
;       push a / ldi #b_fn / swap 0(sp)   ; the bank byte under A's parameter
;       ldx #_fn                          ; the function's window address
;       jal ___sdcc_bcall
;
;  so on entry 0(sp) holds the callee's bank, IX the function, A the first
;  byte parameter and RA the return to the caller.  The callee finds the
;  caller's RA at 0(sp) / 1(sp), the caller's code bank at 2(sp), its return
;  slot and parameters from 3(sp) up (genFunction adds the 3), and returns
;  with ret, the result in A / IX, which this routine leaves alone.  The
;  caller drops the bank byte with the parameters.  Lives in HOME: the
;  common lower 16K words, reachable from every bank.
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

	.include "lisamodel.inc"

	.area HOME (CODE)

	.globl ___sdcc_bcall

___sdcc_bcall:
	sra			; the caller's RA: [ra][ra][bank] ..
	push a			; the first byte parameter: [a][ra][ra][bank] ..
	ldax T+3(sp)		; the callee's bank
	swapi 0x215		; .. into the code bank register, the caller's out
	stax T+3(sp)		; .. kept where the bank byte was
	pop a
	call ix			; the callee: 0(sp) RA, 2(sp) the caller's bank, 3(sp) its arguments
	push a			; the result byte
	ldax T+3(sp)		; the caller's bank
	sta 0x215		; .. restored
	pop a
	lra
	ret

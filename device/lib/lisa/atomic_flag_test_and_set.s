;--------------------------------------------------------------------------
;  atomic_flag_test_and_set.s - for the LISA port
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
; _Bool atomic_flag_test_and_set(volatile atomic_flag *object)
; The flag byte is 1 when clear (ATOMIC_FLAG_INIT) and 0 when set; swap is
; the atomic exchange.  The pointer is at 1(sp),2(sp); the result is in A.

	.area CODE (CODE)

	.globl _atomic_flag_test_and_set
_atomic_flag_test_and_set:
	ldxx	1(sp)
	ldi	#0x00
	swap	0(ix)			; A = old flag, flag = 0 (set)
	cpi	#0x00
	ldac	z			; 1 if it was already set
	ret

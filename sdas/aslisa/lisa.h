/* lisa.h */

/*
 *  Copyright (C) 1998-2009  Alan R. Baldwin
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Alan R. Baldwin
 * 721 Berkeley St.
 * Kent, Ohio  44240
 *
 *   LISA (Little ISA) 8-bit core, 16-bit opcodes.
 *   Ken Pettit / tt07-um-lisa-ttlc
 */

/*
 * Instruction classes (mne.m_type).  m_valu holds the base opcode.
 */
#define S_INH     50    /* no operand:  nop, ret, sra, tax ...            */
#define S_IMM8    51    /* 8-bit immediate: ldi, adc, cpi, andi, reti      */
#define S_SIMM10  52    /* 10-bit signed immediate: ads, adx               */
#define S_IDX     53    /* n(ix) / n(sp): add, sub, ldax, stax, inx ...    */
#define S_SPW     54    /* n(sp) only: ldxx, stxx                          */
#define S_DIR     55    /* [p:abs9] direct: lda, sta, swapi                */
#define S_BRA     56    /* rel11 branch: br, bz, bnz                       */
#define S_JAL     57    /* 15-bit word address: jal                        */
#define S_LDX     58    /* two-word: ldx                                   */
#define S_IF      59    /* if/iftt/ifte cond                               */
#define S_LDAC    60    /* ldac cond                                       */
#define S_BIT     61    /* 1 bit in inst[0]: ldc, eidi, tfa, taf           */
#define S_U2      62    /* 2 bits in inst[1:0]: shl16, shr16, div, rem, f* */
#define S_U3      63    /* 3 bits in inst[2:0]: btst, amode                */
#define S_LDZ     64    /* ldz 0|1|notz|c                                  */
#define S_XCHG    65    /* xchg ra|ia|sp                                   */
#define S_CPX     66    /* cpx ra|sp                                       */
#define S_PUSH    67    /* push a|ix                                       */
#define S_POP     68    /* pop a|ix                                        */
#define S_CALL    69    /* call ix                                         */
#define S_JMP     70    /* jmp ix                                          */
#define S_RET     71    /* ret  [#imm8]                                    */
#define S_LDDIV   72    /* lddiv n(sp): two words, the offset of the high byte   */
#define S_DIVREM  73    /* div/rem dv[, n(sp)]: offset word unless dv bit 0 is set */
#define S_LDXS    74    /* ldxs #expr: ldx with the literal word byte-swapped       */

/*
 * Addressing modes returned by addr().
 */
#define S_IMM     31    /* #expr or bare expr                              */
#define S_SPO     32    /* n(sp)                                           */
#define S_IXO     33    /* n(ix)                                           */
#define S_REG     34    /* a register name, in e_addr                      */

#define R_A       0
#define R_IX      1
#define R_SP      2
#define R_RA      3
#define R_IA      4

/* Opcodes shared by lisamch.c and the data-in-code expansion in asout.c. */
#define LISA_OP_LDI   0x8000
#define LISA_OP_RET   0x8A00

#ifdef OTHERSYSTEM
extern int addr(struct expr *esp);
extern int lisacond(void);
extern int lisareg(void);
#else
extern int addr();
extern int lisacond();
extern int lisareg();
#endif

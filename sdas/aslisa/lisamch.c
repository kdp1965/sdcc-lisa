/* lisamch.c */

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
 *
 *   Code is counted in bytes (two per instruction word, little endian);
 *   the linker converts code addresses to word addresses, see
 *   linksrc/lkrloc3.c (TARGET_IS_LISA).
 */

#include "sdas.h"
#include "asxxxx.h"
#include "lisa.h"

char    *cpu    = "LISA";
char    *dsft   = "asm";

/*
 * Set while machine() emits instruction words, so that the data-in-code
 * expansion in asout.c (ldi/ret pairs for .db in a code area) stays off.
 * Defined in asout.c.
 */
extern int lisa_raw;

/*
 * Immediate operand that may be a byte of a relocatable address
 * (ldi #<_sym, ldi #>_sym).  Emits the low byte (relocated) then the
 * high byte of the opcode.
 */
static VOID
immbyte(struct expr *e, a_uint op, int lo, int hi)
{
        int v;

        if (e->e_flag == 0 && e->e_base.e_ap == NULL) {
                v = (int) e->e_addr;
                if (v < lo || v > hi)
                        aerr();
                outaw(op | (v & 0xFF));
        } else {
                outrb(e, 0);
                outab(op >> 8);
        }
}

/*
 * Parse one of the two-bit operands: ii/ic/ci/cc (div, rem), f0..f3
 * (bf16), n(sp) or a number.
 */
static int
twobits(void)
{
        static const struct { const char *n; int v; } tab[] = {
                { "ii", 0 }, { "ic", 1 }, { "ci", 2 }, { "cc", 3 },
                { "f0", 0 }, { "f1", 1 }, { "f2", 2 }, { "f3", 3 },
                { NULL, 0 }
        };
        struct expr e;
        char id[NCPS];
        char *save;
        int c, i;

        c = getnb();
        if (ctype[c] & LETTER) {
                save = ip;
                getid(id, c);
                for (i = 0; tab[i].n; i++)
                        if (symeq(id, tab[i].n, 1))
                                return tab[i].v;
                ip = save - 1;
                c = getnb();
        }
        unget(c);
        addr(&e);
        if (e.e_mode == S_IXO || e.e_mode == S_REG)
                aerr();
        abscheck(&e);
        if (e.e_addr > 3)
                aerr();
        return (int) e.e_addr;
}

/*
 * Process a machine op.
 */
VOID
machine(struct mne *mp)
{
        struct expr e;
        a_uint op;
        int t, r, v;

        op = mp->m_valu;
        lisa_raw = 1;

        switch (mp->m_type) {

        case S_INH:
                outaw(op);
                break;

        case S_IMM8:
                t = addr(&e);
                if (t != S_IMM)
                        aerr();
                immbyte(&e, op, -128, 255);
                break;

        case S_SIMM10:
                t = addr(&e);
                if (t != S_IMM)
                        aerr();
                abscheck(&e);
                v = (int) e.e_addr;
                if (v < -512 || v > 511)
                        aerr();
                outaw(op | (v & 0x3FF));
                break;

        case S_IDX:
                t = addr(&e);
                if (t == S_SPO)
                        op |= 0x0200;
                else if (t != S_IXO)
                        aerr();
                abscheck(&e);
                if (e.e_addr > 511)
                        aerr();
                outaw(op | (e.e_addr & 0x1FF));
                break;

        case S_SPW:
                t = addr(&e);
                if (t != S_SPO && t != S_IMM)
                        aerr();
                abscheck(&e);
                if (e.e_addr > 511)
                        aerr();
                outaw(op | (e.e_addr & 0x1FF));
                break;

        case S_DIR:
                /*
                 * [p:abs9]: a 10-bit data address, 0x200..0x3ff is the
                 * peripheral space.
                 */
                t = addr(&e);
                if (t != S_IMM)
                        aerr();
                if (e.e_flag == 0 && e.e_base.e_ap == NULL) {
                        if (e.e_addr > 0x3FF)
                                aerr();
                        outaw(op | e.e_addr);
                } else {
                        e.e_addr = (e.e_addr & 0x3FF) | op;
                        outrw(&e, R_PAGN);
                }
                break;

        case S_BRA:
                /*
                 * rel11, in words, relative to the branch itself.
                 * The target must be in this area.
                 */
                clrexpr(&e);
                expr(&e, 0);
                v = (int) (e.e_addr - dot.s_addr);
                if (pass == 2) {
                        /* (forward references look external in pass 1) */
                        if (e.e_flag || e.e_base.e_ap != dot.s_area)
                                xerr('a', "Branch target must be in the current area.");
                        if ((v & 1) || v < -2048 || v > 2046)
                                aerr();
                }
                outaw(op | ((v >> 1) & 0x7FF));
                break;

        case S_JAL:
                clrexpr(&e);
                expr(&e, 0);
                if (e.e_flag == 0 && e.e_base.e_ap == NULL) {
                        /* an absolute number is a word address */
                        if (e.e_addr > 0x7FFF)
                                aerr();
                        outaw(e.e_addr);
                } else {
                        outrw(&e, R_USGN);
                }
                break;

        case S_LDX:
                t = addr(&e);
                if (t != S_IMM)
                        aerr();
                outaw(op);
                if (e.e_flag == 0 && e.e_base.e_ap == NULL)
                        outaw(e.e_addr & 0xFFFF);
                else
                        outrw(&e, R_PAG0);      /* code symbols: the word address */
                break;

        case S_IF:
                v = lisacond();
                if (v & ~0x27)
                        aerr();
                outaw(op | v);
                break;

        case S_LDAC:
                v = lisacond();
                outaw(op | (v & 7));
                break;

        case S_BIT:
                v = more() ? (int) absexpr() : 0;
                if (v & ~1)
                        aerr();
                outaw(op | v);
                break;

        case S_U2:
                outaw(op | twobits());
                break;

        case S_LDDIV:
                /*
                 * lddiv n(sp): IX[7:0] <- A, RA[7:0] <- n(sp); the
                 * offset is the second word.
                 */
                t = addr(&e);
                if (t != S_SPO && t != S_IMM)
                        aerr();
                abscheck(&e);
                if (e.e_addr > 511)
                        aerr();
                outaw(op);
                outaw(e.e_addr & 0x1FF);
                break;

        case S_DIVREM:
                /*
                 * div dv, n(sp) / rem dv, n(sp): dv[0] = 0 takes the
                 * divisor's high byte from n(sp) (the second word) and
                 * writes the result's high byte back there; dv[0] = 1 is
                 * an 8-bit divisor and a single word.
                 */
                v = twobits();
                outaw(op | v);
                if ((v & 1) == 0) {
                        comma(1);
                        t = addr(&e);
                        if (t != S_SPO && t != S_IMM)
                                aerr();
                        abscheck(&e);
                        if (e.e_addr > 511)
                                aerr();
                        outaw(e.e_addr & 0x1FF);
                }
                break;

        case S_U3:
                v = (int) absexpr();
                if (v & ~7)
                        aerr();
                outaw(op | v);
                break;

        case S_LDZ:
                {
                        char id[NCPS];
                        char *save;
                        int c;

                        c = getnb();
                        v = -1;
                        if (ctype[c] & LETTER) {
                                save = ip;
                                getid(id, c);
                                if (symeq(id, "notz", 1))
                                        v = 2;
                                else if (symeq(id, "c", 1))
                                        v = 3;
                                else
                                        ip = save - 1;
                        } else {
                                unget(c);
                        }
                        if (v < 0)
                                v = (int) absexpr();
                        if (v & ~3)
                                aerr();
                        outaw(op | v);
                }
                break;

        case S_XCHG:
                r = lisareg();
                if (r == R_RA)      outaw(0x8AC0);
                else if (r == R_IA) outaw(0x8AC4);
                else if (r == R_SP) outaw(0x8AC8);
                else                aerr();
                break;

        case S_CPX:
                r = lisareg();
                if (r == R_RA)      outaw(0x8AD0);
                else if (r == R_SP) outaw(0x8AD8);
                else                aerr();
                break;

        case S_PUSH:
                r = lisareg();
                if (r == R_A)       outaw(0xA080);
                else if (r == R_IX) outaw(0xA168);
                else                aerr();
                break;

        case S_POP:
                r = lisareg();
                if (r == R_A)       outaw(0xA0C0);
                else if (r == R_IX) outaw(0xA16C);
                else                aerr();
                break;

        case S_CALL:
        case S_JMP:
                r = lisareg();
                if (r != R_IX)
                        aerr();
                outaw(op);
                break;

        case S_RET:
                if (more()) {
                        /* ret #imm8: broken on TT07 silicon, see PLAN.md */
                        t = addr(&e);
                        if (t != S_IMM)
                                aerr();
                        immbyte(&e, 0x8C00, -128, 255);
                } else {
                        outaw(op);
                }
                break;

        default:
                err('o');
                break;
        }

        lisa_raw = 0;
}

/*
 * Machine specific initialization
 */
VOID
minit(void)
{
        /*
         * Byte Order
         */
        hilo = 0;

        /*
         * Address Space
         */
        exprmasks(3);

        set_sdas_target(TARGET_ID_LISA);
}

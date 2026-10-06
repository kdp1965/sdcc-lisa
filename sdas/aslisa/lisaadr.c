/* lisaadr.c */

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
 */

#include "asxxxx.h"
#include "lisa.h"

/*
 * Register names.  Returns R_* or -1 (and ungets nothing: the caller
 * passes the identifier it already read).
 */
static int
regname(const char *id)
{
        if (symeq(id, "a", 1))  return R_A;
        if (symeq(id, "ix", 1)) return R_IX;
        if (symeq(id, "sp", 1)) return R_SP;
        if (symeq(id, "ra", 1)) return R_RA;
        if (symeq(id, "ia", 1)) return R_IA;
        return -1;
}

/*
 * Read a register name operand (a, ix, sp, ra, ia).
 * Returns R_* or -1 if the next token is not a register name.
 */
int
lisareg(void)
{
        char id[NCPS];
        int c, r;
        char *save;

        c = getnb();
        if ((ctype[c] & LETTER) == 0) {
                unget(c);
                return -1;
        }
        save = ip;
        getid(id, c);
        r = regname(id);
        if (r < 0)
                ip = save - 1;          /* put the whole identifier back */
        return r;
}

/*
 *  Classify an operand:
 *
 *      #expr           S_IMM   (e_addr = value)
 *      expr            S_IMM   (bare value, immediate or direct address)
 *      expr(sp) (sp)   S_SPO   (e_addr = offset)
 *      expr(ix) (ix)   S_IXO
 *      a ix sp ra ia   S_REG   (e_addr = R_*)
 */
int
addr(struct expr *esp)
{
        int c, r;
        char id[NCPS];

        clrexpr(esp);
        c = getnb();

        if (c == '#') {
                expr(esp, 0);
                esp->e_mode = S_IMM;
                return esp->e_mode;
        }

        if (c == '(') {
                /* (sp) / (ix): offset 0 */
                getid(id, -1);
                r = regname(id);
                if (getnb() != ')' || (r != R_SP && r != R_IX))
                        qerr();
                esp->e_mode = (r == R_SP) ? S_SPO : S_IXO;
                esp->e_addr = 0;
                return esp->e_mode;
        }

        if (ctype[c] & LETTER) {
                char *save = ip;
                getid(id, c);
                r = regname(id);
                if (r >= 0) {
                        esp->e_mode = S_REG;
                        esp->e_addr = r;
                        return esp->e_mode;
                }
                ip = save - 1;          /* not a register: re-read as expr */
                c = getnb();
        }

        unget(c);
        expr(esp, 0);
        esp->e_mode = S_IMM;

        c = getnb();
        if (c == '(') {
                getid(id, -1);
                r = regname(id);
                if (getnb() != ')' || (r != R_SP && r != R_IX))
                        qerr();
                esp->e_mode = (r == R_SP) ? S_SPO : S_IXO;
        } else {
                unget(c);
        }
        return esp->e_mode;
}

/*
 * Condition code names for if/iftt/ifte/ldac.  Returns the 6-bit value
 * (s<<5 | cond) or a numeric expression.
 */
int
lisacond(void)
{
        static const struct { const char *n; int v; } tab[] = {
                { "eq", 0 }, { "z", 0 },   { "ne", 1 }, { "nz", 1 },
                { "nc", 2 }, { "c", 3 },
                { "gt", 4 }, { "lt", 5 },  { "ge", 6 }, { "gte", 6 },
                { "le", 7 }, { "lte", 7 },
                { "sgt", 0x24 }, { "slt", 0x25 }, { "sge", 0x26 },
                { "sgte", 0x26 }, { "sle", 0x27 }, { "slte", 0x27 },
                { NULL, 0 }
        };
        char id[NCPS];
        int c, i;
        char *save;

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
        return (int) absexpr();
}

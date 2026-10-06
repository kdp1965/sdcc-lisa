/* lisapst.c */

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
 *   This Assembler Ported by
 *      John L. Hartman (JLH)
 *      jhartman at compuserve dot com
 *      noice at noicedebugger dot com
 *
 */

#include "asxxxx.h"
#include "lisa.h"

/*
 * Mnemonic Structure
 */
struct  mne     mne[] = {

        /* machine */

        /* system */


    {   NULL,   "CON",          S_ATYP,         0,      A_CON   },
    {   NULL,   "OVR",          S_ATYP,         0,      A_OVR   },
    {   NULL,   "REL",          S_ATYP,         0,      A_REL   },
    {   NULL,   "ABS",          S_ATYP,         0,      A_ABS   },
    {   NULL,   "NOPAG",        S_ATYP,         0,      A_NOPAG },
    {   NULL,   "PAG",          S_ATYP,         0,      A_PAG   },

    {   NULL,   "CODE",         S_ATYP,         0,      A_CODE  },
    {   NULL,   "DATA",         S_ATYP,         0,      A_DATA  },
    {   NULL,   "XDATA",        S_ATYP,         0,      A_XDATA },
    {   NULL,   "BIT",          S_ATYP,         0,      A_BIT   },
    {   NULL,   "CDATA",        S_ATYP,         0,      A_CDATA },

    {   NULL,   ".page",        S_PAGE,         0,      0       },
    {   NULL,   ".title",       S_HEADER,       0,      O_TITLE },
    {   NULL,   ".sbttl",       S_HEADER,       0,      O_SBTTL },
    {   NULL,   ".module",      S_MODUL,        0,      0       },
    {	NULL,	".include",	S_INCL,		0,	I_CODE	},
    {	NULL,	".incbin",	S_INCL,		0,	I_BNRY	},
    {   NULL,   ".area",        S_AREA,         0,      0       },
    {   NULL,   ".org",         S_ORG,          0,      0       },
    {   NULL,   ".radix",       S_RADIX,        0,      0       },
    {   NULL,   ".globl",       S_GLOBL,        0,      0       },
    {   NULL,   ".local",       S_LOCAL,        0,      0       },
    {	NULL,	".if",		S_CONDITIONAL,	0,	O_IF	},
    {	NULL,	".iff",		S_CONDITIONAL,	0,	O_IFF	},
    {	NULL,	".ift",		S_CONDITIONAL,	0,	O_IFT	},
    {	NULL,	".iftf",	S_CONDITIONAL,	0,	O_IFTF	},
    {	NULL,	".ifdef",	S_CONDITIONAL,	0,	O_IFDEF	},
    {	NULL,	".ifndef",	S_CONDITIONAL,	0,	O_IFNDEF},
    {	NULL,	".ifgt",	S_CONDITIONAL,	0,	O_IFGT	},
    {	NULL,	".iflt",	S_CONDITIONAL,	0,	O_IFLT	},
    {	NULL,	".ifge",	S_CONDITIONAL,	0,	O_IFGE	},
    {	NULL,	".ifle",	S_CONDITIONAL,	0,	O_IFLE	},
    {	NULL,	".ifeq",	S_CONDITIONAL,	0,	O_IFEQ	},
    {	NULL,	".ifne",	S_CONDITIONAL,	0,	O_IFNE	},
    {	NULL,	".ifb",		S_CONDITIONAL,	0,	O_IFB	},
    {	NULL,	".ifnb",	S_CONDITIONAL,	0,	O_IFNB	},
    {	NULL,	".ifidn",	S_CONDITIONAL,	0,	O_IFIDN	},
    {	NULL,	".ifdif",	S_CONDITIONAL,	0,	O_IFDIF	},
    {	NULL,	".iif",		S_CONDITIONAL,	0,	O_IIF	},
    {	NULL,	".iiff",	S_CONDITIONAL,	0,	O_IIFF	},
    {	NULL,	".iift",	S_CONDITIONAL,	0,	O_IIFT	},
    {	NULL,	".iiftf",	S_CONDITIONAL,	0,	O_IIFTF	},
    {	NULL,	".iifdef",	S_CONDITIONAL,	0,	O_IIFDEF},
    {	NULL,	".iifndef",	S_CONDITIONAL,	0,	O_IIFNDEF},
    {	NULL,	".iifgt",	S_CONDITIONAL,	0,	O_IIFGT	},
    {	NULL,	".iiflt",	S_CONDITIONAL,	0,	O_IIFLT	},
    {	NULL,	".iifge",	S_CONDITIONAL,	0,	O_IIFGE	},
    {	NULL,	".iifle",	S_CONDITIONAL,	0,	O_IIFLE	},
    {	NULL,	".iifeq",	S_CONDITIONAL,	0,	O_IIFEQ	},
    {	NULL,	".iifne",	S_CONDITIONAL,	0,	O_IIFNE	},
    {	NULL,	".iifb",	S_CONDITIONAL,	0,	O_IIFB	},
    {	NULL,	".iifnb",	S_CONDITIONAL,	0,	O_IIFNB	},
    {	NULL,	".iifidn",	S_CONDITIONAL,	0,	O_IIFIDN},
    {	NULL,	".iifdif",	S_CONDITIONAL,	0,	O_IIFDIF},
    {	NULL,	".else",	S_CONDITIONAL,	0,	O_ELSE	},
    {	NULL,	".endif",	S_CONDITIONAL,	0,	O_ENDIF	},
    {   NULL,   ".list",        S_LISTING,      0,      O_LIST  },
    {   NULL,   ".nlist",       S_LISTING,      0,      O_NLIST },
    {   NULL,   ".equ",         S_EQU,          0,      O_EQU   },
    {   NULL,   ".gblequ",      S_EQU,          0,      O_GBLEQU},
    {   NULL,   ".lclequ",      S_EQU,          0,      O_LCLEQU},
/* sdas specific */
    {   NULL,   ".optsdcc",     S_OPTSDCC,      0,      0       },
/* end sdas specific */
    {   NULL,   ".byte",        S_DATA,         0,      O_1BYTE },
    {   NULL,   ".db",          S_DATA,         0,      O_1BYTE },
    {   NULL,   ".fcb",         S_DATA,         0,      O_1BYTE },
    {   NULL,   ".word",        S_DATA,         0,      O_2BYTE },
    {   NULL,   ".dw",          S_DATA,         0,      O_2BYTE },
    {   NULL,   ".fdb",         S_DATA,         0,      O_2BYTE },
/*    { NULL,   ".3byte",       S_DATA,         0,      O_3BYTE },      */
/*    { NULL,   ".triple",      S_DATA,         0,      O_3BYTE },      */
/*    { NULL,   ".4byte",       S_DATA,         0,      O_4BYTE },      */
/*    { NULL,   ".quad",        S_DATA,         0,      O_4BYTE },      */
    {   NULL,   ".blkb",        S_BLK,          0,      O_1BYTE },
    {   NULL,   ".ds",          S_BLK,          0,      O_1BYTE },
    {   NULL,   ".rmb",         S_BLK,          0,      O_1BYTE },
    {   NULL,   ".rs",          S_BLK,          0,      O_1BYTE },
    {   NULL,   ".blkw",        S_BLK,          0,      O_2BYTE },
/*    { NULL,   ".blk3",        S_BLK,          0,      O_3BYTE },      */
/*    { NULL,   ".blk4",        S_BLK,          0,      O_4BYTE },      */
    {   NULL,   ".ascii",       S_ASCIX,        0,      O_ASCII },
    {   NULL,   ".ascis",       S_ASCIX,        0,      O_ASCIS },
    {   NULL,   ".asciz",       S_ASCIX,        0,      O_ASCIZ },
    {   NULL,   ".str",         S_ASCIX,        0,      O_ASCII },
    {   NULL,   ".strs",        S_ASCIX,        0,      O_ASCIS },
    {   NULL,   ".strz",        S_ASCIX,        0,      O_ASCIZ },
    {	NULL,	".fcc",		S_ASCIX,	0,	O_ASCII	},
    {	NULL,	".define",	S_DEFINE,	0,	O_DEF	},
    {	NULL,	".undefine",	S_DEFINE,	0,	O_UNDEF	},
    {	NULL,	".even",	S_BOUNDARY,	0,	O_EVEN	},
    {	NULL,	".odd",		S_BOUNDARY,	0,	O_ODD	},
    {	NULL,	".bndry",	S_BOUNDARY,	0,	O_BNDRY	},
    {	NULL,	".msg"	,	S_MSG,		0,	0	},
    {	NULL,	".assume",	S_ERROR,	0,	O_ASSUME},
    {	NULL,	".error",	S_ERROR,	0,	O_ERROR	},

        /* Macro Processor */

    {   NULL,   ".macro",       S_MACRO,        0,      O_MACRO },
    {   NULL,   ".endm",        S_MACRO,        0,      O_ENDM  },
    {   NULL,   ".mexit",       S_MACRO,        0,      O_MEXIT },

    {   NULL,   ".narg",        S_MACRO,        0,      O_NARG  },
    {   NULL,   ".nchr",        S_MACRO,        0,      O_NCHR  },
    {   NULL,   ".ntyp",        S_MACRO,        0,      O_NTYP  },

    {   NULL,   ".irp",         S_MACRO,        0,      O_IRP   },
    {   NULL,   ".irpc",        S_MACRO,        0,      O_IRPC  },
    {   NULL,   ".rept",        S_MACRO,        0,      O_REPT  },

    {   NULL,   ".nval",        S_MACRO,        0,      O_NVAL  },

    {   NULL,   ".mdelete",     S_MACRO,        0,      O_MDEL  },

        /* machine */

        /* control */
    {   NULL,   "jal",          S_JAL,          0,      0x0000    },
    {   NULL,   "br",           S_BRA,          0,      0xB000    },
    {   NULL,   "bnz",          S_BRA,          0,      0xA800    },
    {   NULL,   "bz",           S_BRA,          0,      0xB800    },
    {   NULL,   "call",         S_CALL,         0,      0x8A80    },
    {   NULL,   "call_ix",      S_INH,          0,      0x8A80    },
    {   NULL,   "jmp",          S_JMP,          0,      0x8AA0    },
    {   NULL,   "jmp_ix",       S_INH,          0,      0x8AA0    },
    {   NULL,   "ret",          S_RET,          0,      0x8A00    },
    {   NULL,   "reti",         S_IMM8,         0,      0x8C00    },
    {   NULL,   "rc",           S_INH,          0,      0x8B00    },
    {   NULL,   "rz",           S_INH,          0,      0x8B80    },
    {   NULL,   "rets",         S_INH,          0,      0x8B40    },
    {   NULL,   "if",           S_IF,           0,      0xA200    },
    {   NULL,   "iftt",         S_IF,           0,      0xA208    },
    {   NULL,   "ifte",         S_IF,           0,      0xA210    },
    {   NULL,   "ldac",         S_LDAC,         0,      0xA1C0    },

        /* immediates */
    {   NULL,   "ldi",          S_IMM8,         0,      0x8000    },
    {   NULL,   "adc",          S_IMM8,         0,      0x9000    },
    {   NULL,   "cpi",          S_IMM8,         0,      0xA400    },
    {   NULL,   "andi",         S_IMM8,         0,      0xD400    },
    {   NULL,   "ads",          S_SIMM10,       0,      0x9400    },
    {   NULL,   "adx",          S_SIMM10,       0,      0x9800    },

        /* alu / memory, n(ix) or n(sp) */
    {   NULL,   "add",          S_IDX,          0,      0xC000    },
    {   NULL,   "sub",          S_IDX,          0,      0xC800    },
    {   NULL,   "cmp",          S_IDX,          0,      0xE800    },
    {   NULL,   "and",          S_IDX,          0,      0xD000    },
    {   NULL,   "or",           S_IDX,          0,      0xD800    },
    {   NULL,   "xor",          S_IDX,          0,      0xE000    },
    {   NULL,   "ldax",         S_IDX,          0,      0xF000    },
    {   NULL,   "stax",         S_IDX,          0,      0xF800    },
    {   NULL,   "swap",         S_IDX,          0,      0xEC00    },
    {   NULL,   "inx",          S_IDX,          0,      0xE400    },
    {   NULL,   "dcx",          S_IDX,          0,      0x9C00    },
    {   NULL,   "mul",          S_IDX,          0,      0xC400    },
    {   NULL,   "mulu",         S_IDX,          0,      0x8400    },
    {   NULL,   "ldxx",         S_SPW,          0,      0xCC00    },
    {   NULL,   "stxx",         S_SPW,          0,      0xCE00    },

        /* direct [p:abs9] */
    {   NULL,   "lda",          S_DIR,          0,      0xF400    },
    {   NULL,   "sta",          S_DIR,          0,      0xFC00    },
    {   NULL,   "swapi",        S_DIR,          0,      0xDC00    },

        /* ix / sp / ra */
    {   NULL,   "ldx",          S_LDX,          0,      0xA180    },
    {   NULL,   "tax",          S_INH,          0,      0xA100    },
    {   NULL,   "taxu",         S_INH,          0,      0xA108    },
    {   NULL,   "txa",          S_INH,          0,      0xA010    },
    {   NULL,   "txau",         S_INH,          0,      0xA018    },
    {   NULL,   "addax",        S_INH,          0,      0xA1A0    },
    {   NULL,   "addaxc",       S_INH,          0,      0xA1A4    },
    {   NULL,   "addaxu",       S_INH,          0,      0xA1A1    },
    {   NULL,   "subax",        S_INH,          0,      0xA1A2    },
    {   NULL,   "subaxc",       S_INH,          0,      0xA1A6    },
    {   NULL,   "subaxu",       S_INH,          0,      0xA1A3    },
    {   NULL,   "xchg",         S_XCHG,         0,      0x8AC0    },
    {   NULL,   "xchg_ra",      S_INH,          0,      0x8AC0    },
    {   NULL,   "xchg_ia",      S_INH,          0,      0x8AC4    },
    {   NULL,   "xchg_sp",      S_INH,          0,      0x8AC8    },
    {   NULL,   "spix",         S_INH,          0,      0x8ACC    },
    {   NULL,   "cpx",          S_CPX,          0,      0x8AD0    },
    {   NULL,   "cpx_ra",       S_INH,          0,      0x8AD0    },
    {   NULL,   "cpx_sp",       S_INH,          0,      0x8AD8    },
    {   NULL,   "push",         S_PUSH,         0,      0xA080    },
    {   NULL,   "pop",          S_POP,          0,      0xA0C0    },
    {   NULL,   "push_a",       S_INH,          0,      0xA080    },
    {   NULL,   "pop_a",        S_INH,          0,      0xA0C0    },
    {   NULL,   "push_ix",      S_INH,          0,      0xA168    },
    {   NULL,   "pop_ix",       S_INH,          0,      0xA16C    },
    {   NULL,   "sra",          S_INH,          0,      0xA160    },
    {   NULL,   "lra",          S_INH,          0,      0xA164    },

        /* shifts, flags, misc */
    {   NULL,   "shl",          S_INH,          0,      0xA000    },
    {   NULL,   "shr",          S_INH,          0,      0xA004    },
    {   NULL,   "shl16",        S_U2,           0,      0xA020    },
    {   NULL,   "shr16",        S_U2,           0,      0xA030    },
    {   NULL,   "ldc",          S_BIT,          0,      0xA008    },
    {   NULL,   "ldz",          S_LDZ,          0,      0xA060    },
    {   NULL,   "notz",         S_INH,          0,      0xA074    },
    {   NULL,   "btst",         S_U3,           0,      0xA040    },
    {   NULL,   "amode",        S_U3,           0,      0xA140    },
    {   NULL,   "savec",        S_INH,          0,      0xA178    },
    {   NULL,   "restc",        S_INH,          0,      0xA17C    },
    {   NULL,   "ldirq",        S_INH,          0,      0xA120    },
    {   NULL,   "stirq",        S_INH,          0,      0xA124    },
    {   NULL,   "eidi",         S_BIT,          0,      0xA078    },
    {   NULL,   "di",           S_INH,          0,      0xA078    },
    {   NULL,   "ei",           S_INH,          0,      0xA079    },
    {   NULL,   "brk",          S_INH,          0,      0xA07C    },
    {   NULL,   "nop",          S_INH,          0,      0xA070    },

        /* divider */
    {   NULL,   "lddiv",        S_INH,          0,      0xA170    },
    {   NULL,   "div",          S_U2,           0,      0xA300    },
    {   NULL,   "rem",          S_U2,           0,      0xA310    },

        /* bf16 */
    {   NULL,   "tfa",          S_BIT,          0,      0xA1E0    },
    {   NULL,   "tfau",         S_INH,          0,      0xA1E1    },
    {   NULL,   "taf",          S_BIT,          0,      0xA1E2    },
    {   NULL,   "tafu",         S_INH,          0,      0xA1E3    },
    {   NULL,   "fmul",         S_U2,           0,      0xA1E4    },
    {   NULL,   "fadd",         S_U2,           0,      0xA1E8    },
    {   NULL,   "fneg",         S_U2,           0,      0xA1EC    },
    {   NULL,   "fswap",        S_U2,           0,      0xA1F0    },
    {   NULL,   "fcmp",         S_U2,           0,      0xA1F4    },
    {   NULL,   "fdiv",         S_U2,           0,      0xA1F8    },
    {   NULL,   "itof",         S_INH,          0,      0xA320    },
    {   NULL,   "ftoi",         S_INH,          0,      0xA321    },
    {   NULL,   "fclr",         S_INH,          S_EOL,  0xA322    },
};

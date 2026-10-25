/*-------------------------------------------------------------------------
   tt07.h - the peripherals of the LISA core on the TT07 Tiny Tapeout chip
            (tt_um_lisa), for sdcc -mlisa

   Register map from lisa_periph.v.  The peripheral space is the data
   addresses 0x200..0x3ff (the `p` bit of the direct lda/sta forms);
   every register is one byte.  `__sfr __at(0x2xx)` declarations compile
   to the direct forms, so `PORTB = 0x4f;` is a single sta.

   Copyright (C) 2026

   This library is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this library; see the file COPYING. If not, write to the
   Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.

   As a special exception, if you link this library with other files,
   some of which are compiled with SDCC, to produce an executable,
   this library does not by itself cause the resulting executable to
   be covered by the GNU General Public License. This exception does
   not however invalidate any other reasons why the executable file
   might be covered by the GNU General Public License.
-------------------------------------------------------------------------*/

#ifndef __TT07_H
#define __TT07_H 1

/* ---- GPIO -------------------------------------------------------------- */
__sfr __at(0x200) PORTA;            /* input */
__sfr __at(0x201) PORTB;            /* output (LEDs on the demo board) */
__sfr __at(0x202) PORTC;            /* bidirectional, low 4 bits */
__sfr __at(0x203) PORTC_DIR;        /* 1 = output, low 4 bits */
__sfr __at(0x204) PORTD;            /* input */
__sfr __at(0x205) PORTE;            /* output */
__sfr __at(0x206) PORTA_INT_CFG0;   /* PORTA edge interrupt: bit set = rising edge enabled */
__sfr __at(0x207) PORTA_INT_CFG1;   /*                       bit set = falling edge enabled */

/* ---- Timers ------------------------------------------------------------
   Each timer counts system clocks: every PREDIV+1 clocks it steps DIV
   (the "ms" count, reloaded from DIV), and when that reaches 1 the
   rollover flag and the interrupt are raised and COUNT (the tick counter)
   increments.  At 50 MHz, PREDIV = 49999 gives a 1 ms step, so the
   interrupt period is DIV ms.  Reading CTRL clears the rollover flag;
   the interrupt is cleared through INT_STATUS. */
__sfr __at(0x208) TIMER1_PREDIV_LO;
__sfr __at(0x209) TIMER1_PREDIV_HI;
__sfr __at(0x20a) TIMER1_DIV_LO;
__sfr __at(0x20b) TIMER1_DIV_HI;
__sfr __at(0x20c) TIMER1_CTRL;
__sfr __at(0x20d) TIMER1_COUNT;     /* tick counter; a write also restarts the timing */

__sfr __at(0x218) TIMER2_PREDIV_LO;
__sfr __at(0x219) TIMER2_PREDIV_HI;
__sfr __at(0x21a) TIMER2_DIV_LO;
__sfr __at(0x21b) TIMER2_DIV_HI;
__sfr __at(0x21c) TIMER2_CTRL;
__sfr __at(0x21d) TIMER2_COUNT;

#define TIMER_CTRL_ENABLE    0x01
#define TIMER_CTRL_ROLLOVER  0x80   /* read-only; cleared by reading CTRL */

/* ---- Interrupts --------------------------------------------------------
   INT_ENABLE masks the eight sources; INT_STATUS shows the pending ones
   and a write with a bit set clears that one.  The core vectors a pending
   enabled source n to code address n + 1 (lowest bit first); that is the
   number to give __interrupt: `void timer1_isr (void) __interrupt (LISA_INT_TIMER1)`.
   Vector 9 catches a request that is not one-hot.  Interrupts are enabled
   globally with lisa_ei () and disabled with lisa_di (); the handler runs
   with them disabled and rets re-enables them. */
__sfr __at(0x20e) INT_ENABLE;
__sfr __at(0x20f) INT_STATUS;

#define INT_TIMER1   0x01
#define INT_TIMER2   0x02
#define INT_UART1    0x04
#define INT_UART2    0x08
#define INT_PORTA    0x10
#define INT_TTLC     0x20
#define INT_I2C      0x40

#define LISA_INT_TIMER1   1
#define LISA_INT_TIMER2   2
#define LISA_INT_UART1    3
#define LISA_INT_UART2    4
#define LISA_INT_PORTA    5
#define LISA_INT_TTLC     6
#define LISA_INT_I2C      7
#define LISA_INT_SPURIOUS 9
#define LISA_INT_TIMER3   9    /* the third chip's Timer 3 (ttsky26d RESPIN 32): source 8, vector 9 */

/* ---- Timer 3 (the third chip, RESPIN 32) -------------------------------
   A 16-bit counter 0..TOP with a prescaler (a tick every PRE + 1 clocks),
   two compares driving PWM0 / PWM1 (uo[1] / uo[2] in mux 2, while
   T3_CTRL enables them) and two input captures (ui_in[4] / ui_in[5]).
   Reading T3_CNT_L latches T3_CNT_H; a T3_CNT_L write loads the count with
   the T3_CNT_H written before.  Any event enabled in T3_IE is interrupt
   source 8, vector 9: __interrupt (LISA_INT_TIMER3). */
__sfr __at(0x230) T3_PRE;
__sfr __at(0x231) T3_TOP_L;
__sfr __at(0x232) T3_TOP_H;
__sfr __at(0x233) T3_CNT_L;
__sfr __at(0x234) T3_CNT_H;
__sfr __at(0x235) T3_CMP0_L;
__sfr __at(0x236) T3_CMP0_H;
__sfr __at(0x237) T3_CMP1_L;
__sfr __at(0x238) T3_CMP1_H;
__sfr __at(0x239) T3_CAP0_L;
__sfr __at(0x23A) T3_CAP0_H;
__sfr __at(0x23B) T3_CAP1_L;
__sfr __at(0x23C) T3_CAP1_H;
__sfr __at(0x23D) T3_CTRL;
__sfr __at(0x23E) T3_IE;
__sfr __at(0x23F) T3_STATUS;

#define T3_CTRL_EN        0x01
#define T3_CTRL_PWM0      0x02
#define T3_CTRL_PWM1      0x04
#define T3_CTRL_CAP0_RISE 0x10
#define T3_CTRL_CAP0_FALL 0x20
#define T3_CTRL_CAP1_RISE 0x40
#define T3_CTRL_CAP1_FALL 0x80
#define T3_OVF            0x01   /* T3_IE / T3_STATUS bits */
#define T3_CMP0           0x02
#define T3_CMP1           0x04
#define T3_CAP0           0x08
#define T3_CAP1           0x10

/* ---- WS2812B driver (the third chip, RESPIN 33) ------------------------
   The pixels are the TTLC's 48 output flops, written through the TTLC I/O
   window (TIDX 0x22C / TDATA 0x22D): half A bytes 0-2, half B bytes 3-5,
   each G, R, B.  Set WS_EN and a pin (uo[0], [1], [2], [5], [6] in mux 3),
   load a half, write its FULL bit to WS_STAT, and keep filling A, B, A ...
   from DONE on; WS_IE makes a free half the TTLC's interrupt source
   (__interrupt (LISA_INT_TTLC)).  Keep WS_EN set while strings are
   attached: off, the pins are the TTLC's again.  The MC14500B must be
   halted (its reset state). */
__sfr __at(0x240) WS_CTRL;
__sfr __at(0x241) WS_STAT;
__sfr __at(0x242) WS_T0H;
__sfr __at(0x243) WS_T1H;
__sfr __at(0x244) WS_TBIT;
__sfr __at(0x245) WS_TRST;

#define WS_EN             0x01
#define WS_PIN(n)         ((n) << 1)   /* 0 uo[0], 1 uo[1], 2 uo[2], 3 uo[5], 4 uo[6] */
#define WS_IE             0x10
#define WS_FULL_A         0x01   /* WS_STAT */
#define WS_FULL_B         0x02
#define WS_BUSY           0x04
#define WS_DONE           0x08
#define WS_UNDERRUN       0x10
#define WS_NEXT           0x20

/* DMA (RESPIN 35): frames straight from the PSRAM.  Put the buffer (G, R, B
   a pixel) on a 256-byte boundary, BASE = its address / 256 into the TTLC
   output bytes 0-1 and LEN (pixels, up to 8191) into bytes 2-3 (TIDX = 0,
   four TDATA writes), set WS_EN | WS_DMA and a pin, write WS_START: the
   data cache is flushed, the frame sent, WS_FRAME set at its end (and the
   request with WS_IE); WS_ACTIVE until then, or on with WS_REPEAT. */
#define WS_DMA            0x20   /* WS_CTRL */
#define WS_REPEAT         0x40
#define WS_ACTIVE         0x40   /* WS_STAT: a frame running */
#define WS_START          0x40   /* WS_STAT written: a frame */
#define WS_FRAME          0x80   /* WS_STAT: a frame ended, write 1 to clear */

/* ---- Cycle counter and watchdog (the third chip, RESPIN 36) ------------
   CYC0..CYC3: a 32-bit count of clocks; reading CYC0 latches CYC1..CYC3, a
   write to CYC0 clears it.  The watchdog: set WDT_TIMEOUT and WDT_CTRL =
   WDT_EN | WDT_SCALE(n), then write WDT_KICK_KEY to WDT_KICK within
   (TIMEOUT + 1) x 2^(16 + n) clocks, or it resets the core and the
   peripherals (the program restarts at 0; WDT_EN is then clear and
   WDT_WDRST set - a 1 written clears it).  The PDM outputs are Timer 3's
   channels in PDM mode: T3_IE_PDM0 / T3_IE_PDM1 in T3_IE make the channel
   a sigma-delta output of CMPn's low byte / 256 (on its PWM pin, with
   T3_CTRL's PWM out bit) instead of an input capture; an RC filter makes
   it an 8-bit DAC. */
__sfr __at(0x248) CYC0;
__sfr __at(0x249) CYC1;
__sfr __at(0x24A) CYC2;
__sfr __at(0x24B) CYC3;
__sfr __at(0x24C) WDT_CTRL;
__sfr __at(0x24D) WDT_KICK;
__sfr __at(0x24E) WDT_TIMEOUT;
__sfr __at(0x24F) WDT_STAT;

#define WDT_EN            0x01   /* WDT_CTRL */
#define WDT_SCALE(n)      ((n) << 1)
#define WDT_KICK_KEY      0xA5
#define WDT_WDRST         0x01   /* WDT_STAT */
#define T3_IE_PDM0        0x20   /* T3_IE: channel 0 a PDM output (no CAP0) */
#define T3_IE_PDM1        0x40   /* T3_IE: channel 1 a PDM output (no CAP1) */

#define lisa_ei()  __asm__ ("eidi\t1")
#define lisa_di()  __asm__ ("eidi\t0")

/* ---- UARTs ---------------------------------------------------------------
   UART1 is the console (the demo board forwards it through the debug UART).
   DATA reads the received byte and writes the byte to send. */
__sfr __at(0x210) UART1_DATA;
__sfr __at(0x211) UART1_STATUS;
__sfr __at(0x212) UART2_DATA;
__sfr __at(0x213) UART2_STATUS;
__sfr __at(0x214) UART_INT_EN;

#define UART_RX_AVAIL        0x01   /* a received byte is waiting */
#define UART_TX_EMPTY        0x02   /* the transmit buffer is free */
#define UART2_AUTOBAUD_OFF   0x04   /* UART2 only */

#define UART_INT_RX1         0x01   /* UART_INT_EN bits */
#define UART_INT_TX1         0x02
#define UART_INT_RX2         0x04
#define UART_INT_TX2         0x08

/* ---- I2C master (when built in) ---------------------------------------- */
__sfr __at(0x220) I2C_PRESCALE_LO;
__sfr __at(0x221) I2C_PRESCALE_HI;
__sfr __at(0x222) I2C_CTRL;
__sfr __at(0x223) I2C_RX_DATA;
__sfr __at(0x224) I2C_STATUS;
__sfr __at(0x225) I2C_TX_DATA;
__sfr __at(0x226) I2C_CMD;

/* ---- Memory --------------------------------------------------------------
   Without the data cache the chip has 128 bytes of RAM at 0..0x7f; the
   default stack starts at 0x7f (sdcc --stack-loc).  With the cache and
   the PSRAM Pmod the data space is 32K (--stack-loc 0x7fff). */
#define LISA_TT07_RAM_BYTES  128

#endif

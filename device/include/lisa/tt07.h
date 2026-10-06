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

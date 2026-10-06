/* Port-specific support for the regression tests on lisa_sim.
 *
 * Output goes to UART1 (peripheral 0x10/0x11, data addresses 0x210/0x211),
 * which lisa_sim -b echoes to stdout.  _exitEmu() executes `brk`, which
 * halts the simulator so batch mode returns at once.
 */

__sfr __at(0x210) UART1_TX;
__sfr __at(0x211) UART1_STATUS;

void
_putchar(unsigned char c)
{
  while (!(UART1_STATUS & 2))   /* bit 1: tx buffer empty */
    ;
  UART1_TX = c;
}

void
_initEmu(void)
{
}

void
_exitEmu(void)
{
  __asm
    brk
  __endasm;
}

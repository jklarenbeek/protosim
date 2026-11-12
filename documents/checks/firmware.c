/* Fixture for the checks: a .data variable, a flash constant, a timer interrupt,
 * and a line on UART0. Built with -Os -g so the debugger has source lines. */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>

static const char banner[] PROGMEM = "hello from flash";
volatile uint16_t counter = 0x1234;          /* .data: initialised RAM */
volatile uint8_t  scratch[40];               /* .bss  */

ISR(TIMER0_OVF_vect) { counter++; }

static void put(char c) { while (!(UCSR0A & _BV(UDRE0))) { } UDR0 = c; }

int main(void)
{
  scratch[0] = pgm_read_byte(&banner[0]);
  UCSR0A = _BV(U2X0); UBRR0L = 16; UCSR0B = _BV(TXEN0);
  for (const char *p = "fixture up | end\n"; *p; p++) put(*p);
  DDRB |= 1;
  for (;;) { PORTB ^= 1; counter += scratch[0]; }
}

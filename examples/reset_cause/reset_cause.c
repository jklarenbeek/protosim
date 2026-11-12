/* reset_cause.c - says at every start why the part was reset.
 *
 * An AVR records the cause of a reset in MCUSR. This program prints the
 * register at every start and then clears it, as firmware should.
 *
 *   k   keep: from now on only the watchdog flag is cleared, so the other
 *       flags add up from reset to reset as they do on a chip. (While WDRF
 *       is set the watchdog cannot be switched off.)
 *   w   arm the watchdog at 15 ms and hang: the watchdog resets the part
 *   .   ignored
 *   any other character is echoed in brackets
 *
 * The start counter and the "keep" flag live in .noinit: they survive every
 * reset but a power-on.
 *
 *   protosim reset_cause.elf --uart0-out out.txt --max-steps 2000000 \
 *            --reset-at 20 --reset-at 40:brownout --reset-at 60:power
 */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#include <avr/io.h>
#include <avr/wdt.h>

static uint8_t starts __attribute__((section(".noinit")));
static uint8_t keep __attribute__((section(".noinit")));

static void uart_tx(char c) {
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = c;
}

static void uart_puts(const char *s) {
    while (*s) uart_tx(*s++);
}

static void uart_hex(uint8_t v) {
    const char *digits = "0123456789abcdef";
    uart_tx(digits[v >> 4]);
    uart_tx(digits[v & 15]);
}

int main(void) {
    uint8_t cause = MCUSR;

    MCUSR = keep ? cause & ~(1 << WDRF) : 0;
    wdt_disable();                      /* after a watchdog reset the watchdog is still running */

    UCSR0A = (1 << U2X0);               /* 115200 baud at 16 MHz */
    UBRR0H = 0;
    UBRR0L = 16;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);

    starts++;
    uart_puts("start ");
    uart_hex(starts);
    uart_puts(": MCUSR=");
    uart_hex(cause);
    if (cause & (1 << PORF)) uart_puts(" power-on");
    if (cause & (1 << EXTRF)) uart_puts(" pin");
    if (cause & (1 << BORF)) uart_puts(" brown-out");
    if (cause & (1 << WDRF)) uart_puts(" watchdog");
    uart_tx('\n');

    for (;;) {
        if (!(UCSR0A & (1 << RXC0))) continue;
        char c = UDR0;
        if (c == 'k') {
            keep = 1;
        } else if (c == 'w') {
            while (!(UCSR0A & (1 << TXC0)));
            wdt_enable(WDTO_15MS);
            for (;;) { }
        } else if (c != '.') {
            uart_tx('[');
            uart_tx(c);
            uart_tx(']');
        }
    }
}

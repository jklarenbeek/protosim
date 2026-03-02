/*
 * demo.c — protosim capability demonstration firmware
 *
 * ATmega328P, 16 MHz, no Arduino framework.
 * Self-running: initialises UART, sends a banner over UART0, then
 * calls compute() in a loop.  Designed so that --coverage, --profile,
 * --callgraph and named breakpoints all produce meaningful output.
 *
 * Compile:
 *   avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o demo.elf demo.c
 *   avr-objcopy -O ihex -R .eeprom demo.elf demo.hex
 */

#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include <avr/io.h>
#include <stdint.h>

/* ── UART ─────────────────────────────────────────────────────── */

#define BAUD 9600UL
#define UBRR_VAL ((F_CPU / 16 / BAUD) - 1)

void uart_init(void) {
  UBRR0H = (uint8_t)(UBRR_VAL >> 8);
  UBRR0L = (uint8_t)(UBRR_VAL);
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); /* 8N1 */
}

void uart_tx(uint8_t c) {
  while (!(UCSR0A & (1 << UDRE0)))
    ;
  UDR0 = c;
}

void uart_send(const char *s) {
  while (*s)
    uart_tx((uint8_t)*s++);
}

/* ── Computation ──────────────────────────────────────────────── */

/*
 * compute(n): iterative integer multiply-accumulate.
 * Returns the sum of 1*1 + 2*2 + ... + n*n mod 256.
 * Keeps the CPU busy in a named function — ideal for profiling.
 */
uint8_t compute(uint8_t n) {
  uint16_t acc = 0;
  for (uint8_t i = 1; i <= n; i++)
    acc += (uint16_t)i * i;
  return (uint8_t)(acc & 0xFF);
}

/* ── Dead code (to demo coverage "NEVER REACHED") ───────────── */

void unused_handler(void) { uart_send("ERR\r\n"); }

/* ── main ─────────────────────────────────────────────────────── */

int main(void) {
  uart_init();
  uart_send("protosim-demo ready\r\n");

  uint8_t iter = 0;
  while (1) {
    uint8_t result = compute(20);
    uart_tx('0' + (result & 0x07)); /* send one digit */
    iter++;
    if (iter == 0) /* wraps at 256 — very infrequent */
      uart_send("wrap\r\n");
  }
}

/* uart_echo.c - an RP2040 or RP2350 that answers on UART0 (GP0, GP1)
 *
 * Says hello, then sends back each line it receives in upper case.
 *   protosim uart_echo.elf --uart0-in "hello\r" --uart0-out out.txt --run-ms 50
 */
#include <ctype.h>
#include <stdio.h>
#include "pico/stdlib.h"

int main(void) {
  stdio_init_all();
  printf("uart_echo: ready\n");
  char line[64];
  int n = 0;
  for (;;) {
    int c = getchar_timeout_us(1000000);
    if (c == PICO_ERROR_TIMEOUT) continue;
    if (c == '\r' || c == '\n') {
      line[n] = 0;
      if (n) printf("> %s\n", line);
      n = 0;
    } else if (n < (int)sizeof(line) - 1) {
      line[n++] = (char)toupper(c);
    }
  }
}

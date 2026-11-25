/* blink_sleep.c - the LED on GP25 blinks, and the cores sleep in between
 *
 * sleep_ms() of the Pico SDK waits with WFE until a timer alarm: the core
 * sleeps. 100 ms on, 400 ms off; the count of blinks on UART0.
 *   protosim blink_sleep.elf --pin-log GP25 --pin-duty GP25:1000 --warp --run-ms 3000
 */
#include <stdio.h>
#include "pico/stdlib.h"

#define LED 25

int main(void) {
  stdio_init_all();
  gpio_init(LED);
  gpio_set_dir(LED, GPIO_OUT);
  printf("blink_sleep: ready\n");
  for (unsigned n = 1;; n++) {
    gpio_put(LED, 1);
    sleep_ms(100);
    gpio_put(LED, 0);
    sleep_ms(400);
    printf("blink %u\n", n);
  }
}

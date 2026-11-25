/* watchdog.c - the watchdog resets a program that stops feeding it
 *
 * Feeds the watchdog (100 ms) three times, then stops. After the reset the
 * program knows it was the watchdog, and counts the resets in a scratch
 * register, which a watchdog reset keeps. After three it stays.
 *   protosim watchdog.elf --uart0-out out.txt --run-ms 1000
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"

int main(void) {
  stdio_init_all();
  uint32_t resets = 0;
  if (watchdog_caused_reboot()) {
    resets = watchdog_hw->scratch[0] + 1;
    printf("watchdog: reset by the watchdog, %lu so far\n", (unsigned long)resets);
  } else {
    printf("watchdog: power on\n");
  }
  watchdog_hw->scratch[0] = resets;
  if (resets >= 3) {
    printf("watchdog: that is enough\n");
    for (;;) __wfi();
  }
  watchdog_enable(100, true);
  for (int i = 1; i <= 3; i++) {
    sleep_ms(50);
    watchdog_update();
    printf("fed %d\n", i);
  }
  printf("watchdog: stops feeding\n");
  for (;;) tight_loop_contents();
}

/* multicore.c - both cores at work, talking through the SIO FIFOs
 *
 * Core 0 starts core 1, sends it numbers and prints what comes back: the
 * square, computed on core 1. Core 1 also toggles GP15 for each number, so
 * that its work is seen on a pin as well.
 *   protosim multicore.elf --pin-log GP15 --uart0-out out.txt --run-ms 50
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#define PIN 15

static void core1_main(void) {
  gpio_init(PIN);
  gpio_set_dir(PIN, GPIO_OUT);
  multicore_fifo_push_blocking(0xC0DE0001u); /* core 1 is up */
  for (;;) {
    uint32_t n = multicore_fifo_pop_blocking();
    gpio_xor_mask(1u << PIN);
    multicore_fifo_push_blocking((sio_hw->cpuid << 24) | (n * n));
  }
}

int main(void) {
  stdio_init_all();
  printf("multicore: core %lu starts core 1\n", (unsigned long)sio_hw->cpuid);
  multicore_launch_core1(core1_main);
  uint32_t hello = multicore_fifo_pop_blocking();
  printf("core 1 is up: 0x%08lx\n", (unsigned long)hello);
  for (uint32_t n = 1; n <= 5; n++) {
    multicore_fifo_push_blocking(n);
    uint32_t r = multicore_fifo_pop_blocking();
    printf("%lu squared on core %lu: %lu\n", (unsigned long)n, (unsigned long)(r >> 24), (unsigned long)(r & 0xFFFFFF));
  }
  printf("multicore: done\n");
  for (;;) __wfi();
}

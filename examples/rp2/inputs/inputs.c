/* inputs.c - what the world puts on the pins: edges on GP2, voltages at
 * ADC0 (GP26) and at the temperature sensor
 *
 * Rising edges on GP2 are counted in an interrupt handler; every 100 ms the
 * count and both voltages go out on UART0, in mV.
 *   protosim inputs.elf --pin-wave GP2:10 --adc 0:1650 --adc 0:3000@250 --adc temp:706 \
 *            --uart0-out out.txt --run-ms 450
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

#define EDGES 2

static volatile uint32_t edges;

static void on_edge(uint gpio, uint32_t events) {
  if (gpio == EDGES && (events & GPIO_IRQ_EDGE_RISE)) edges++;
}

static uint32_t millivolts(uint input) {
  adc_select_input(input);
  return (uint32_t)adc_read() * 3300u / 4096u;
}

int main(void) {
  stdio_init_all();
  gpio_init(EDGES);
  gpio_set_dir(EDGES, GPIO_IN);
  gpio_set_irq_enabled_with_callback(EDGES, GPIO_IRQ_EDGE_RISE, true, on_edge);
  adc_init();
  adc_gpio_init(26);
  adc_set_temp_sensor_enabled(true);
  printf("inputs: ready\n");
  for (int t = 100;; t += 100) {
    sleep_ms(100);
    printf("%d ms: %lu edges, adc0 %lu mV, temp %lu mV\n", t, (unsigned long)edges, (unsigned long)millivolts(0),
           (unsigned long)millivolts(4));
  }
}

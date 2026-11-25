#ifndef STIMULUS_H
#define STIMULUS_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════
   Pins and analog inputs

   A part without a USART talks on port pins, and what small firmware
   does is mostly seen on pins: a reset line it pulls, a PWM it makes.
   This module is the outside world for such firmware, on any backend:

     a serial line on two pins   bridged to the PTY, like UART0
     levels and square waves     into input pins
     voltages                    at ADC inputs, and the supply itself
     a log of changes            of output pins
     the duty cycle              of a pin, per window of time

   Times are simulated time since the start of the simulation, in ms.
   A pin is named the way the backend names it: B3 on an AVR, GP25 on
   an RP2040.
   ═══════════════════════════════════════════════════════════════ */

#define STIMULUS_MAX 32

/* Each returns 0, or -1 on a malformed argument. The backend has to be
   chosen first: it reads the pin names. */
int stimulus_soft_uart_arg(const char *arg); /* <rx>,<tx>[,<baud>[,<gap bits>]] */
int stimulus_pin_wave_arg(const char *arg);  /* <pin>:<period ms>[:<from ms>[:<to ms>]] */
int stimulus_pin_set_arg(const char *arg);   /* <pin>:<0|1>[@<ms>] */
int stimulus_pin_pull_arg(const char *arg);  /* <pin>:<0|1> */
int stimulus_pin_log_arg(const char *arg);   /* <pin> */
int stimulus_pin_duty_arg(const char *arg);  /* <pin>[:<window ms>] */
int stimulus_adc_arg(const char *arg);       /* <channel|temp>:<mV>[@<ms>] */
int stimulus_vcc_arg(const char *arg);       /* <mV>[@<ms>] */

/* True when --soft-uart was given: the PTY carries that line, not UART0. */
int stimulus_has_soft_uart(void);

/* Call once, after the firmware is loaded and before the first step. */
void stimulus_setup(void);

/* The soft line and whatever carries it on the host side (the PTY): what
   the part sends, and when the line is free for the next byte to it. */
typedef struct {
  void (*from_part)(uint8_t byte);
  void (*xon)(void);
  void (*xoff)(void);
} stimulus_bridge_t;

void stimulus_soft_uart_bridge(const stimulus_bridge_t *bridge);
/* a byte for the part, on its RX pin */
void stimulus_soft_uart_put(uint8_t byte);

#endif /* STIMULUS_H */

#ifndef STIMULUS_H
#define STIMULUS_H

#include <stdint.h>
#include "sim_avr.h"
#include "sim_irq.h"

/* ═══════════════════════════════════════════════════════════════
   Pins and analog inputs

   A part without a USART talks on port pins, and what small firmware
   does is mostly seen on pins: a reset line it pulls, a PWM it makes.
   This module is the outside world for such firmware:

     a serial line on two pins   bridged to the PTY, like UART0
     levels and square waves     into input pins
     voltages                    at ADC inputs, and the supply itself
     a log of changes            of output pins
     the duty cycle              of a pin, per window of time

   Times are simulated time since the start of the simulation, in ms.
   A pin is a port letter and a bit: B3.
   ═══════════════════════════════════════════════════════════════ */

#define STIMULUS_MAX 32

/* Each returns 0, or -1 on a malformed argument. */
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

/* Call once, after the firmware is loaded and before the first step.
   byte_in receives what the part sends on its soft line; byte_out delivers
   what it is to receive; xon and xoff tell the sender when the line is
   free. They are the IRQs of the PTY bridge, or NULL without one. */
void stimulus_setup(avr_t *avr);
void stimulus_soft_uart_irqs(avr_irq_t **from_part, avr_irq_t **to_part, avr_irq_t **xon, avr_irq_t **xoff);

/* A sleeping part costs no real time. */
void stimulus_warp(avr_t *avr);

#endif /* STIMULUS_H */

#ifndef RESET_H
#define RESET_H

#include <stdint.h>
#include "sim_avr.h"

/* ═══════════════════════════════════════════════════════════════
   Reset causes

   An AVR records why it was reset in MCUSR. Firmware and bootloaders
   branch on it: a bootloader may wait for an upload only after a reset
   through the pin, an application may want to know that the watchdog
   stopped it. simavr records a power-on at start and a watchdog reset;
   this module adds the rest and keeps the flags as a chip keeps them:

     power      PORF is set and the other three flags are cleared; RAM is
                cleared, as it is when a simulation starts
     pin        EXTRF is added to the flags that were there
     brownout   BORF is added
     watchdog   WDRF is added

   The values are the positions of the flags in MCUSR on the classic AVRs.
   ═══════════════════════════════════════════════════════════════ */

#define RESET_POWER_ON  0x01
#define RESET_PIN       0x02
#define RESET_BROWN_OUT 0x04
#define RESET_WATCHDOG  0x08

#define MAX_RESETS 32
#define MAX_RESET_HOOKS 8

/* "power", "pin", "brownout", "watchdog" → cause; -1 when the name is unknown. */
int reset_cause_from_name(const char *name);
const char *reset_cause_name(int cause);

/* --reset-at <ms>[:cause]  schedule a reset at that simulated time, counted
   from the start of the simulation across earlier resets. The cause is the
   reset pin when none is given. Returns -1 on a malformed argument. */
int add_reset_arg(const char *arg);

/* Call once, after the firmware is loaded and before the first step.
   first_cause is the cause the first start reports. */
void reset_setup(avr_t *a, int first_cause);

/* Reset the part now. */
void reset_now(avr_t *a, int cause);

/* Call once per step of the main loop, after avr_run(). Carries out resets
   that are due, by schedule or by signal, and notices a reset that simavr
   made itself (the watchdog, or a debugger's "monitor reset"), so that the
   flags and the hooks are right for those too. */
void reset_after_step(avr_t *a);

/* simavr drops every pending cycle timer at a reset. A hook is called after
   each reset so that its owner can register its timers again. */
void reset_add_hook(void (*hook)(avr_t *a));

/* Cycles since the simulation began; avr->cycle restarts at every reset. */
uint64_t reset_total_cycles(avr_t *a);

/* The four flags as they stand in MCUSR, in the RESET_* positions. */
uint8_t reset_flags(avr_t *a);

/* POSIX: SIGUSR1 pulls the reset pin, SIGUSR2 cycles the power.
   Does nothing where those signals do not exist. */
void reset_install_signals(void);

#endif /* RESET_H */

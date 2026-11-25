#ifndef PROTOSIM_SIM_H
#define PROTOSIM_SIM_H

/* ═══════════════════════════════════════════════════════════════
   The backend layer

   protosim runs firmware on an emulator it links in: simavr for AVR
   parts, Bramble for the RP2040 and RP2350. The driver (protosim.c)
   reads the shared options, runs the loop and writes the shared
   reports; what the shared options need from a chip is asked through
   this table. Options only one emulator can serve (a bootloader, reset
   causes, AVR breakpoints and profiles) stay inside that backend, and the
   driver refuses them for the other with an error.

   Time is counted in cycles of the chip since the start of the run,
   across resets: now() and the cycle numbers timers get. A timer
   callback gets the cycle it is due at and returns the cycle it wants
   to be called again at, or 0 for once.
   ═══════════════════════════════════════════════════════════════ */

#include <stdint.h>
#include <stdio.h>

typedef uint64_t sim_cycles_t;
typedef sim_cycles_t (*sim_timer_fn)(sim_cycles_t when, void *param);

/* a pin: AVR port letter and bit (B3), or bank 0 and a GPIO number (GP25) */
typedef struct {
  int port;
  int bit;
} sim_pin_t;

/* the state a step leaves the chip in */
enum { SIM_RUNNING = 0, SIM_SLEEPING, SIM_DONE, SIM_CRASHED };

/* analog channels besides the numbered inputs */
#define SIM_ANALOG_SUPPLY (-1)
#define SIM_ANALOG_TEMP   (-2)

typedef struct sim_backend {
  const char *name;   /* simavr, bramble */
  const char *chips;  /* for the usage text */

  /* 1 when this backend runs the file (by its contents), 0 otherwise */
  int (*probe)(const char *path);
  /* its own options: 1 when argv[*i] was taken (*i moves past values),
     0 when it is not one of its, -1 when it is malformed */
  int (*arg)(int *i, int argc, char **argv);
  /* 1 when a shared option can be served; names as typed, "--soft-uart" */
  int (*supports)(const char *option);

  /* create the chip and load the firmware; exits with a message on failure */
  void (*start)(const char *path);
  /* after the stimuli are in place: debugger, --warp, profiling tables */
  void (*prepare)(void);
  /* without the headless options: UART0 (or the soft line) on a PTY */
  void (*bridge)(void);
  /* the last thing before the first step: uploads */
  void (*launch)(void);
  /* one step of the chip; *asleep: cycles of it spent asleep */
  int (*step)(long steps, sim_cycles_t *asleep);
  /* after the driver's checks of a step: traces, breakpoints */
  void (*inspect)(void);
  /* when --max-steps ends the run */
  void (*at_max_steps)(void);
  /* reports at the end, before the shared ones; then the cleanup */
  void (*report)(void);
  void (*stop)(int headless);

  /* primitives for the shared options */
  uint32_t (*hz)(void);
  sim_cycles_t (*now)(void);
  void (*timer)(sim_cycles_t in, sim_timer_fn fn, void *param);
  void (*on_reset)(void (*hook)(void));
  int (*pin)(const char *s, sim_pin_t *pin, const char **end);
  int (*has_pin)(sim_pin_t pin);           /* once the chip exists */
  const char *(*pin_name)(sim_pin_t pin); /* a static buffer */
  void (*drive)(sim_pin_t pin, int level);
  void (*pull)(sim_pin_t pin, int level);
  void (*watch)(sim_pin_t pin, void (*fn)(void *param, int level), void *param);
  /* a voltage at a channel, SIM_ANALOG_SUPPLY or SIM_ANALOG_TEMP; -1 when
     the chip has no such input */
  int (*analog)(int channel, uint32_t mv);
  /* a byte for UART n of the chip; 0 when it has no room for it now */
  int (*uart_send)(int uart, uint8_t byte);
  /* every byte UART n of the chip sends, in place of the PTY */
  void (*uart_listen)(int uart, void (*fn)(uint8_t byte));
} sim_backend_t;

/* the backend of this run */
extern const sim_backend_t *sim;

extern const sim_backend_t sim_backend_avr;
#ifdef PROTOSIM_BRAMBLE
extern const sim_backend_t sim_backend_bramble;
#endif

/* shared options the backends read */
extern int verbose;
extern int opt_warp;
extern double opt_run_ms;

static inline double sim_now_ms(void) { return (double)sim->now() * 1000.0 / sim->hz(); }
static inline sim_cycles_t sim_ms_to_cycles(double ms) { return (sim_cycles_t)(ms * sim->hz() / 1000.0); }

#endif /* PROTOSIM_SIM_H */

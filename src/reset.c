#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reset.h"
#include "sim_regbit.h"

typedef struct {
  double ms;     /* simulated time since the start of the simulation */
  uint64_t at;   /* the same in cycles */
  int cause;
} scheduled_reset_t;

static scheduled_reset_t resets[MAX_RESETS];
static int n_resets = 0;
static int next_reset = 0;

static void (*hooks[MAX_RESET_HOOKS])(avr_t *a);
static int n_hooks = 0;

static uint64_t base_cycles = 0; /* cycles spent before the last reset */
static uint64_t last_cycle = 0;  /* avr->cycle after the previous step */
static uint8_t last_flags = 0;   /* reset flags after the previous step */

static volatile sig_atomic_t signalled_cause = 0;

/* ═══════════════════════════════════════════════════════════════
   Names
   ═══════════════════════════════════════════════════════════════ */

int reset_cause_from_name(const char *name) {
  if (!strcmp(name, "power") || !strcmp(name, "power-on")) return RESET_POWER_ON;
  if (!strcmp(name, "pin") || !strcmp(name, "external")) return RESET_PIN;
  if (!strcmp(name, "brownout") || !strcmp(name, "brown-out")) return RESET_BROWN_OUT;
  if (!strcmp(name, "watchdog")) return RESET_WATCHDOG;
  return -1;
}

const char *reset_cause_name(int cause) {
  switch (cause) {
  case RESET_POWER_ON:  return "power-on";
  case RESET_PIN:       return "pin";
  case RESET_BROWN_OUT: return "brown-out";
  case RESET_WATCHDOG:  return "watchdog";
  }
  return "unknown";
}

/* ═══════════════════════════════════════════════════════════════
   The flags in MCUSR
   ═══════════════════════════════════════════════════════════════ */

uint8_t reset_flags(avr_t *a) {
  return (avr_regbit_get(a, a->reset_flags.porf) ? RESET_POWER_ON : 0) |
         (avr_regbit_get(a, a->reset_flags.extrf) ? RESET_PIN : 0) |
         (avr_regbit_get(a, a->reset_flags.borf) ? RESET_BROWN_OUT : 0) |
         (avr_regbit_get(a, a->reset_flags.wdrf) ? RESET_WATCHDOG : 0);
}

static void reset_flags_put(avr_t *a, uint8_t flags) {
  avr_regbit_setto(a, a->reset_flags.porf, (flags & RESET_POWER_ON) != 0);
  avr_regbit_setto(a, a->reset_flags.extrf, (flags & RESET_PIN) != 0);
  avr_regbit_setto(a, a->reset_flags.borf, (flags & RESET_BROWN_OUT) != 0);
  avr_regbit_setto(a, a->reset_flags.wdrf, (flags & RESET_WATCHDOG) != 0);
}

/* ═══════════════════════════════════════════════════════════════
   Arguments
   ═══════════════════════════════════════════════════════════════ */

int add_reset_arg(const char *arg) {
  char *end = NULL;
  double ms = strtod(arg, &end);
  int cause = RESET_PIN;

  if (end == arg || ms < 0) return -1;
  if (*end == ':') {
    cause = reset_cause_from_name(end + 1);
    if (cause < 0) return -1;
  } else if (*end != '\0') {
    return -1;
  }
  if (n_resets == MAX_RESETS) {
    fprintf(stderr, "WARN: more than %d --reset-at, ignoring '%s'\n", MAX_RESETS, arg);
    return 0;
  }
  resets[n_resets].ms = ms;
  resets[n_resets].cause = cause;
  n_resets++;
  return 0;
}

static int by_time(const void *x, const void *y) {
  const scheduled_reset_t *a = x, *b = y;
  return (a->at > b->at) - (a->at < b->at);
}

/* ═══════════════════════════════════════════════════════════════
   Resets
   ═══════════════════════════════════════════════════════════════ */

uint64_t reset_total_cycles(avr_t *a) { return base_cycles + a->cycle; }

void reset_add_hook(void (*hook)(avr_t *a)) {
  if (n_hooks < MAX_RESET_HOOKS) hooks[n_hooks++] = hook;
}

/* What every reset ends with, whoever made it. */
static void after_reset(avr_t *a, int cause) {
  for (int i = 0; i < n_hooks; i++) hooks[i](a);
  last_cycle = a->cycle;
  last_flags = reset_flags(a);
  printf("[RST] %s reset at %.3f ms → PC=0x%04x MCUSR=0x%02x\n", reset_cause_name(cause),
         (double)base_cycles * 1000.0 / a->frequency, a->pc, last_flags);
}

void reset_setup(avr_t *a, int first_cause) {
  for (int i = 0; i < n_resets; i++)
    resets[i].at = (uint64_t)(resets[i].ms * a->frequency / 1000.0);
  qsort(resets, n_resets, sizeof(resets[0]), by_time);

  /* simavr starts every part as after a power-on */
  if (first_cause != RESET_POWER_ON) {
    reset_flags_put(a, (uint8_t)first_cause);
    printf("[RST] first start reports a %s reset, MCUSR=0x%02x\n", reset_cause_name(first_cause), reset_flags(a));
  }
  last_cycle = a->cycle;
  last_flags = reset_flags(a);
}

void reset_now(avr_t *a, int cause) {
  /* a power-on clears the other flags; any other reset adds its own */
  uint8_t kept = cause == RESET_POWER_ON ? 0 : reset_flags(a);

  base_cycles += a->cycle;
  avr_reset(a); /* clears the I/O registers, MCUSR among them */
  if (cause == RESET_POWER_ON)
    memset(a->data + a->ioend + 1, 0, a->ramend - a->ioend);
  reset_flags_put(a, kept | (uint8_t)cause);
  after_reset(a, cause);
}

void reset_after_step(avr_t *a) {
  if (a->cycle < last_cycle) {
    /* simavr reset the part itself: its watchdog fired, or a debugger asked.
       It cleared MCUSR on the way; a chip keeps the earlier flags. A reset
       that left no flag of its own is a pull on the reset line. */
    uint8_t now = reset_flags(a);
    int cause = (now & RESET_WATCHDOG) ? RESET_WATCHDOG : RESET_PIN;

    base_cycles += last_cycle;
    reset_flags_put(a, last_flags | now | (uint8_t)cause);
    after_reset(a, cause);
  }

  if (signalled_cause) {
    int cause = signalled_cause;
    signalled_cause = 0;
    reset_now(a, cause);
  }

  while (next_reset < n_resets && reset_total_cycles(a) >= resets[next_reset].at)
    reset_now(a, resets[next_reset++].cause);

  last_cycle = a->cycle;
  last_flags = reset_flags(a);
}

/* ═══════════════════════════════════════════════════════════════
   Signals: a reset button and a power switch for a running simulation
   ═══════════════════════════════════════════════════════════════ */

#if defined(SIGUSR1) && defined(SIGUSR2)
static void on_signal(int sig) {
  signalled_cause = sig == SIGUSR1 ? RESET_PIN : RESET_POWER_ON;
}

void reset_install_signals(void) {
  signal(SIGUSR1, on_signal);
  signal(SIGUSR2, on_signal);
}
#else
void reset_install_signals(void) {}
#endif

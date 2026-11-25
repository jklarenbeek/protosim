#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stimulus.h"
#include "sim.h"

/* The outside world of the firmware, for any backend: what it needs from
   the chip is in sim.h. */

static double now_ms(void) { return sim_now_ms(); }

static sim_cycles_t ms_to_cycles(double ms) { return sim_ms_to_cycles(ms); }

/* in how many cycles a time of the whole simulation is due; 1 if it is past */
static sim_cycles_t cycles_until(double ms) {
  uint64_t at = (uint64_t)(ms * sim->hz() / 1000.0);
  uint64_t now = sim->now();
  return at > now ? at - now : 1;
}

/* ═══════════════════════════════════════════════════════════════
   Pins
   ═══════════════════════════════════════════════════════════════ */

static int parse_pin(const char *s, sim_pin_t *pin, const char **end) { return sim->pin(s, pin, end); }

static void need_pin(sim_pin_t pin) {
  if (!sim->has_pin(pin)) {
    fprintf(stderr, "ERROR: this part has no pin %s\n", sim->pin_name(pin));
    exit(1);
  }
}

/* ═══════════════════════════════════════════════════════════════
   A serial line on two pins

   From the part: a falling edge on its TX pin starts a byte, which is
   sampled in the middle of each bit. To the part: the bits of a byte are
   put on its RX pin one bit time apart. The line is idle for `gap` bit
   times before the next byte, as it is when a program at the other end
   paces what it sends; firmware that receives in an interrupt handler
   needs that time to take the byte.
   ═══════════════════════════════════════════════════════════════ */

static struct {
  int on;
  sim_pin_t rx, tx; /* as the part sees them */
  uint32_t baud, gap;
  sim_cycles_t bit;
  stimulus_bridge_t bridge;
  /* from the part */
  int level, busy, nbits;
  uint8_t shift;
  /* to the part */
  uint16_t frame;
  int left;
} su;

int stimulus_has_soft_uart(void) { return su.on; }

int stimulus_soft_uart_arg(const char *arg) {
  const char *p;
  su.baud = 9600;
  su.gap = 20;
  if (parse_pin(arg, &su.rx, &p) < 0 || *p != ',' || parse_pin(p + 1, &su.tx, &p) < 0) return -1;
  if (*p == ',') {
    su.baud = (uint32_t)strtoul(p + 1, (char **)&p, 10);
    if (*p == ',') su.gap = (uint32_t)strtoul(p + 1, (char **)&p, 10);
  }
  if (*p != '\0' || su.baud == 0) return -1;
  su.on = 1;
  return 0;
}

static void su_xon(void) {
  if (su.bridge.xon) su.bridge.xon();
}

static sim_cycles_t su_sample(sim_cycles_t when, void *param) {
  if (su.nbits < 8) {
    su.shift = (uint8_t)((su.shift >> 1) | (su.level ? 0x80 : 0));
    su.nbits++;
    return when + su.bit;
  }
  /* the middle of the stop bit */
  if (su.level && su.bridge.from_part) su.bridge.from_part(su.shift);
  su.busy = 0;
  return 0;
}

static void su_tx_changed(void *param, int level) {
  su.level = level;
  if (!su.level && !su.busy) {
    su.busy = 1;
    su.nbits = 0;
    sim->timer(su.bit + su.bit / 2, su_sample, NULL);
  }
}

static sim_cycles_t su_drive(sim_cycles_t when, void *param) {
  if (su.left > 0) {
    sim->drive(su.rx, su.frame & 1);
    su.frame >>= 1;
    su.left--;
    return when + (su.left ? su.bit : su.bit * (1 + su.gap));
  }
  su_xon(); /* the line is free again */
  return 0;
}

void stimulus_soft_uart_put(uint8_t byte) {
  if (su.bridge.xoff) su.bridge.xoff(); /* one byte at a time */
  su.frame = (uint16_t)((byte << 1) | 0x200); /* start, 8 data, stop */
  su.left = 10;
  sim->timer(1, su_drive, NULL);
}

void stimulus_soft_uart_bridge(const stimulus_bridge_t *bridge) { su.bridge = *bridge; }

static void su_after_reset(void) {
  su.busy = 0;
  su.left = 0;
  sim->drive(su.rx, 1);
  su_xon();
}

static void su_setup(void) {
  su.bit = sim->hz() / su.baud;
  need_pin(su.rx);
  need_pin(su.tx);
  su.level = 1;
  sim->watch(su.tx, su_tx_changed, NULL);
  sim->drive(su.rx, 1); /* the line idles high */
  sim->on_reset(su_after_reset);
  printf("[PIN] serial line on two pins: the part receives on %s", sim->pin_name(su.rx));
  printf(" and sends on %s, %u baud\n", sim->pin_name(su.tx), su.baud);
}

/* ═══════════════════════════════════════════════════════════════
   Levels, waves, pulls
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  sim_pin_t pin;
  double at_ms, period_ms, to_ms; /* period 0: one level */
  int level;
} drive_t;

static drive_t drives[STIMULUS_MAX];
static int n_drives;

static struct {
  sim_pin_t pin;
  int level;
} pulls[STIMULUS_MAX];
static int n_pulls;

int stimulus_pin_wave_arg(const char *arg) {
  const char *p;
  drive_t d = {.level = 1, .to_ms = -1};
  if (n_drives == STIMULUS_MAX || parse_pin(arg, &d.pin, &p) < 0 || *p != ':') return -1;
  d.period_ms = strtod(p + 1, (char **)&p);
  if (*p == ':') d.at_ms = strtod(p + 1, (char **)&p);
  if (*p == ':') d.to_ms = strtod(p + 1, (char **)&p);
  if (*p != '\0' || d.period_ms <= 0) return -1;
  drives[n_drives++] = d;
  return 0;
}

int stimulus_pin_set_arg(const char *arg) {
  const char *p;
  drive_t d = {.to_ms = -1};
  if (n_drives == STIMULUS_MAX || parse_pin(arg, &d.pin, &p) < 0 || *p != ':' || (p[1] != '0' && p[1] != '1')) return -1;
  d.level = p[1] - '0';
  p += 2;
  if (*p == '@') d.at_ms = strtod(p + 1, (char **)&p);
  if (*p != '\0') return -1;
  drives[n_drives++] = d;
  return 0;
}

int stimulus_pin_pull_arg(const char *arg) {
  const char *p;
  if (n_pulls == STIMULUS_MAX || parse_pin(arg, &pulls[n_pulls].pin, &p) < 0 || *p != ':' || (p[1] != '0' && p[1] != '1') || p[2] != '\0')
    return -1;
  pulls[n_pulls++].level = p[1] - '0';
  return 0;
}

static sim_cycles_t drive_timer(sim_cycles_t when, void *param) {
  drive_t *d = param;
  if (d->period_ms > 0 && d->to_ms >= 0 && now_ms() >= d->to_ms) return 0; /* the wave ends where it is */
  sim->drive(d->pin, d->level);
  if (d->period_ms <= 0) return 0;
  d->level = !d->level;
  return when + ms_to_cycles(d->period_ms / 2);
}

/* ═══════════════════════════════════════════════════════════════
   Watching pins
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  sim_pin_t pin;
  double window_ms; /* 0: log every change */
  int level;
  uint64_t since, high; /* cycles */
} watch_t;

static watch_t watches[STIMULUS_MAX];
static int n_watches;

int stimulus_pin_log_arg(const char *arg) {
  const char *p;
  if (n_watches == STIMULUS_MAX || parse_pin(arg, &watches[n_watches].pin, &p) < 0 || *p != '\0') return -1;
  watches[n_watches].level = -1;
  watches[n_watches++].window_ms = 0;
  return 0;
}

int stimulus_pin_duty_arg(const char *arg) {
  const char *p;
  watch_t w = {.window_ms = 100};
  if (n_watches == STIMULUS_MAX || parse_pin(arg, &w.pin, &p) < 0) return -1;
  if (*p == ':') w.window_ms = strtod(p + 1, (char **)&p);
  if (*p != '\0' || w.window_ms <= 0) return -1;
  watches[n_watches++] = w;
  return 0;
}

static void watch_changed(void *param, int level) {
  watch_t *w = param;
  uint64_t now = sim->now();
  if (level == w->level) return;
  if (w->window_ms > 0) {
    if (w->level == 1) w->high += now - w->since;
    w->since = now;
  } else {
    printf("[PIN] %s=%d at %.3f ms\n", sim->pin_name(w->pin), level, now_ms());
  }
  w->level = level;
}

static sim_cycles_t duty_timer(sim_cycles_t when, void *param) {
  watch_t *w = param;
  uint64_t now = sim->now();
  uint64_t window = ms_to_cycles(w->window_ms);
  if (w->level == 1) w->high += now - w->since;
  w->since = now;
  printf("[DUTY] %s %.1f%% at %.3f ms\n", sim->pin_name(w->pin), 100.0 * (double)w->high / (double)window, now_ms());
  w->high = 0;
  return when + window;
}

/* ═══════════════════════════════════════════════════════════════
   Voltages
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  int channel; /* 0..15, SIM_ANALOG_TEMP, or SIM_ANALOG_SUPPLY */
  uint32_t mv;
  double at_ms;
} volt_t;

static volt_t volts[STIMULUS_MAX];
static int n_volts;

static int volt_arg(const char *p, int channel) {
  volt_t v = {.channel = channel};
  if (n_volts == STIMULUS_MAX) return -1;
  v.mv = (uint32_t)strtoul(p, (char **)&p, 10);
  if (*p == '@') v.at_ms = strtod(p + 1, (char **)&p);
  if (*p != '\0') return -1;
  volts[n_volts++] = v;
  return 0;
}

int stimulus_adc_arg(const char *arg) {
  const char *p = strchr(arg, ':');
  int channel;
  if (!p) return -1;
  if (!strncmp(arg, "temp", 4) && arg + 4 == p) {
    channel = SIM_ANALOG_TEMP;
  } else {
    channel = (int)strtol(arg, NULL, 10);
    if (arg[0] < '0' || arg[0] > '9' || channel > 15) return -1;
  }
  return volt_arg(p + 1, channel);
}

int stimulus_vcc_arg(const char *arg) { return volt_arg(arg, SIM_ANALOG_SUPPLY); }

static sim_cycles_t volt_timer(sim_cycles_t when, void *param) {
  volt_t *v = param;
  sim->analog(v->channel, v->mv);
  if (v->channel == SIM_ANALOG_SUPPLY) printf("[ADC] supply %u mV at %.3f ms\n", v->mv, now_ms());
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
   Setup
   ═══════════════════════════════════════════════════════════════ */

/* What is still to come is registered again after a reset, each at its own
   time of the whole simulation (simavr drops every pending cycle timer at a
   reset). */
static void arm(int after_reset) {
  for (int i = 0; i < n_drives; i++) {
    drive_t *d = &drives[i];
    if (d->period_ms <= 0 && now_ms() > d->at_ms && after_reset) continue; /* a level that was set stays set */
    sim->timer(cycles_until(d->at_ms), drive_timer, d);
  }
  for (int i = 0; i < n_watches; i++)
    if (watches[i].window_ms > 0) sim->timer(ms_to_cycles(watches[i].window_ms), duty_timer, &watches[i]);
  for (int i = 0; i < n_volts; i++)
    if (!after_reset || now_ms() <= volts[i].at_ms) sim->timer(cycles_until(volts[i].at_ms), volt_timer, &volts[i]);
}

static void arm_after_reset(void) { arm(1); }

void stimulus_setup(void) {
  for (int i = 0; i < n_pulls; i++) {
    need_pin(pulls[i].pin);
    sim->pull(pulls[i].pin, pulls[i].level);
  }
  for (int i = 0; i < n_drives; i++) need_pin(drives[i].pin);
  for (int i = 0; i < n_watches; i++) {
    need_pin(watches[i].pin);
    sim->watch(watches[i].pin, watch_changed, &watches[i]);
  }
  /* voltages given without a time are there before the first instruction */
  for (int i = 0; i < n_volts; i++)
    if (volts[i].at_ms <= 0 && volts[i].channel == SIM_ANALOG_SUPPLY) sim->analog(SIM_ANALOG_SUPPLY, volts[i].mv);

  if (su.on) su_setup();
  arm(0);
  if (n_drives || n_watches || n_volts) sim->on_reset(arm_after_reset);
}

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stimulus.h"
#include "reset.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "sim_time.h"

static avr_t *avr;

static double now_ms(void) {
  return (double)reset_total_cycles(avr) * 1000.0 / avr->frequency;
}

static avr_cycle_count_t ms_to_cycles(double ms) {
  return (avr_cycle_count_t)(ms * avr->frequency / 1000.0);
}

/* in how many cycles a time of the whole simulation is due; 1 if it is past */
static avr_cycle_count_t cycles_until(double ms) {
  uint64_t at = (uint64_t)(ms * avr->frequency / 1000.0);
  uint64_t now = reset_total_cycles(avr);
  return at > now ? at - now : 1;
}

/* ═══════════════════════════════════════════════════════════════
   Pins
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  char port;
  int bit;
} pin_t;

static int parse_pin(const char *s, pin_t *pin, const char **end) {
  char port = s[0] & ~0x20; /* upper case */
  if (port < 'A' || port > 'L' || s[1] < '0' || s[1] > '7') return -1;
  pin->port = port;
  pin->bit = s[1] - '0';
  if (end) *end = s + 2;
  return 0;
}

static avr_irq_t *pin_irq(pin_t pin) {
  avr_irq_t *irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(pin.port), pin.bit);
  if (!irq) {
    fprintf(stderr, "ERROR: this part has no pin %c%d\n", pin.port, pin.bit);
    exit(1);
  }
  return irq;
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

enum { SU_FROM_PART = 0, SU_TO_PART, SU_XON, SU_XOFF, SU_COUNT };
static const char *su_names[SU_COUNT] = {"8<soft.from", "8>soft.to", ">soft.xon", ">soft.xoff"};

static struct {
  int on;
  pin_t rx, tx; /* as the part sees them */
  uint32_t baud, gap;
  avr_cycle_count_t bit;
  avr_irq_t *irq, *rx_irq;
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

static avr_cycle_count_t su_sample(avr_t *a, avr_cycle_count_t when, void *param) {
  if (su.nbits < 8) {
    su.shift = (uint8_t)((su.shift >> 1) | (su.level ? 0x80 : 0));
    su.nbits++;
    return when + su.bit;
  }
  /* the middle of the stop bit */
  if (su.level) avr_raise_irq(su.irq + SU_FROM_PART, su.shift);
  su.busy = 0;
  return 0;
}

static void su_tx_changed(struct avr_irq_t *irq, uint32_t value, void *param) {
  su.level = value != 0;
  if (!su.level && !su.busy) {
    su.busy = 1;
    su.nbits = 0;
    avr_cycle_timer_register(avr, su.bit + su.bit / 2, su_sample, NULL);
  }
}

static avr_cycle_count_t su_drive(avr_t *a, avr_cycle_count_t when, void *param) {
  if (su.left > 0) {
    avr_raise_irq(su.rx_irq, su.frame & 1);
    su.frame >>= 1;
    su.left--;
    return when + (su.left ? su.bit : su.bit * (1 + su.gap));
  }
  avr_raise_irq(su.irq + SU_XON, 1); /* the line is free again */
  return 0;
}

static void su_byte_for_part(struct avr_irq_t *irq, uint32_t value, void *param) {
  avr_raise_irq(su.irq + SU_XOFF, 1); /* one byte at a time */
  su.frame = (uint16_t)(((value & 0xFF) << 1) | 0x200); /* start, 8 data, stop */
  su.left = 10;
  avr_cycle_timer_register(avr, 1, su_drive, NULL);
}

static void su_after_reset(avr_t *a) {
  su.busy = 0;
  su.left = 0;
  avr_raise_irq(su.rx_irq, 1);
  avr_raise_irq(su.irq + SU_XON, 1);
}

static void su_setup(void) {
  su.bit = avr->frequency / su.baud;
  su.irq = avr_alloc_irq(&avr->irq_pool, 0, SU_COUNT, su_names);
  su.rx_irq = pin_irq(su.rx);
  su.level = 1;
  avr_irq_register_notify(pin_irq(su.tx), su_tx_changed, NULL);
  avr_irq_register_notify(su.irq + SU_TO_PART, su_byte_for_part, NULL);
  avr_raise_irq(su.rx_irq, 1); /* the line idles high */
  reset_add_hook(su_after_reset);
  printf("[PIN] serial line on two pins: the part receives on %c%d and sends on %c%d, %u baud\n",
         su.rx.port, su.rx.bit, su.tx.port, su.tx.bit, su.baud);
}

void stimulus_soft_uart_irqs(avr_irq_t **from_part, avr_irq_t **to_part, avr_irq_t **xon, avr_irq_t **xoff) {
  *from_part = su.irq + SU_FROM_PART;
  *to_part = su.irq + SU_TO_PART;
  *xon = su.irq + SU_XON;
  *xoff = su.irq + SU_XOFF;
}

/* ═══════════════════════════════════════════════════════════════
   Levels, waves, pulls
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  pin_t pin;
  double at_ms, period_ms, to_ms; /* period 0: one level */
  int level;
  avr_irq_t *irq;
} drive_t;

static drive_t drives[STIMULUS_MAX];
static int n_drives;

static struct {
  pin_t pin;
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

static avr_cycle_count_t drive_timer(avr_t *a, avr_cycle_count_t when, void *param) {
  drive_t *d = param;
  if (d->period_ms > 0 && d->to_ms >= 0 && now_ms() >= d->to_ms) return 0; /* the wave ends where it is */
  avr_raise_irq(d->irq, d->level);
  if (d->period_ms <= 0) return 0;
  d->level = !d->level;
  return when + ms_to_cycles(d->period_ms / 2);
}

/* ═══════════════════════════════════════════════════════════════
   Watching pins
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  pin_t pin;
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

static void watch_changed(struct avr_irq_t *irq, uint32_t value, void *param) {
  watch_t *w = param;
  int level = value != 0;
  uint64_t now = reset_total_cycles(avr);
  if (level == w->level) return;
  if (w->window_ms > 0) {
    if (w->level == 1) w->high += now - w->since;
    w->since = now;
  } else {
    printf("[PIN] %c%d=%d at %.3f ms\n", w->pin.port, w->pin.bit, level, now_ms());
  }
  w->level = level;
}

static avr_cycle_count_t duty_timer(avr_t *a, avr_cycle_count_t when, void *param) {
  watch_t *w = param;
  uint64_t now = reset_total_cycles(avr);
  uint64_t window = ms_to_cycles(w->window_ms);
  if (w->level == 1) w->high += now - w->since;
  w->since = now;
  printf("[DUTY] %c%d %.1f%% at %.3f ms\n", w->pin.port, w->pin.bit, 100.0 * (double)w->high / (double)window, now_ms());
  w->high = 0;
  return when + window;
}

/* ═══════════════════════════════════════════════════════════════
   Voltages
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  int channel; /* 0..15, ADC_IRQ_TEMP, or -1 for the supply */
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
    channel = ADC_IRQ_TEMP;
  } else {
    channel = (int)strtol(arg, NULL, 10);
    if (arg[0] < '0' || arg[0] > '9' || channel > 15) return -1;
  }
  return volt_arg(p + 1, channel);
}

int stimulus_vcc_arg(const char *arg) { return volt_arg(arg, -1); }

static avr_cycle_count_t volt_timer(avr_t *a, avr_cycle_count_t when, void *param) {
  volt_t *v = param;
  if (v->channel < 0) {
    avr->vcc = avr->avcc = v->mv;
    printf("[ADC] supply %u mV at %.3f ms\n", v->mv, now_ms());
  } else {
    avr_irq_t *irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, v->channel);
    if (irq) avr_raise_irq(irq, v->mv);
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
   Setup
   ═══════════════════════════════════════════════════════════════ */

/* simavr drops every pending cycle timer at a reset: what is still to come
   is registered again, each at its own time of the whole simulation */
static void arm(avr_t *a) {
  for (int i = 0; i < n_drives; i++) {
    drive_t *d = &drives[i];
    if (d->period_ms <= 0 && now_ms() > d->at_ms && a) continue; /* a level that was set stays set */
    avr_cycle_timer_register(avr, cycles_until(d->at_ms), drive_timer, d);
  }
  for (int i = 0; i < n_watches; i++)
    if (watches[i].window_ms > 0)
      avr_cycle_timer_register(avr, ms_to_cycles(watches[i].window_ms), duty_timer, &watches[i]);
  for (int i = 0; i < n_volts; i++)
    if (!a || now_ms() <= volts[i].at_ms) avr_cycle_timer_register(avr, cycles_until(volts[i].at_ms), volt_timer, &volts[i]);
}

void stimulus_setup(avr_t *a) {
  avr = a;

  for (int i = 0; i < n_pulls; i++) {
    avr_ioport_external_t pull = {.name = pulls[i].pin.port, .mask = (uint8_t)(1 << pulls[i].pin.bit),
                                  .value = (uint8_t)(pulls[i].level << pulls[i].pin.bit)};
    avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(pulls[i].pin.port), &pull);
  }
  for (int i = 0; i < n_drives; i++) drives[i].irq = pin_irq(drives[i].pin);
  for (int i = 0; i < n_watches; i++) avr_irq_register_notify(pin_irq(watches[i].pin), watch_changed, &watches[i]);
  /* voltages given without a time are there before the first instruction */
  for (int i = 0; i < n_volts; i++)
    if (volts[i].at_ms <= 0 && volts[i].channel < 0) avr->vcc = avr->avcc = volts[i].mv;

  if (su.on) su_setup();
  arm(NULL);
  if (n_drives || n_watches || n_volts) reset_add_hook(arm);
}

/* ═══════════════════════════════════════════════════════════════
   Sleep
   ═══════════════════════════════════════════════════════════════ */

static void sleep_at_once(avr_t *a, avr_cycle_count_t how_long) {}

void stimulus_warp(avr_t *a) { a->sleep = sleep_at_once; }

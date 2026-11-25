/*
    backend_bramble.c
    The Bramble backend: the RP2040 (Cortex-M0+) and the RP2350 (Cortex-M33),
    from UF2 or ELF, on the emulator in libraries/bramble.

    Bramble is an application: its parts are linked in here without its
    main(), and this file is the run loop around them. What it adds:
      - the shared options of protosim (pins, ADC, UART files, run time)
      - time that keeps going on across a watchdog reboot
      - the watchdog counting on the Arm cores (Bramble ticks it only for
        RISC-V)
      - --warp: while every core sleeps, time jumps to the next thing that
        can wake one, instead of one cycle a step
      - the share of time asleep
    The RISC-V cores of the RP2350 are not served yet.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator.h"
#include "gpio.h"
#include "timer.h"
#include "uart.h"
#include "adc.h"
#include "nvic.h"
#include "clocks.h"
#include "rom.h"
#include "spi.h"
#include "i2c.h"
#include "pwm.h"
#include "dma.h"
#include "pio.h"
#include "usb.h"
#include "rtc.h"
#include "devtools.h"
#include "rp2350_arm/m33_cpu.h"
#include "rp2350_rv/rp2350_periph.h"
#include "rp2350_rv/picobin.h"

#include "sim.h"
#include "pty_line.h"

/* ═══════════════════════════════════════════════════════════════
   Options
   ═══════════════════════════════════════════════════════════════ */

static const char *chip = NULL; /* rp2040 | rp2350; NULL: by the file */
static uint32_t mhz = 0;        /* 0: 125 on the RP2040, 150 on the RP2350 */
static int is_m33 = 0;
static int headless = 1;

static int bramble_probe(const char *path) {
  size_t n = strlen(path);
  if (n > 4 && !strcmp(path + n - 4, ".uf2")) return 1;
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  unsigned char h[20] = {0};
  size_t got = fread(h, 1, sizeof(h), fp);
  fclose(fp);
  if (got != sizeof(h)) return 0;
  /* UF2: "UF2\n" and its second magic */
  if (h[0] == 'U' && h[1] == 'F' && h[2] == '2' && h[3] == '\n') return 1;
  /* an ELF file for Arm (40) */
  return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[18] == 40 && h[19] == 0;
}

static int bramble_arg(int *ip, int argc, char **argv) {
  int i = *ip;
  const char *a = argv[i];
  if (!strcmp(a, "-m") && i + 1 < argc) {
    chip = argv[++i];
    if (strcmp(chip, "rp2040") && strcmp(chip, "rp2350")) {
      fprintf(stderr, "ERROR: -m %s: Bramble runs rp2040 and rp2350 (Arm cores)\n", chip);
      return -1;
    }
  } else if (!strcmp(a, "-f") && i + 1 < argc) {
    uint32_t hz = (uint32_t)strtoul(argv[++i], NULL, 10);
    if (hz < 1000000 || hz % 1000000) {
      fprintf(stderr, "ERROR: -f %s: Bramble counts whole MHz\n", argv[i]);
      return -1;
    }
    mhz = hz / 1000000;
  } else {
    return 0;
  }
  *ip = i;
  return 1;
}

static int bramble_supports(const char *option) {
  /* no supply measurement, and UARTs in place of a line on two pins */
  return strcmp(option, "--soft-uart") && strcmp(option, "--vcc") && strcmp(option, "--wait-tcp");
}

/* ═══════════════════════════════════════════════════════════════
   Time
   ═══════════════════════════════════════════════════════════════ */

static sim_cycles_t base_cycles; /* before the last reboot */

static sim_cycles_t local_cycles(void) {
  return timer_state.time_us * timing_config.cycles_per_us + timing_config.cycle_accumulator;
}

static uint32_t bramble_hz(void) { return timing_config.cycles_per_us * 1000000u; }
static sim_cycles_t bramble_now(void) { return base_cycles + local_cycles(); }

/* the timers of the shared options: a function and a parameter, as simavr
   knows them; registering a pair again moves it */
typedef struct {
  sim_cycles_t due;
  sim_timer_fn fn;
  void *param;
  int on;
} timer_t_;

#define MAX_TIMERS 128
static timer_t_ timers[MAX_TIMERS];
static int n_timers;
static sim_cycles_t next_due = UINT64_MAX;

static void recompute_next(void) {
  next_due = UINT64_MAX;
  for (int i = 0; i < n_timers; i++)
    if (timers[i].on && timers[i].due < next_due) next_due = timers[i].due;
}

static void bramble_timer(sim_cycles_t in, sim_timer_fn fn, void *param) {
  timer_t_ *t = NULL;
  for (int i = 0; i < n_timers && !t; i++)
    if (timers[i].fn == fn && timers[i].param == param) t = &timers[i];
  for (int i = 0; i < n_timers && !t; i++)
    if (!timers[i].on) t = &timers[i];
  if (!t) {
    if (n_timers == MAX_TIMERS) {
      fprintf(stderr, "ERROR: too many timers\n");
      exit(1);
    }
    t = &timers[n_timers++];
  }
  t->fn = fn;
  t->param = param;
  t->due = bramble_now() + (in ? in : 1);
  t->on = 1;
  if (t->due < next_due) next_due = t->due;
}

static void run_timers(void) {
  sim_cycles_t now = bramble_now();
  while (next_due <= now) {
    timer_t_ *t = NULL;
    for (int i = 0; i < n_timers; i++)
      if (timers[i].on && timers[i].due <= now && (!t || timers[i].due < t->due)) t = &timers[i];
    if (!t) break;
    t->on = 0;
    sim_timer_fn fn = t->fn;
    void *param = t->param;
    sim_cycles_t next = fn(t->due, param);
    if (next) {
      /* the callback may have registered itself again meanwhile */
      int again = 0;
      for (int i = 0; i < n_timers; i++)
        if (timers[i].on && timers[i].fn == fn && timers[i].param == param) again = 1;
      if (!again) {
        t->fn = fn;
        t->param = param;
        t->due = next > now ? next : now + 1;
        t->on = 1;
      }
    }
    recompute_next();
  }
}

#define MAX_RESET_HOOKS 8
static void (*reset_hooks[MAX_RESET_HOOKS])(void);
static int n_reset_hooks;

static void bramble_on_reset(void (*hook)(void)) {
  if (n_reset_hooks < MAX_RESET_HOOKS) reset_hooks[n_reset_hooks++] = hook;
}

/* ═══════════════════════════════════════════════════════════════
   Pins: GP0 to GP29 (GP47 on an RP2350B)
   ═══════════════════════════════════════════════════════════════ */

static int bramble_pin(const char *s, sim_pin_t *pin, const char **end) {
  const char *p = s;
  if ((p[0] == 'G' || p[0] == 'g') && (p[1] == 'P' || p[1] == 'p')) p += 2;
  if (*p < '0' || *p > '9') return -1;
  char *e;
  long n = strtol(p, &e, 10);
  if (n < 0 || n >= NUM_GPIO_PINS) return -1;
  pin->port = 0;
  pin->bit = (int)n;
  if (end) *end = e;
  return 0;
}

static int bramble_has_pin(sim_pin_t pin) { return pin.bit >= 0 && pin.bit < gpio_num_user_pins(); }

static const char *bramble_pin_name(sim_pin_t pin) {
  static char name[8];
  snprintf(name, sizeof(name), "GP%d", pin.bit);
  return name;
}

static void bramble_drive(sim_pin_t pin, int level) { gpio_set_input_pin((uint8_t)pin.bit, (uint8_t)level); }

/* an input reads the pull while nothing drives it: on Bramble the input
   level stands apart from the output */
static void bramble_pull(sim_pin_t pin, int level) { gpio_set_input_pin((uint8_t)pin.bit, (uint8_t)level); }

typedef struct {
  int bit;
  void (*fn)(void *param, int level);
  void *param;
} pin_watch_t;

#define MAX_WATCHES 32
static pin_watch_t watches[MAX_WATCHES];
static int n_watches;
static uint64_t watched_mask, last_levels;

/* what the pins carry: SIO outputs where enabled, the inputs elsewhere.
   Bramble does not put PWM or PIO outputs on its pins (BR6). */
static uint64_t pin_levels(void) {
  return ((uint64_t)gpio_state.gpio_out & gpio_state.gpio_oe) | ((uint64_t)gpio_state.gpio_in & ~gpio_state.gpio_oe);
}

static void bramble_watch(sim_pin_t pin, void (*fn)(void *param, int level), void *param) {
  if (n_watches == MAX_WATCHES) return;
  watches[n_watches++] = (pin_watch_t){pin.bit, fn, param};
  watched_mask |= 1ull << pin.bit;
}

static void check_watches(void) {
  uint64_t now = pin_levels() & watched_mask;
  uint64_t changed = now ^ last_levels;
  if (!changed) return;
  last_levels = now;
  for (int i = 0; i < n_watches; i++)
    if (changed & (1ull << watches[i].bit)) watches[i].fn(watches[i].param, (int)((now >> watches[i].bit) & 1));
}

/* ═══════════════════════════════════════════════════════════════
   ADC: inputs 0 to 3 on GP26 to GP29, 4 the temperature sensor;
   12 bits of a 3.3 V reference
   ═══════════════════════════════════════════════════════════════ */

static int bramble_analog(int channel, uint32_t mv) {
  if (channel == SIM_ANALOG_TEMP) channel = 4;
  if (channel < 0 || channel >= ADC_NUM_CHANNELS) return -1;
  uint32_t value = mv * 4096u / 3300u;
  adc_set_channel_value((uint8_t)channel, (uint16_t)(value > 4095 ? 4095 : value));
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
   UARTs
   ═══════════════════════════════════════════════════════════════ */

static void (*listeners[2])(uint8_t byte);
static int pty_uart = -1;

/* Bramble's UART write, renamed when uart.c is built for protosim */
void bramble_uart_write32_hw(int uart_num, uint32_t offset, uint32_t val);

/* every write to a UART register passes here: a byte sent goes to the
   files or the PTY, not to the standard output of protosim */
void uart_write32(int uart_num, uint32_t offset, uint32_t val) {
  if (offset == UART_DR && uart_num >= 0 && uart_num < 2 && (uart_state[uart_num].cr & UART_CR_TXE)) {
    if (listeners[uart_num] || pty_uart == uart_num) {
      if (listeners[uart_num]) listeners[uart_num]((uint8_t)val);
      if (pty_uart == uart_num) pty_line_write((uint8_t)val);
      int quiet = usb_cdc_stdout_enabled;
      usb_cdc_stdout_enabled = 1; /* Bramble prints only without it */
      bramble_uart_write32_hw(uart_num, offset, val);
      usb_cdc_stdout_enabled = quiet;
      return;
    }
  }
  bramble_uart_write32_hw(uart_num, offset, val);
}

/* Bramble's spinlock release, renamed when cpu.c is built for protosim. It
   sets the event register of both cores, which a chip does not: the Pico
   SDK sleeps by releasing a spinlock and then waiting with WFE, and that
   WFE then returned at once, so sleep_ms() never slept (BR9). The SDK sends
   its own SEV where a waiter needs one (lock_internal_spin_unlock_with_notify). */
void bramble_spinlock_release_hw(uint32_t lock_num);

void spinlock_release(uint32_t lock_num) {
  if (lock_num < SPINLOCK_SIZE) spinlocks[lock_num] = 0;
}

/* The TIMER0 of an RP2350 has LOCKED and SOURCE at 0x34 and 0x38, so its
   INTR, INTE, INTF and INTS sit 8 bytes above those of the RP2040. Bramble
   hands the RP2350 offsets to its RP2040 timer unchanged: the alarm
   interrupt was never enabled, and sleep_ms() slept for good (BR15). */
static uint32_t timer0_offset(uint32_t addr) {
  uint32_t off = addr - TIMER_BASE;
  if (off >= 0x3C && off <= 0x48) return addr - 8;
  if (off == 0x34 || off == 0x38) return 0; /* LOCKED, SOURCE: not modelled */
  return addr;
}

uint32_t rp2350_timer0_read32(uint32_t addr) {
  uint32_t a = timer0_offset(addr);
  return a ? timer_read32(a) : 0;
}

void rp2350_timer0_write32(uint32_t addr, uint32_t val) {
  uint32_t a = timer0_offset(addr);
  if (a) timer_write32(a, val);
}

static int bramble_uart_send(int uart, uint8_t byte) {
  if (uart < 0 || uart > 1) return 1;
  uart_state_t *u = &uart_state[uart];
  /* a UART that is off, or full, takes it later: Bramble would count an overrun */
  if (!(u->cr & UART_CR_UARTEN) || !(u->cr & UART_CR_RXE) || u->rx_count >= UART_RX_FIFO_SIZE) return 0;
  return uart_rx_push(uart, byte);
}

static void bramble_uart_listen(int uart, void (*fn)(uint8_t byte)) {
  if (uart >= 0 && uart < 2) listeners[uart] = fn;
}

/* bytes from the PTY wait here until UART0 can take them */
static uint8_t pty_in[256];
static int pty_in_len;

static void poll_pty(void) {
  while (pty_in_len < (int)sizeof(pty_in) && pty_line_read(&pty_in[pty_in_len])) pty_in_len++;
  int sent = 0;
  while (sent < pty_in_len && bramble_uart_send(0, pty_in[sent])) sent++;
  if (sent) {
    memmove(pty_in, pty_in + sent, pty_in_len - sent);
    pty_in_len -= sent;
  }
}

/* ═══════════════════════════════════════════════════════════════
   Starting
   ═══════════════════════════════════════════════════════════════ */

/* After any reset core 1 runs the boot ROM: it drains its mailbox, pushes
   a 0 to core 0 and waits for the launch sequence. Bramble puts it there
   only after a reset of core 1 through the PSM, which the Pico SDK 2.x does
   not make before multicore_launch_core1(): core 1 never started (BR8). */
static void core1_in_bootrom(void) { sio_set_core1_reset(0); }

/* what Bramble's main() does before the first instruction; after a
   watchdog reset the watchdog keeps its reason and scratch registers, which
   clocks_init() clears (BR10): clocks_reset() keeps them */
static void reset_runtime_peripherals(int soft) {
  gpio_init();
  timer_init();
  nvic_init();
  rom_init();
  uart_init();
  spi_init();
  i2c_init();
  pwm_init();
  dma_init();
  pio_init();
  if (soft) clocks_reset(); else clocks_init();
  adc_init();
  usb_init();
  rtc_init();
}

static void bramble_start(const char *path) {
  cpu_init();
  memset(cpu.flash, 0xFF, FLASH_SIZE_MAX);
  reset_runtime_peripherals(0);

  size_t n = strlen(path);
  int loaded = (n > 4 && !strcmp(path + n - 4, ".elf")) ? load_elf(path) : load_uf2(path);
  if (!loaded) {
    fprintf(stderr, "Cannot load %s\n", path);
    exit(1);
  }

  int arch = loader_detected_arch();
  if (arch == FW_ARCH_RV32) {
    fprintf(stderr, "ERROR: %s is for the RISC-V cores of the RP2350: not served by protosim yet\n", path);
    exit(1);
  }
  /* an ELF file says Arm, not which chip: an RP2350 image carries an
     IMAGE_DEF block in its first 4 KB, an RP2040 image does not */
  picobin_info_t pbi = picobin_scan(cpu.flash, 4096);
  is_m33 = chip ? !strcmp(chip, "rp2350") : (arch == FW_ARCH_ARM_M33 || (pbi.found && pbi.is_rp2350 && !pbi.is_riscv));
  if (!mhz) mhz = is_m33 ? 150 : 125;
  timing_config.cycles_per_us = mhz;
  timing_config.cycle_accumulator = 0;

  if (cpu_has_boot2()) cpu_set_boot2(1);
  dual_core_init();
  cpu_reset_core(CORE0);
  core1_in_bootrom();

  if (is_m33) {
    m33_init_overlay();
    membus_rp2350_mode = 1;
    rom_patch_rp2350_arm();
    static uint8_t m33_sram[520 * 1024];
    memset(m33_sram, 0, sizeof(m33_sram));
    rp2350_sram_ptr = m33_sram;
    mem_set_ram_ptr(m33_sram, 0x20000000, 520 * 1024);
    static rp2350_periph_state_t m33_periph;
    rp2350_periph_init(&m33_periph, 1);
    membus_rp2350_periph = &m33_periph;
  }
  printf("[BRAMBLE] %s, Cortex-%s at %u MHz: %s\n", is_m33 ? "RP2350" : "RP2040", is_m33 ? "M33" : "M0+", mhz, path);
}

static void bramble_prepare(void) { last_levels = pin_levels() & watched_mask; }

static void bramble_bridge(void) {
  headless = 0;
  if (pty_line_open('0') == 0) pty_uart = 0;
}

static void bramble_launch(void) {}

/* ═══════════════════════════════════════════════════════════════
   The run
   ═══════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
   IT blocks of the Cortex-M33 (BR12)

   Bramble decodes IT and keeps its condition, but no instruction is ever
   predicated on it: every instruction of an IT block runs. Thumb-2 code is
   full of them (it cs; movcs r2, #1), and the Pico SDK went wrong in its
   first microsecond on an RP2350. The M33 is stepped here instead, with
   the ITSTATE of the architecture kept per core:
     - IT itself is taken here and not given to Bramble
     - an instruction whose condition fails is passed over
     - a 16-bit instruction in a block does not set the flags, other than
       CMP, CMN and TST (Bramble's MOVS, ADDS, ... always would)
     - an exception taken in a block keeps the state for the return
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  uint8_t state;      /* ITSTATE: firstcond and mask */
  uint8_t saved[MAX_EXCEPTION_DEPTH];
  uint32_t resume[MAX_EXCEPTION_DEPTH];
  int depth;
} it_core_t;

static it_core_t it_cores[NUM_CORES];

static int cond_passes(uint32_t xpsr, unsigned cond) {
  int n = (xpsr >> 31) & 1, z = (xpsr >> 30) & 1, c = (xpsr >> 29) & 1, v = (xpsr >> 28) & 1;
  switch (cond >> 1) {
  case 0: return (z ^ (cond & 1));
  case 1: return (c ^ (cond & 1));
  case 2: return (n ^ (cond & 1));
  case 3: return (v ^ (cond & 1));
  case 4: return ((c && !z) ^ (cond & 1));
  case 5: return ((n == v) ^ (cond & 1));
  case 6: return ((!z && n == v) ^ (cond & 1));
  default: return 1;
  }
}

static uint32_t fetch16(uint32_t pc) { return mem_read16(pc); }

static int is_32bit(uint32_t hw) { return (hw >> 11) >= 0x1D; }

/* a 16-bit instruction that sets the flags inside an IT block too */
static int sets_flags_in_it(uint32_t hw) {
  return (hw & 0xF800) == 0x2800 ||  /* CMP immediate */
         (hw & 0xFFC0) == 0x4200 ||  /* TST */
         (hw & 0xFFC0) == 0x4280 ||  /* CMP register */
         (hw & 0xFFC0) == 0x42C0 ||  /* CMN */
         (hw & 0xFF00) == 0x4500;    /* CMP high registers */
}

static void it_advance(it_core_t *it) {
  if ((it->state & 0x7) == 0) it->state = 0;
  else it->state = (uint8_t)((it->state & 0xE0) | ((it->state << 1) & 0x1F));
}

/* one instruction of core c, bound */
static void m33_step_core(int c) {
  it_core_t *it = &it_cores[c];
  cpu_bind_context_t ctx;
  if (!cpu_bind_core_context(c, &ctx)) return;

  uint32_t pc = cpu.r[15];
  /* Bramble fetches instructions below RAM_TOP from the RAM of the RP2040,
     while the M33 reads and writes the 520 KB of the RP2350: code the SDK
     runs from RAM was not there (BR13). The bytes at the PC are copied over
     for the fetch; that RAM is not used otherwise on an RP2350. */
  if (pc - RAM_BASE < RAM_SIZE - 4 && rp2350_sram_ptr) memcpy(&cpu.ram[pc - RAM_BASE], rp2350_sram_ptr + (pc - RAM_BASE), 4);
  /* back from an exception taken inside a block */
  if (it->depth && it->resume[it->depth - 1] == pc && cores[c].exception_depth < it->depth + 0) {
    it->depth--;
    it->state = it->saved[it->depth];
  }

  uint32_t hw = fetch16(pc);
  /* A load relative to the PC with the offset subtracted (U=0: F81F, F83F,
     F85F, F91F, F93F) is decoded by Bramble as a register offset; the
     linker's veneer "ldr.w pc, [pc, #-0]" jumped to address 0 (BR14). */
  if ((hw & 0xFE9F) == 0xF81F && (hw & 0x60) != 0x60 && (!it->state || cond_passes(cpu.xpsr, it->state >> 4))) {
    uint32_t lo = fetch16(pc + 2);
    uint32_t rt = lo >> 12, addr = ((pc + 4) & ~3u) - (lo & 0xFFF);
    unsigned size = (hw >> 5) & 3;
    int sign = (hw >> 8) & 1;
    uint32_t v = size == 2 ? mem_read32(addr) : size == 1 ? mem_read16(addr) : mem_read8(addr);
    if (sign) v = size == 1 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
    if (it->state) it_advance(it);
    if (rt == 15) cpu.r[15] = v & ~1u;
    else {
      cpu.r[rt] = v;
      cpu.r[15] = pc + 4;
    }
    cpu_unbind_core_context(c, &ctx);
    return;
  }
  if ((hw & 0xFF00) == 0xBF00 && (hw & 0xF) != 0 && !it->state) {
    /* IT: the block starts with the next instruction */
    it->state = (uint8_t)(hw & 0xFF);
    cpu.r[15] = pc + 2;
    cpu_unbind_core_context(c, &ctx);
    return;
  }

  if (it->state) {
    int depth = cores[c].exception_depth;
    if (!cond_passes(cpu.xpsr, it->state >> 4)) {
      cpu.r[15] = pc + (is_32bit(hw) ? 4 : 2);
      it_advance(it);
      cpu_unbind_core_context(c, &ctx);
      return;
    }
    uint32_t flags = cpu.xpsr & 0xF0000000u;
    uint8_t state = it->state;
    cpu_step();
    if (cores[c].exception_depth > depth) {
      /* taken before the instruction: it runs after the return */
      if (it->depth < MAX_EXCEPTION_DEPTH) {
        it->saved[it->depth] = state;
        it->resume[it->depth] = pc;
        it->depth++;
      }
      it->state = 0;
    } else {
      if (!is_32bit(hw) && !sets_flags_in_it(hw)) cpu.xpsr = (cpu.xpsr & 0x0FFFFFFFu) | flags;
      it_advance(it);
    }
  } else {
    cpu_step();
  }
  cpu_unbind_core_context(c, &ctx);
}

/* Bramble's dual_core_step(), with the core stepped by m33_step_core() */
static void m33_dual_core_step(void) {
  static int current = 0;
  for (int i = 0; i < num_active_cores; i++) {
    int c = current % num_active_cores;
    current = (current + 1) % num_active_cores;
    if (cores[c].is_wfi) {
      systick_tick_for_core(c, 1);
      if (c == CORE0 && (num_active_cores == 1 || cores[CORE1].is_wfi || cores[CORE1].is_halted)) {
        timing_config.cycle_accumulator += 1;
        uint32_t us = timing_config.cycle_accumulator / timing_config.cycles_per_us;
        if (us > 0) {
          timing_config.cycle_accumulator -= us * timing_config.cycles_per_us;
          timer_tick(us);
          rtc_tick(us);
          if (membus_rp2350_periph) rp2350_timer1_tick((rp2350_periph_state_t *)membus_rp2350_periph, us);
        }
      }
      int saved_core = get_active_core();
      set_active_core(c);
      uint32_t pending = nvic_get_pending_irq();
      int wake = (pending != 0xFFFFFFFF) || systick_states[c].pending || nvic_states[c].pendsv_pending || cores[c].event_pending;
      set_active_core(saved_core);
      if (!wake) continue;
      cores[c].is_wfi = 0;
      if (cores[c].is_wfe) {
        cores[c].event_pending = 0;
        cores[c].is_wfe = 0;
      }
    }
    m33_step_core(c);
  }
}

static int all_asleep(void) {
  for (int c = 0; c < num_active_cores; c++)
    if (!cores[c].is_wfi && !cores[c].is_halted) return 0;
  return 1;
}

/* how many cycles until something can wake a sleeping core; 0 when it
   cannot be told, and time goes one cycle a step as Bramble has it */
static sim_cycles_t quiet_cycles(void) {
  uint32_t cpus = timing_config.cycles_per_us;
  sim_cycles_t now = bramble_now();
  sim_cycles_t limit = (sim_cycles_t)1 << 31;

  /* wake sources whose time is not known here */
  for (int b = 0; b < PIO_NUM_BLOCKS; b++)
    if (pio_state[b].ctrl & 0xF) return 0;
  if (rtc_state.inte || usb_state.dpram_touched || usb_state.sie_ctrl_written) return 0;
  if (pty_in_len) return 0;
  if (membus_rp2350_mode) return 0; /* TIMER1 and the RP2350 timers: not followed yet */

  if (next_due != UINT64_MAX) {
    if (next_due <= now) return 0;
    if (next_due - now < limit) limit = next_due - now;
  }
  if (opt_run_ms > 0) {
    sim_cycles_t end = sim_ms_to_cycles(opt_run_ms);
    if (end <= now) return 0;
    if (end - now < limit) limit = end - now;
  }
  /* the alarms of the timer */
  uint32_t low = (uint32_t)timer_state.time_us;
  for (int i = 0; i < 4; i++) {
    if (!(timer_state.armed & (1u << i))) continue;
    uint32_t us = timer_state.alarm[i] - low;
    if (us == 0 || us > 0x7fffffffu) return 0;
    sim_cycles_t c = (sim_cycles_t)us * cpus - timing_config.cycle_accumulator;
    if (c < limit) limit = c;
  }
  /* SysTick, of each core */
  for (int c = 0; c < num_active_cores; c++) {
    systick_state_t *st = &systick_states[c];
    if ((st->csr & 3) == 3) {
      if (st->cvr == 0) return 0;
      if (st->cvr < limit) limit = st->cvr;
    }
  }
  /* the watchdog, when it counts */
  return limit > 2 ? limit - 1 : 0;
}

/* time passes for sleeping cores, in one go */
static void jump(sim_cycles_t cycles) {
  uint32_t cpus = timing_config.cycles_per_us;
  uint64_t acc = timing_config.cycle_accumulator + cycles;
  uint32_t us = (uint32_t)(acc / cpus);
  timing_config.cycle_accumulator = (uint32_t)(acc % cpus);
  for (int c = 0; c < num_active_cores; c++) systick_tick_for_core(c, (uint32_t)cycles);
  if (us) {
    timer_tick(us);
    rtc_tick(us);
  }
}

static void reboot(void) {
  base_cycles += local_cycles();
  watchdog_reboot_pending = 0;
  clocks_state.wdog_ctrl &= ~(1u << 31);
  reset_runtime_peripherals(1);
  dual_core_init();
  cpu_reset_core(CORE0);
  core1_in_bootrom();
  timing_config.cycle_accumulator = 0;
  printf("[RST] watchdog reset at %.3f ms\n", sim_now_ms());
  for (int i = 0; i < n_reset_hooks; i++) reset_hooks[i]();
  last_levels = pin_levels() & watched_mask;
}

static int bramble_step(long steps, sim_cycles_t *asleep) {
  int sleeping = all_asleep();
  sim_cycles_t before = bramble_now();
  uint64_t us_before = timer_state.time_us;

  if (sleeping && opt_warp) {
    sim_cycles_t quiet = quiet_cycles();
    if (quiet) jump(quiet);
  }

  if (is_m33) m33_dual_core_step();
  else dual_core_step();
  pio_step();
  usb_step();

  /* Bramble counts the watchdog only on its RISC-V path (BR7). The counter
     of an RP2040 goes down by two each microsecond (erratum RP2040-E1), and
     the Pico SDK loads it with twice the time for that (BR11). */
  if (timer_state.time_us > us_before)
    clocks_watchdog_tick((uint32_t)(timer_state.time_us - us_before) * (is_m33 ? 1 : 2));

  *asleep = sleeping ? bramble_now() - before : 0;

  if ((steps & 0x3FF) == 0 && pty_uart >= 0) poll_pty();
  if (watched_mask) check_watches();
  run_timers();

  if (watchdog_reboot_pending) reboot();

  if (!any_core_running() || semihost_exit_requested) {
    printf("[DBG] CPU DONE at PC=0x%08x after %ld steps\n", cores[CORE0].r[15], steps);
    return SIM_DONE;
  }
  return sleeping ? SIM_SLEEPING : SIM_RUNNING;
}

static void bramble_inspect(void) {}
static void bramble_at_max_steps(void) {
  printf("[DBG] time %.3f ms\n", sim_now_ms());
  for (int c = 0; c < NUM_CORES; c++)
    printf("[DBG] core %d PC=0x%08x SP=0x%08x %s\n", c, cores[c].r[15], cores[c].r[13],
           cores[c].is_halted ? "halted" : cores[c].is_wfi ? "asleep" : "running");
}

static void bramble_report(void) {
  /* a pin whose function is not SIO is not seen on it here */
  for (int i = 0; i < n_watches; i++) {
    uint32_t func = gpio_state.pins[watches[i].bit].ctrl & 0x1F;
    if (func != GPIO_FUNC_SIO && func != GPIO_FUNC_NULL)
      printf("[PIN] GP%d is driven by function %u: Bramble does not put it on the pin, only SIO is seen\n", watches[i].bit, func);
  }
}

static void bramble_stop(int hl) {
  if (!headless) pty_line_close();
}

const sim_backend_t sim_backend_bramble = {
    .name = "bramble",
    .chips = "RP2040 and RP2350 (Arm cores), from .uf2 or .elf",
    .probe = bramble_probe,
    .arg = bramble_arg,
    .supports = bramble_supports,
    .start = bramble_start,
    .prepare = bramble_prepare,
    .bridge = bramble_bridge,
    .launch = bramble_launch,
    .step = bramble_step,
    .inspect = bramble_inspect,
    .at_max_steps = bramble_at_max_steps,
    .report = bramble_report,
    .stop = bramble_stop,
    .hz = bramble_hz,
    .now = bramble_now,
    .timer = bramble_timer,
    .on_reset = bramble_on_reset,
    .pin = bramble_pin,
    .has_pin = bramble_has_pin,
    .pin_name = bramble_pin_name,
    .drive = bramble_drive,
    .pull = bramble_pull,
    .watch = bramble_watch,
    .analog = bramble_analog,
    .uart_send = bramble_uart_send,
    .uart_listen = bramble_uart_listen,
};

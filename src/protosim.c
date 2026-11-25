/*
    protosim.c
    Runs firmware for Protoduino on an emulator, with a PTY or files for
    its UART: simavr for AVR parts, Bramble for the RP2040 and RP2350.
    This file is the driver: the options all targets share, the loop and
    the shared reports. What a target needs is in sim.h.
 */

#ifdef _WIN32
#define PROTOSIM_BUILD 1
#endif

#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "stimulus.h"

/* ═══════════════════════════════════════════════════════════════
   Shared options
   ═══════════════════════════════════════════════════════════════ */

const sim_backend_t *sim = NULL;

int verbose = 0;
long opt_max_steps = 0;
char *opt_uart0_in = NULL;
char *opt_uart0_out = NULL;
char *opt_exit_on_uart = NULL;
int opt_wait_tcp = 0;
int opt_warp = 0;      /* a sleeping part costs no real time */
double opt_run_ms = 0; /* stop after this much simulated time; 0: run on */

/* SIGTERM ends the run like the end of --run-ms: reports are still written */
static volatile sig_atomic_t stop_requested = 0;
static void on_stop_signal(int sig) { stop_requested = 1; }

static int force_exit_success = 0;
static FILE *uart0_out_file = NULL;
static char exit_uart_buf[512] = {0};
static int exit_uart_len = 0;

static void uart0_out_hook(uint8_t value) {
  if (uart0_out_file) {
    fputc(value, uart0_out_file);
  }
  if (opt_exit_on_uart) {
    if (exit_uart_len < (int)sizeof(exit_uart_buf) - 1) {
      exit_uart_buf[exit_uart_len++] = (char)value;
      exit_uart_buf[exit_uart_len] = '\0';
    } else {
      memmove(exit_uart_buf, exit_uart_buf + 1, exit_uart_len - 1);
      exit_uart_buf[exit_uart_len - 1] = (char)value;
    }
    if (strstr(exit_uart_buf, opt_exit_on_uart) != NULL) {
      force_exit_success = 1;
    }
  }
}

/* one byte a millisecond, about 9600 baud; a byte the chip has no room for
   is offered again a millisecond later */
static sim_cycles_t uart0_in_timer(sim_cycles_t when, void *param) {
  static int injected_idx = 0;
  if (!opt_uart0_in || opt_uart0_in[injected_idx] == '\0') return 0;
  if (sim->uart_send(0, (uint8_t)opt_uart0_in[injected_idx])) injected_idx++;
  return when + sim->hz() / 1000;
}

/* a reset drops every pending timer on some targets; the injection goes on after it */
static void uart0_in_rearm(void) {
  if (opt_uart0_in) sim->timer(sim->hz() / 1000, uart0_in_timer, NULL);
}

/* ═══════════════════════════════════════════════════════════════
   Choosing the target
   ═══════════════════════════════════════════════════════════════ */

static const sim_backend_t *const backends[] = {
    &sim_backend_avr,
#ifdef PROTOSIM_BRAMBLE
    &sim_backend_bramble,
#endif
};
#define N_BACKENDS (sizeof(backends) / sizeof(backends[0]))

/* --target names a backend, or a chip family it runs */
static const sim_backend_t *backend_named(const char *name) {
  if (!strcmp(name, "avr") || !strcmp(name, "simavr")) return &sim_backend_avr;
#ifdef PROTOSIM_BRAMBLE
  if (!strcmp(name, "bramble") || !strncmp(name, "rp2040", 6) || !strncmp(name, "rp2350", 6)) return &sim_backend_bramble;
#endif
  return NULL;
}

static void usage(const char *self) {
  fprintf(stderr,
          "Usage: %s <firmware.elf|hex|uf2> [options]\n"
          "\nTargets (chosen by the file; --target to choose yourself):\n",
          self);
  for (unsigned i = 0; i < N_BACKENDS; i++) fprintf(stderr, "  %-22s %s\n", backends[i]->name, backends[i]->chips);
#ifndef PROTOSIM_BRAMBLE
  fprintf(stderr, "  %-22s RP2040 and RP2350: not in this build (make BRAMBLE=1)\n", "bramble");
#endif
  fprintf(
      stderr,
      "  --target <name>        avr | rp2040 | rp2350 (or the backend: simavr | bramble)\n"
      "\nCore options:\n"
      "  -m <mcu>               MCU type (default: atmega328p; on Bramble: rp2040 or rp2350, by the file)\n"
      "  -f <freq>              CPU frequency Hz (default: 16000000; on Bramble: 125 MHz, 150 MHz on the RP2350)\n"
      "  -d                     Enable GDB server on port 1234\n"
      "  -v                     Verbose simavr output\n"
      "  --core-skew <n>:<m>    RP2040/RP2350: core 0 takes n steps, then core 1 m (default 1:1)\n"
      "  --bootloader <file>    Load a bootloader .hex (or .elf) into flash\n"
      "                         and start execution at its entry point.\n"
      "\nReset:\n"
      "  --reset-cause <cause>  Cause the first start reports in MCUSR (default: power)\n"
      "  --reset-at <ms>[:cause] Reset at that simulated time; repeatable (default cause: pin)\n"
      "                         cause: power | pin | brownout | watchdog\n"
      "  --reset-vector <addr>  Where a reset leads, e.g. 0x7800 for a boot section\n"
      "                         (default: 0, or the entry of --bootloader)\n"
      "  SIGUSR1 / SIGUSR2      Pull the reset pin / cycle the power of a running simulation\n"
      "\nPins and analog inputs (times are simulated ms; a pin is a port and a bit, B3, or GP25):\n"
      "  --soft-uart <rx>,<tx>[,<baud>[,<gap>]]\n"
      "                         A serial line on two pins, on the PTY in place of UART0, for parts\n"
      "                         without a USART: B0,B1,9600. <rx> is where the part receives.\n"
      "                         <gap>: idle bit times between two bytes to the part (default 20)\n"
      "  --pin-set <pin>:<0|1>[@<ms>]   Level of an input pin, from the start or from a time\n"
      "  --pin-wave <pin>:<period>[:<from>[:<to>]]\n"
      "                         Square wave into a pin, e.g. a heartbeat: B2:100:0:5000\n"
      "  --pin-pull <pin>:<0|1> External pull on a pin: what an open-drain output reads when released\n"
      "  --pin-log <pin>        Print every change of a pin\n"
      "  --pin-duty <pin>[:<window>]    Print the share of time a pin is high, per window (default 100 ms)\n"
      "  --adc <ch|temp>:<mV>[@<ms>]    Voltage at an ADC input, from the start or from a time\n"
      "  --vcc <mV>[@<ms>]      Supply voltage, as the ADC sees it\n"
      "  --warp                 A sleeping part, or one that polls its UART, costs no real time\n"
      "  --run-ms <ms>          Stop after this much simulated time\n"
      "  --dump-eeprom <addr:len>       Print that part of EEPROM when the run ends\n"
      "\nLLM Debug tools:\n"
      "  -b <addr|symbol>       Breakpoint at hex addr or ELF symbol\n"
      "  -w <addr:sz:name>      Watch SRAM variable at breakpoints\n"
      "  --dump-regs            Dump all 32 CPU registers at each breakpoint\n"
      "  --dump-sram <addr:len> Hex+ASCII SRAM dump at each breakpoint\n"
      "  --max-steps <n>        Exit after N steps (prevents hangs)\n"
      "  -t <n>                 Trace: print PC + watches every N cycles\n"
      "  -s                     Single-step: print PC for every instruction\n"
      "\nLLM Testing & CI tools:\n"
      "  --uart0-in <string>    Inject literal string to UART0 RX\n"
      "  --uart0-out <file>     Capture UART0 TX to file (bypasses TCP)\n"
      "  --exit-on-uart <str>   Exit successfully when exact string is printed\n"
      "  --wait-tcp             Wait for TCP client before starting simulation\n"
      "\nLLM Profiling tools (ELF only):\n"
      "  --coverage             Per-function code coverage report\n"
      "  --profile              Cycle-exact flat performance profile\n"
      "  --callgraph            Call graph with per-edge cycle costs\n"
      "  --profile-out <file>   Write profiling reports to file\n"
      "\nOn the RP2040 and RP2350 the reset, bootloader, EEPROM, soft line, supply, profiling\n"
      "and breakpoint options are not served; protosim says so and stops.\n");
}

static const sim_backend_t *choose_backend(int argc, char *argv[]) {
  for (int i = 2; i + 1 < argc; i++) {
    if (!strcmp(argv[i], "--target")) {
      const sim_backend_t *b = backend_named(argv[i + 1]);
      if (!b) {
        fprintf(stderr, "ERROR: --target %s: no such target in this build\n", argv[i + 1]);
        exit(1);
      }
      return b;
    }
  }
  for (unsigned i = 0; i < N_BACKENDS; i++)
    if (backends[i]->probe(argv[1])) return backends[i];
  /* what it was before targets: an AVR */
  return &sim_backend_avr;
}

/* a shared option the target does not serve ends the run before it starts */
static void need_support(const char *option) {
  if (!sim->supports(option)) {
    fprintf(stderr, "ERROR: %s is not served on %s\n", option, sim->name);
    exit(1);
  }
}

static void parse_options(int argc, char *argv[]) {
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--target")) {
      i++;
    } else if (!strcmp(argv[i], "-v")) {
      verbose++;
    } else if (!strcmp(argv[i], "--max-steps")) {
      if (i + 1 < argc) opt_max_steps = atol(argv[++i]);
    } else if (!strcmp(argv[i], "--uart0-in")) {
      if (i + 1 < argc) opt_uart0_in = argv[++i];
    } else if (!strcmp(argv[i], "--uart0-out")) {
      if (i + 1 < argc) opt_uart0_out = argv[++i];
    } else if (!strcmp(argv[i], "--exit-on-uart")) {
      if (i + 1 < argc) opt_exit_on_uart = argv[++i];
    } else if (!strcmp(argv[i], "--wait-tcp")) {
      need_support(argv[i]);
      opt_wait_tcp = 1;
    } else if (!strcmp(argv[i], "--warp")) {
      opt_warp = 1;
    } else if (!strcmp(argv[i], "--run-ms")) {
      if (i + 1 < argc) opt_run_ms = strtod(argv[++i], NULL);
    } else if (!strcmp(argv[i], "--soft-uart") || !strncmp(argv[i], "--pin-", 6) || !strcmp(argv[i], "--adc") ||
               !strcmp(argv[i], "--vcc")) {
      const char *name = argv[i];
      const char *value = i + 1 < argc ? argv[++i] : "";
      need_support(name);
      int r = !strcmp(name, "--soft-uart")  ? stimulus_soft_uart_arg(value)
              : !strcmp(name, "--pin-set")  ? stimulus_pin_set_arg(value)
              : !strcmp(name, "--pin-wave") ? stimulus_pin_wave_arg(value)
              : !strcmp(name, "--pin-pull") ? stimulus_pin_pull_arg(value)
              : !strcmp(name, "--pin-log")  ? stimulus_pin_log_arg(value)
              : !strcmp(name, "--pin-duty") ? stimulus_pin_duty_arg(value)
              : !strcmp(name, "--adc")      ? stimulus_adc_arg(value)
              : !strcmp(name, "--vcc")      ? stimulus_vcc_arg(value)
                                            : -1;
      if (r < 0) {
        fprintf(stderr, "ERROR: %s '%s' is not understood\n", name, value);
        exit(1);
      }
    } else {
      int r = sim->arg(&i, argc, argv);
      if (r < 0) exit(1);
      /* an option of another target */
      if (r == 0 && argv[i][0] == '-' && sim != &sim_backend_avr) {
        fprintf(stderr, "ERROR: %s is not served on %s\n", argv[i], sim->name);
        exit(1);
      }
    }
  }
}

/* ═══════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  if (argc < 2) {
    usage(argv[0]);
    exit(1);
  }

  sim = choose_backend(argc, argv);
  parse_options(argc, argv);

  sim->start(argv[1]);
  sim->on_reset(uart0_in_rearm);
  signal(SIGTERM, on_stop_signal);
  stimulus_setup();
  sim->prepare();

  /* ── UART: files, or the PTY ─────────────────────────────── */
  int headless_uart = (opt_uart0_in || opt_uart0_out || opt_exit_on_uart);
  if (headless_uart) {
    printf("[UART] Headless mode enabled. Bypassing TCP/PTY.\n");
    if (opt_uart0_out) {
      uart0_out_file = fopen(opt_uart0_out, "wb");
      if (!uart0_out_file) {
        fprintf(stderr, "Failed to open --uart0-out %s\n", opt_uart0_out);
      }
    }
    sim->uart_listen(0, uart0_out_hook);
    if (opt_uart0_in) sim->timer(sim->hz() / 1000, uart0_in_timer, NULL);
  } else {
    sim->bridge();
  }
  sim->launch();

  /* ══════════════════════════════════════════════════════════
     Main simulation loop
     ══════════════════════════════════════════════════════════ */
  long steps = 0;
  uint64_t asleep_cycles = 0;
  uint64_t run_cycles = (uint64_t)(opt_run_ms * sim->hz() / 1000.0);

  while (1) {
    sim_cycles_t asleep = 0;
    int state = sim->step(steps, &asleep);
    asleep_cycles += asleep;

    if (state == SIM_DONE || state == SIM_CRASHED) break;

    steps++;

    if (force_exit_success) {
      printf("[DBG] SUCCESS: Match '--exit-on-uart %s' found. Exiting correctly.\n", opt_exit_on_uart);
      break;
    }

    if (stop_requested || (run_cycles && sim->now() >= run_cycles)) {
      printf("[DBG] stopped at %.3f ms\n", sim_now_ms());
      break;
    }

    if (opt_max_steps && steps >= opt_max_steps) {
      printf("[DBG] MAX-STEPS (%ld) reached — stopping.\n", opt_max_steps);
      sim->at_max_steps();
      if (opt_exit_on_uart) {
        fprintf(stderr, "[ERR] --exit-on-uart condition not met within max-steps.\n");
        exit(1);
      }
      break;
    }

    sim->inspect();
  }

  sim->report();

  if (asleep_cycles) {
    uint64_t total = sim->now();
    printf("[SLEEP] asleep %.2f%% of %.3f ms, awake %.3f ms\n", 100.0 * (double)asleep_cycles / (double)total,
           (double)total * 1000.0 / sim->hz(), (double)(total - asleep_cycles) * 1000.0 / sim->hz());
  }

  if (uart0_out_file) fclose(uart0_out_file);
  sim->stop(headless_uart);

  return 0;
}

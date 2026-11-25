/*
    backend_avr.c
    The simavr backend: AVR parts, from ELF or HEX.
 */

#ifdef _WIN32
#define PROTOSIM_BUILD 1
#endif

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_gdb.h"
#include "sim_hex.h"
#include "avr_uart.h"
#include "avr_eeprom.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "sim_time.h"

#ifdef _WIN32
#include "uart_com.h"
#else
#include "uart_pty.h"
#endif

#include "sim.h"
#include "stk500_uploader.h"
#include "protosim_core.h"
#include "debug.h"
#include "profiling.h"
#include "ihex_loader.h"
#include "reset.h"
#include "stimulus.h"

/* ═══════════════════════════════════════════════════════════════
   Options of this backend
   ═══════════════════════════════════════════════════════════════ */

avr_t *avr = NULL;
int opt_dump_regs = 0;
int opt_single_step = 0;
long opt_trace_every = 0;

static char *mmcu = "atmega328p";
static uint32_t freq = 16000000;
static int debug_gdb = 0;
static const char *opt_bootloader = NULL;
static int opt_reset_cause = RESET_POWER_ON; /* what the first start reports */
static long opt_reset_vector = -1;           /* where a reset leads; -1: address 0, or the bootloader */
static const char *opt_dump_eeprom = NULL;   /* <addr>:<len>, printed when the run ends */

static int is_hex;
static elf_firmware_t f = {0};
static uint8_t *stk_app_binary = NULL;
static size_t stk_app_size = 0;
static uint64_t last_trace_cyc = 0;

#ifdef _WIN32
uart_com_t uart_com;
#else
uart_pty_t uart_pty;
#endif

static int avr_probe(const char *path) {
  if (strstr(path, ".hex") != NULL) return 1;
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  unsigned char h[20] = {0};
  size_t n = fread(h, 1, sizeof(h), fp);
  fclose(fp);
  /* an ELF file for the AVR: e_machine 83 */
  return n == sizeof(h) && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[18] == 83 && h[19] == 0;
}

static int avr_arg(int *ip, int argc, char **argv) {
  int i = *ip;
  const char *a = argv[i];
  int has = i + 1 < argc;
  if (!strcmp(a, "-m")) {
    if (has) mmcu = argv[++i];
  } else if (!strcmp(a, "-f")) {
    if (has) freq = (uint32_t)atoi(argv[++i]);
  } else if (!strcmp(a, "-d")) {
    debug_gdb++;
  } else if (!strcmp(a, "-b")) {
    if (has) add_breakpoint_arg(argv[++i]);
  } else if (!strcmp(a, "-w")) {
    if (has) add_watch_arg(argv[++i]);
  } else if (!strcmp(a, "--dump-regs")) {
    opt_dump_regs = 1;
  } else if (!strcmp(a, "--dump-sram")) {
    if (has) add_sram_dump_arg(argv[++i]);
  } else if (!strcmp(a, "-t")) {
    if (has) opt_trace_every = atol(argv[++i]);
  } else if (!strcmp(a, "-s")) {
    opt_single_step = 1;
  } else if (!strcmp(a, "--coverage")) {
    opt_coverage = 1;
  } else if (!strcmp(a, "--profile")) {
    opt_profile = 1;
    opt_callgraph = 1;
  } else if (!strcmp(a, "--callgraph")) {
    opt_callgraph = 1;
    opt_profile = 1;
  } else if (!strcmp(a, "--profile-out")) {
    if (has) opt_profile_out = argv[++i];
  } else if (!strcmp(a, "--bootloader")) {
    if (has) opt_bootloader = argv[++i];
  } else if (!strcmp(a, "--reset-cause")) {
    if (has && (opt_reset_cause = reset_cause_from_name(argv[++i])) < 0) {
      fprintf(stderr, "ERROR: --reset-cause %s: use power, pin, brownout or watchdog\n", argv[i]);
      exit(1);
    }
  } else if (!strcmp(a, "--reset-at")) {
    if (has && add_reset_arg(argv[++i]) < 0) {
      fprintf(stderr, "ERROR: --reset-at %s: use <ms> or <ms>:<cause>\n", argv[i]);
      exit(1);
    }
  } else if (!strcmp(a, "--reset-vector")) {
    if (has) opt_reset_vector = strtol(argv[++i], NULL, 0);
  } else if (!strcmp(a, "--dump-eeprom")) {
    if (has) opt_dump_eeprom = argv[++i];
  } else {
    return 0;
  }
  *ip = i;
  return 1;
}

static int avr_supports(const char *option) { return 1; }

/* ═══════════════════════════════════════════════════════════════
   Loading
   ═══════════════════════════════════════════════════════════════ */

/* simavr's ELF loader takes the last flash address as its limit, so a file
   that fills flash to its last byte arrives one byte short ("would overflow
   buffer"). A bootloader keeps its version or a service table there. The
   loader frees the chunks, so the byte is picked up before it runs. */
static void load_firmware_whole(avr_t *a, elf_firmware_t *fw) {
  int last = -1;
  for (fw_chunk_t *c = fw->chunks; c; c = c->next)
    if (c->addr < AVR_SEGMENT_OFFSET_DATA && c->size && c->addr + c->size == a->flashend + 1)
      last = c->data[c->size - 1];
  avr_load_firmware(a, fw);
  if (last >= 0) a->flash[a->flashend] = (uint8_t)last;
}

/* On a chip the flag of an external or pin change interrupt is cleared by
   writing a one to it. simavr keeps these flag registers as plain memory,
   so that the same write sets the flag for good: firmware that polls
   INTF0 without an interrupt handler sees an edge on every look. Where
   the register is shared with the pin change flag, as GIFR is on the
   ATtiny, the write is dropped and the flag cannot be cleared at all. This
   gives the flag bits of those registers their behaviour. */
static void interrupt_flag_write(struct avr_t *a, avr_io_addr_t addr, uint8_t v, void *param) {
  uint8_t flags = (uint8_t)(uintptr_t)param;
  for (int i = 0; i < a->interrupts.vector_count; i++) {
    avr_int_vector_t *vector = a->interrupts.vector[i];
    if (vector->raised.reg == addr && (v & (vector->raised.mask << vector->raised.bit))) avr_clear_interrupt(a, vector);
  }
  a->data[addr] = (uint8_t)((a->data[addr] & flags) | (v & ~flags));
}

static void fix_interrupt_flags(avr_t *a) {
  static const avr_io_addr_t registers[] = {0x3B, 0x3C, 0x5A}; /* PCIFR, EIFR; GIFR */
  for (unsigned r = 0; r < sizeof(registers) / sizeof(registers[0]); r++) {
    uint8_t flags = 0;
    for (int i = 0; i < a->interrupts.vector_count; i++) {
      avr_int_vector_t *vector = a->interrupts.vector[i];
      if (vector->raised.reg == registers[r]) flags |= (uint8_t)(vector->raised.mask << vector->raised.bit);
    }
    /* simavr's own handlers there, where it has any, clear the pin change
       flags and nothing else. This one does that too, so it takes their
       place: a register has room for four, and a part with three ports of
       pin change interrupts has none left. */
    if (flags) {
      a->io[AVR_DATA_TO_IO(registers[r])].w.c = interrupt_flag_write;
      a->io[AVR_DATA_TO_IO(registers[r])].w.param = (void *)(uintptr_t)flags;
    }
  }
}

static void avr_start(const char *filename) {
  /* ── Load firmware ───────────────────────────────────────── */
  is_hex = (strstr(filename, ".hex") != NULL);
  uint8_t *hex_data = NULL;
  uint32_t hex_size = 0, hex_base = 0;

  if (is_hex) {
    hex_data = read_ihex_file(filename, &hex_size, &hex_base);
    if (!hex_data) {
      fprintf(stderr, "Cannot load HEX %s\n", filename);
      exit(1);
    }
    printf("Loaded HEX: %u bytes at 0x%04x\n", hex_size, hex_base);
  } else {
    if (elf_read_firmware(filename, &f) < 0) {
      fprintf(stderr, "Cannot load ELF %s\n", filename);
      exit(1);
    }
    if (f.mmcu[0]) mmcu = f.mmcu;
    if (f.frequency) freq = f.frequency;
  }

  /* ── Resolve symbol breakpoints ──────────────────────────── */
  for (int i = 0; i < n_breakpoints; i++) {
    if (breakpoints[i].addr != UINT32_MAX) continue;
    if (is_hex) {
      fprintf(stderr, "WARN: symbol breakpoints need ELF, skipping '%s'\n", breakpoints[i].label);
      breakpoints[i] = breakpoints[--n_breakpoints];
      i--;
    } else {
      uint32_t a = resolve_symbol(&f, breakpoints[i].label);
      if (a == UINT32_MAX) {
        fprintf(stderr, "WARN: symbol '%s' not found — skipping\n", breakpoints[i].label);
        breakpoints[i] = breakpoints[--n_breakpoints];
        i--;
      } else {
        breakpoints[i].addr = a;
        printf("[DBG] Breakpoint '%s' → PC=0x%04x\n", breakpoints[i].label, a);
      }
    }
  }

  /* ── Create and initialise AVR ───────────────────────────── */
  avr = avr_make_mcu_by_name(mmcu);
  if (!avr) {
    fprintf(stderr, "Cannot create AVR core for %s\n", mmcu);
    exit(1);
  }
  avr_init(avr);
  avr->frequency = freq;
  fix_interrupt_flags(avr);

  if (is_hex) {
    memcpy(avr->flash + hex_base, hex_data, hex_size);
    free(hex_data);
    avr->pc = hex_base;
    avr->codeend = hex_base + hex_size;
  } else {
    load_firmware_whole(avr, &f);
    if (avr->codeend == 0 || avr->codeend == avr->flashend) {
      uint32_t old_codeend = avr->codeend;
      avr->codeend = 0;
      for (int i = 0; i < (int)f.symbolcount; i++) {
        uint32_t addr = f.symbol[i]->addr;
        uint32_t end = addr + f.symbol[i]->size;
        if (addr < avr->flashend && end > avr->codeend && end <= avr->flashend + 1) {
          if (strcmp(f.symbol[i]->symbol, "__stack") == 0) continue;
          if (addr == 0 && f.symbol[i]->size == 0) continue;
          avr->codeend = end;
        }
      }
      if (avr->codeend == 0) avr->codeend = old_codeend;
    }
    if (avr->codeend == 0) avr->codeend = avr->flashend;
  }

  /* ── Snapshot the app binary BEFORE bootloader overlay ─────── */
  if (opt_bootloader) {
    stk_app_size = (avr->codeend > 0) ? avr->codeend : (avr->flashend + 1);
    stk_app_binary = (uint8_t *)malloc(stk_app_size);
    if (!stk_app_binary) {
      perror("malloc stk_app_binary");
      exit(1);
    }
    memcpy(stk_app_binary, avr->flash, stk_app_size);
    printf("[STK] Captured app binary: %zu bytes from flash\n", stk_app_size);
  }

  /* ── Bootloader overlay ──────────────────────────────────── */
  if (opt_bootloader) {
    int bl_is_hex = (strstr(opt_bootloader, ".hex") != NULL || strstr(opt_bootloader, ".ihex") != NULL);
    uint32_t bl_size = 0, bl_base = 0;
    uint8_t *bl_data = NULL;
    uint32_t bl_entry = 0;

    if (bl_is_hex) {
      if (protosim_read_ihex(opt_bootloader, &bl_data, &bl_size, &bl_base) < 0 || !bl_data) {
        fprintf(stderr, "ERROR: Cannot load bootloader HEX: %s\n", opt_bootloader);
        exit(1);
      }
      if (bl_base + bl_size > avr->flashend + 1) {
        fprintf(stderr, "ERROR: Bootloader HEX exceeds flash\n");
        free(bl_data);
        exit(1);
      }
      memcpy(avr->flash + bl_base, bl_data, bl_size);
      free(bl_data);
      bl_entry = bl_base;
      printf("[BL] Loaded bootloader HEX: %u bytes at 0x%04x\n", bl_size, bl_base);
    } else {
      elf_firmware_t bl_fw = {0};
      if (elf_read_firmware(opt_bootloader, &bl_fw) < 0) {
        fprintf(stderr, "ERROR: Cannot load bootloader ELF: %s\n", opt_bootloader);
        exit(1);
      }
      avr_t *tmp = avr_make_mcu_by_name(mmcu);
      avr_init(tmp);
      load_firmware_whole(tmp, &bl_fw);
      /* simavr does not set the program counter when it loads a file: the
         bootloader begins at the lowest address it occupies */
      while (bl_entry <= tmp->flashend && tmp->flash[bl_entry] == 0xff) bl_entry++;
      if (bl_entry > tmp->flashend) bl_entry = 0;
      memcpy(avr->flash, tmp->flash, tmp->flashend + 1);
      printf("[BL] Loaded bootloader ELF: %s  entry=0x%04x\n", opt_bootloader, bl_entry);
    }
    avr->pc = bl_entry;
    avr->reset_pc = bl_entry; /* BOOTRST: a reset enters the bootloader again */
    printf("[BL] Initial PC set to bootloader entry 0x%04x\n", bl_entry);
    if ((bl_entry + bl_size) > avr->codeend) avr->codeend = bl_entry + bl_size;
  }

  /* ── Reset vector and reset cause ────────────────────────── */
  if (opt_reset_vector >= 0) {
    if ((uint32_t)opt_reset_vector > avr->flashend) {
      fprintf(stderr, "ERROR: --reset-vector 0x%lx is outside flash\n", opt_reset_vector);
      exit(1);
    }
    avr->reset_pc = (avr_flashaddr_t)opt_reset_vector;
    avr->pc = avr->reset_pc;
    printf("[RST] Reset vector 0x%04x\n", avr->reset_pc);
  }
  reset_setup(avr, opt_reset_cause);
  reset_install_signals();
  avr->log = 1 + verbose;
}

/* ═══════════════════════════════════════════════════════════════
   Before the run
   ═══════════════════════════════════════════════════════════════ */

static void sleep_at_once(avr_t *a, avr_cycle_count_t how_long) {}

static void avr_prepare(void) {
  if (debug_gdb) {
    avr->gdb_port = 1234;
    avr->state = cpu_Stopped;
    avr_gdb_init(avr);
    printf("GDB server listening on port 1234\n");
  } else if (opt_warp) {
    /* a sleeping part moves straight to its next event */
    avr->sleep = sleep_at_once;
    /* simavr sleeps a little each time firmware reads a UART status
       register while nothing is pending, so that a polling loop does not
       hog the host: firmware that sends with polled writes then runs about
       80 times slower than real time. Under --warp nobody waits for real
       time. */
    for (char u = '0'; u <= '3'; u++) {
      uint32_t flags = 0;
      if (avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS(u), &flags) < 0) continue;
      flags &= ~AVR_UART_FLAG_POLL_SLEEP;
      avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(u), &flags);
    }
  }

  /* ── Build profiling tables ───────────────────────────────── */
  if (opt_coverage || opt_profile || opt_callgraph) {
    if (!is_hex) build_func_table(&f, avr->flashend);
    profiling_alloc(avr->flashend);
  }
}

#ifndef _WIN32
/* the soft line of stimulus.c, on the IRQs the PTY bridge connects to */
enum { SU_FROM_PART = 0, SU_TO_PART, SU_XON, SU_XOFF, SU_COUNT };
static const char *su_names[SU_COUNT] = {"8<soft.from", "8>soft.to", ">soft.xon", ">soft.xoff"};
static avr_irq_t *su_irq;

static void su_from_part(uint8_t byte) { avr_raise_irq(su_irq + SU_FROM_PART, byte); }
static void su_xon(void) { avr_raise_irq(su_irq + SU_XON, 1); }
static void su_xoff(void) { avr_raise_irq(su_irq + SU_XOFF, 1); }
static void su_to_part(struct avr_irq_t *irq, uint32_t value, void *param) { stimulus_soft_uart_put((uint8_t)value); }
#endif

static void avr_bridge(void) {
#ifdef _WIN32
  uart_com_init(avr, &uart_com);
  uart_com_connect(&uart_com, '0');
  if (opt_wait_tcp) {
    printf("[TCP] Waiting for client to connect to port %d...\n", uart_com.port.port);
    while (!uart_com_is_connected(&uart_com)) {
      Sleep(100);
    }
    printf("[TCP] Client connected. Starting simulation.\n");
  }
#else
  uart_pty_init(avr, &uart_pty);
  if (stimulus_has_soft_uart()) {
    /* the same bridge, on the line of two pins */
    static const stimulus_bridge_t bridge = {su_from_part, su_xon, su_xoff};
    su_irq = avr_alloc_irq(&avr->irq_pool, 0, SU_COUNT, su_names);
    avr_irq_register_notify(su_irq + SU_TO_PART, su_to_part, NULL);
    stimulus_soft_uart_bridge(&bridge);
    uart_pty_connect_irqs(&uart_pty, su_irq + SU_FROM_PART, su_irq + SU_TO_PART, su_irq + SU_XON, su_irq + SU_XOFF, '0');
    avr_raise_irq(su_irq + SU_XON, 1);
  } else {
    uart_pty_connect(&uart_pty, '0');
  }
  if (opt_wait_tcp) {
    printf("[TCP] Waiting for client to connect on PTY %s...\n", uart_pty.port[0].slavename);
    while (!uart_pty_is_connected(&uart_pty)) {
      usleep(100000);
    }
    printf("[TCP] Client connected. Starting simulation.\n");
  }
#endif
}

static void avr_launch(void) {
  /* ── STK500 auto-upload ────────── */
  if (opt_bootloader && stk_app_binary) {
    static stk500_args_t stk_args;
#ifdef _WIN32
    stk500_args_init(&stk_args, stk_app_binary, stk_app_size, uart_com.port.port);
#else
    stk500_args_init(&stk_args, stk_app_binary, stk_app_size, 0);
#endif
    free(stk_app_binary);
    stk500_start_upload_thread(&stk_args);
    printf("[STK] Auto-upload thread started (port %d)\n", stk_args.tcp_port);
  }
}

/* ═══════════════════════════════════════════════════════════════
   The run
   ═══════════════════════════════════════════════════════════════ */

static int avr_step(long steps, sim_cycles_t *asleep) {
  uint32_t pre_pc = avr->pc;
  uint64_t pre_cycle = avr->cycle;

  int state = avr_run(avr);

  /* a sleeping part moves straight to its next event */
  *asleep = (state == cpu_Sleeping && avr->cycle > pre_cycle) ? avr->cycle - pre_cycle : 0;

  if (opt_coverage || opt_profile || opt_callgraph) profiling_step(pre_pc, pre_cycle, avr->pc, avr->cycle);

  if (state == cpu_Done || state == cpu_Crashed) {
    printf("[DBG] CPU %s at PC=0x%04x after %ld steps\n", state == cpu_Done ? "DONE" : "CRASHED", avr->pc, steps);
    return state == cpu_Done ? SIM_DONE : SIM_CRASHED;
  }

  reset_after_step(avr);
  return state == cpu_Sleeping ? SIM_SLEEPING : SIM_RUNNING;
}

static void avr_inspect(void) {
  if (opt_single_step) {
    printf("[DBG] STEP  PC=0x%04x  cycle=%" PRIu64 "\n", avr->pc, avr->cycle);
    dump_watches(avr);
  }

  if (opt_trace_every && (avr->cycle - last_trace_cyc) >= (uint64_t)opt_trace_every) {
    last_trace_cyc = avr->cycle;
    printf("[DBG] TRACE PC=0x%04x  cycle=%" PRIu64 "\n", avr->pc, avr->cycle);
    if (opt_dump_regs) dump_registers(avr);
    dump_watches(avr);
  }

  for (int i = 0; i < n_breakpoints; i++) {
    if (avr->pc == breakpoints[i].addr) {
      breakpoints[i].hit_count++;
      on_breakpoint_hit(avr, breakpoints[i].label);
    }
  }
}

static void avr_at_max_steps(void) {
  if (opt_dump_regs) dump_registers(avr);
  dump_watches(avr);
}

static void avr_report(void) {
  /* ── Profiling reports ───────────────────────────────────── */
  if (opt_coverage || opt_profile || opt_callgraph) {
    finalize_call_stack(avr->cycle);
    FILE *pout = stdout;
    if (opt_profile_out) pout = fopen(opt_profile_out, "w");

    if (opt_coverage) report_coverage(pout, avr->cycle, freq);
    if (opt_profile) report_profile(pout, avr->cycle, freq);
    if (opt_callgraph) report_callgraph(pout, avr->cycle, freq);

    if (pout && pout != stdout) fclose(pout);
  }

  if (opt_dump_eeprom) {
    unsigned at = 0, len = 16;
    sscanf(opt_dump_eeprom, "%i:%i", (int *)&at, (int *)&len);
    avr_eeprom_desc_t d = {.ee = NULL, .offset = (uint16_t)at, .size = len};
    if (avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &d) >= 0 && d.ee) {
      printf("[EEPROM] 0x%04x:", at);
      for (unsigned i = 0; i < len; i++) printf(" %02x", d.ee[i]);
      printf("\n");
    } else {
      printf("[EEPROM] cannot read %u bytes at 0x%04x\n", len, at);
    }
  }
}

static void avr_stop(int headless) {
  /* Clean up sockets/PTY if used */
  if (!headless) {
#ifdef _WIN32
    uart_com_stop(&uart_com);
#else
    uart_pty_stop(&uart_pty);
#endif
  }
}

/* ═══════════════════════════════════════════════════════════════
   Primitives
   ═══════════════════════════════════════════════════════════════ */

static uint32_t avr_hz(void) { return avr->frequency; }

/* since the start of the run; avr->cycle starts again at each reset */
static sim_cycles_t avr_now(void) { return reset_total_cycles(avr); }

/* simavr knows a timer by its function and parameter: registering the same
   pair again moves it. One adapter per pair keeps that. */
typedef struct {
  sim_timer_fn fn;
  void *param;
} timer_adapter_t;

#define MAX_TIMER_ADAPTERS 128
static timer_adapter_t timer_adapters[MAX_TIMER_ADAPTERS];
static int n_timer_adapters;

static avr_cycle_count_t timer_call(struct avr_t *a, avr_cycle_count_t when, void *param) {
  timer_adapter_t *t = param;
  sim_cycles_t base = reset_total_cycles(a) - a->cycle;
  sim_cycles_t next = t->fn(base + when, t->param);
  return next ? (avr_cycle_count_t)(next - base) : 0;
}

static void avr_timer(sim_cycles_t in, sim_timer_fn fn, void *param) {
  timer_adapter_t *t = NULL;
  for (int i = 0; i < n_timer_adapters && !t; i++)
    if (timer_adapters[i].fn == fn && timer_adapters[i].param == param) t = &timer_adapters[i];
  if (!t) {
    if (n_timer_adapters == MAX_TIMER_ADAPTERS) {
      fprintf(stderr, "ERROR: too many timers\n");
      exit(1);
    }
    t = &timer_adapters[n_timer_adapters++];
    t->fn = fn;
    t->param = param;
  }
  avr_cycle_timer_register(avr, (avr_cycle_count_t)in, timer_call, t);
}

#define MAX_SIM_RESET_HOOKS 8
static void (*sim_reset_hooks[MAX_SIM_RESET_HOOKS])(void);
static int n_sim_reset_hooks;

static void reset_call(avr_t *a) {
  for (int i = 0; i < n_sim_reset_hooks; i++) sim_reset_hooks[i]();
}

static void avr_on_reset(void (*hook)(void)) {
  if (n_sim_reset_hooks == 0) reset_add_hook(reset_call);
  if (n_sim_reset_hooks < MAX_SIM_RESET_HOOKS) sim_reset_hooks[n_sim_reset_hooks++] = hook;
}

static int avr_pin(const char *s, sim_pin_t *pin, const char **end) {
  char port = s[0] & ~0x20; /* upper case */
  if (port < 'A' || port > 'L' || s[1] < '0' || s[1] > '7') return -1;
  pin->port = port;
  pin->bit = s[1] - '0';
  if (end) *end = s + 2;
  return 0;
}

static avr_irq_t *pin_irq(sim_pin_t pin) { return avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(pin.port), pin.bit); }

static int avr_has_pin(sim_pin_t pin) { return pin_irq(pin) != NULL; }

static const char *avr_pin_name(sim_pin_t pin) {
  static char name[8];
  snprintf(name, sizeof(name), "%c%d", pin.port, pin.bit);
  return name;
}

static void avr_drive(sim_pin_t pin, int level) { avr_raise_irq(pin_irq(pin), level); }

static void avr_pull(sim_pin_t pin, int level) {
  avr_ioport_external_t pull = {.name = (char)pin.port, .mask = (uint8_t)(1 << pin.bit), .value = (uint8_t)(level << pin.bit)};
  avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL(pin.port), &pull);
}

typedef struct {
  void (*fn)(void *param, int level);
  void *param;
} watch_adapter_t;

static void watch_call(struct avr_irq_t *irq, uint32_t value, void *param) {
  watch_adapter_t *w = param;
  w->fn(w->param, value != 0);
}

static void avr_watch(sim_pin_t pin, void (*fn)(void *param, int level), void *param) {
  watch_adapter_t *w = malloc(sizeof(*w));
  w->fn = fn;
  w->param = param;
  avr_irq_register_notify(pin_irq(pin), watch_call, w);
}

static int avr_analog(int channel, uint32_t mv) {
  if (channel == SIM_ANALOG_SUPPLY) {
    avr->vcc = avr->avcc = mv;
    return 0;
  }
  avr_irq_t *irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, channel == SIM_ANALOG_TEMP ? ADC_IRQ_TEMP : channel);
  if (!irq) return -1;
  avr_raise_irq(irq, mv);
  return 0;
}

static int avr_uart_send(int uart, uint8_t byte) {
  avr_irq_t *rx_irq = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0' + uart), UART_IRQ_INPUT);
  if (rx_irq) avr_raise_irq(rx_irq, byte);
  return 1;
}

static void (*listeners[4])(uint8_t byte);

static void uart_out_call(struct avr_irq_t *irq, uint32_t value, void *param) {
  listeners[(uintptr_t)param]((uint8_t)value);
}

static void avr_uart_listen(int uart, void (*fn)(uint8_t byte)) {
  uint32_t flags = 0;
  avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS('0' + uart), &flags);
  flags &= ~AVR_UART_FLAG_STDIO;
  avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS('0' + uart), &flags);

  avr_irq_t *tx = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0' + uart), UART_IRQ_OUTPUT);
  if (tx) {
    listeners[uart] = fn;
    avr_irq_register_notify(tx, uart_out_call, (void *)(uintptr_t)uart);
  }
}

const sim_backend_t sim_backend_avr = {
    .name = "simavr",
    .chips = "AVR parts: atmega328p (default), atmega2560, attiny85 and the rest simavr knows",
    .probe = avr_probe,
    .arg = avr_arg,
    .supports = avr_supports,
    .start = avr_start,
    .prepare = avr_prepare,
    .bridge = avr_bridge,
    .launch = avr_launch,
    .step = avr_step,
    .inspect = avr_inspect,
    .at_max_steps = avr_at_max_steps,
    .report = avr_report,
    .stop = avr_stop,
    .hz = avr_hz,
    .now = avr_now,
    .timer = avr_timer,
    .on_reset = avr_on_reset,
    .pin = avr_pin,
    .has_pin = avr_has_pin,
    .pin_name = avr_pin_name,
    .drive = avr_drive,
    .pull = avr_pull,
    .watch = avr_watch,
    .analog = avr_analog,
    .uart_send = avr_uart_send,
    .uart_listen = avr_uart_listen,
};

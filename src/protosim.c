/*
    protosim.c
    A simavr runner for Protoduino with PTY UART support.
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

#ifdef _WIN32
#include "uart_com.h"
#else
#include "uart_pty.h"
#endif

#include "stk500_uploader.h"
#include "protosim_core.h"
#include "debug.h"
#include "profiling.h"
#include "ihex_loader.h"

/* ═══════════════════════════════════════════════════════════════
   Globals (Definitions)
   ═══════════════════════════════════════════════════════════════ */

avr_t *avr = NULL;
int verbose = 0;
int opt_dump_regs = 0;
int opt_single_step = 0;
long opt_max_steps = 0;
long opt_trace_every = 0;

#ifdef _WIN32
uart_com_t uart_com;
#else
uart_pty_t uart_pty;
#endif

/* ═══════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
  char *mmcu = "atmega328p";
  const char *filename = NULL;
  uint32_t freq = 16000000;
  int debug_gdb = 0;
  const char *opt_bootloader = NULL;

  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  if (argc < 2) {
    fprintf(
        stderr,
        "Usage: %s <firmware.elf|hex> [options]\n"
        "\nCore options:\n"
        "  -m <mcu>               MCU type (default: atmega328p)\n"
        "  -f <freq>              CPU frequency Hz (default: 16000000)\n"
        "  -d                     Enable GDB server on port 1234\n"
        "  -v                     Verbose simavr output\n"
        "  --bootloader <file>    Load a bootloader .hex (or .elf) into flash\n"
        "                         and start execution at its entry point.\n"
        "\nLLM Debug tools:\n"
        "  -b <addr|symbol>       Breakpoint at hex addr or ELF symbol\n"
        "  -w <addr:sz:name>      Watch SRAM variable at breakpoints\n"
        "  --dump-regs            Dump all 32 CPU registers at each breakpoint\n"
        "  --dump-sram <addr:len> Hex+ASCII SRAM dump at each breakpoint\n"
        "  --max-steps <n>        Exit after N steps (prevents hangs)\n"
        "  -t <n>                 Trace: print PC + watches every N cycles\n"
        "  -s                     Single-step: print PC for every instruction\n"
        "\nLLM Profiling tools (ELF only):\n"
        "  --coverage             Per-function code coverage report\n"
        "  --profile              Cycle-exact flat performance profile\n"
        "  --callgraph            Call graph with per-edge cycle costs\n"
        "  --profile-out <file>   Write profiling reports to file\n",
        argv[0]);
    exit(1);
  }

  filename = argv[1];

  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-m")) {
      if (i + 1 < argc) mmcu = argv[++i];
    } else if (!strcmp(argv[i], "-f")) {
      if (i + 1 < argc) freq = (uint32_t)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "-d")) {
      debug_gdb++;
    } else if (!strcmp(argv[i], "-v")) {
      verbose++;
    } else if (!strcmp(argv[i], "-b")) {
      if (i + 1 < argc) add_breakpoint_arg(argv[++i]);
    } else if (!strcmp(argv[i], "-w")) {
      if (i + 1 < argc) add_watch_arg(argv[++i]);
    } else if (!strcmp(argv[i], "--dump-regs")) {
      opt_dump_regs = 1;
    } else if (!strcmp(argv[i], "--dump-sram")) {
      if (i + 1 < argc) add_sram_dump_arg(argv[++i]);
    } else if (!strcmp(argv[i], "--max-steps")) {
      if (i + 1 < argc) opt_max_steps = atol(argv[++i]);
    } else if (!strcmp(argv[i], "-t")) {
      if (i + 1 < argc) opt_trace_every = atol(argv[++i]);
    } else if (!strcmp(argv[i], "-s")) {
      opt_single_step = 1;
    } else if (!strcmp(argv[i], "--coverage")) {
      opt_coverage = 1;
    } else if (!strcmp(argv[i], "--profile")) {
      opt_profile = 1;
      opt_callgraph = 1;
    } else if (!strcmp(argv[i], "--callgraph")) {
      opt_callgraph = 1;
      opt_profile = 1;
    } else if (!strcmp(argv[i], "--profile-out")) {
      if (i + 1 < argc) opt_profile_out = argv[++i];
    } else if (!strcmp(argv[i], "--bootloader")) {
      if (i + 1 < argc) opt_bootloader = argv[++i];
    }
  }

  /* ── Load firmware ───────────────────────────────────────── */
  int is_hex = (strstr(filename, ".hex") != NULL);
  elf_firmware_t f = {0};
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

  if (is_hex) {
    memcpy(avr->flash + hex_base, hex_data, hex_size);
    free(hex_data);
    avr->pc = hex_base;
    avr->codeend = hex_base + hex_size;
  } else {
    avr_load_firmware(avr, &f);
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
  uint8_t *stk_app_binary = NULL;
  size_t   stk_app_size   = 0;
  if (opt_bootloader) {
    stk_app_size   = (avr->codeend > 0) ? avr->codeend : (avr->flashend + 1);
    stk_app_binary = (uint8_t *)malloc(stk_app_size);
    if (!stk_app_binary) { perror("malloc stk_app_binary"); exit(1); }
    memcpy(stk_app_binary, avr->flash, stk_app_size);
    printf("[STK] Captured app binary: %zu bytes from flash\n", stk_app_size);
  }

  /* ── Bootloader overlay ──────────────────────────────────── */
  if (opt_bootloader) {
    int bl_is_hex = (strstr(opt_bootloader, ".hex")  != NULL || strstr(opt_bootloader, ".ihex") != NULL);
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
      avr_load_firmware(tmp, &bl_fw);
      bl_entry = tmp->pc;
      memcpy(avr->flash, tmp->flash, tmp->flashend + 1);
      printf("[BL] Loaded bootloader ELF: %s  entry=0x%04x\n", opt_bootloader, bl_entry);
    }
    avr->pc = bl_entry;
    printf("[BL] Initial PC set to bootloader entry 0x%04x\n", bl_entry);
    if ((bl_entry + bl_size) > avr->codeend) avr->codeend = bl_entry + bl_size;
  }

  avr->log = 1 + verbose;

  if (debug_gdb) {
    avr->gdb_port = 1234;
    avr->state = cpu_Stopped;
    avr_gdb_init(avr);
    printf("GDB server listening on port 1234\n");
  }

  /* ── Build profiling tables ───────────────────────────────── */
  if (opt_coverage || opt_profile || opt_callgraph) {
    if (!is_hex) build_func_table(&f, avr->flashend);
    profiling_alloc(avr->flashend);
  }

  /* ── PTY/COM UART ────────────────────────────────────────────── */
#ifdef _WIN32
  uart_com_init(avr, &uart_com);
  uart_com_connect(&uart_com, '0');
#else
  uart_pty_init(avr, &uart_pty);
  uart_pty_connect(&uart_pty, '0');
#endif

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

  /* ══════════════════════════════════════════════════════════
     Main simulation loop
     ══════════════════════════════════════════════════════════ */
  long steps = 0;
  uint64_t last_trace_cyc = 0;

  while (1) {
    uint32_t pre_pc = avr->pc;
    uint64_t pre_cycle = avr->cycle;

    int state = avr_run(avr);

    if (opt_coverage || opt_profile || opt_callgraph)
      profiling_step(pre_pc, pre_cycle, avr->pc, avr->cycle);

    if (state == cpu_Done || state == cpu_Crashed) {
      printf("[DBG] CPU %s at PC=0x%04x after %ld steps\n", state == cpu_Done ? "DONE" : "CRASHED", avr->pc, steps);
      break;
    }

    steps++;

    if (opt_max_steps && steps >= opt_max_steps) {
      printf("[DBG] MAX-STEPS (%ld) reached — stopping.\n", opt_max_steps);
      if (opt_dump_regs) dump_registers(avr);
      dump_watches(avr);
      break;
    }

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

  return 0;
}

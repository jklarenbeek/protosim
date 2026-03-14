/*
    protosim.c
    A simavr runner for Protoduino with PTY UART support.

    ── LLM Debug Tools ──────────────────────────────────────────
      -b <addr|symbol>       Breakpoint at PC address or ELF symbol
      -w <addr:sz:name>      Watch SRAM variable (dump on breakpoint)
      --dump-regs            Dump all 32 CPU regs + SP + SREG at breakpoints
      --dump-sram <addr:len> Hex+ASCII dump of SRAM range at breakpoints
      --max-steps <n>        Exit after N steps (prevents infinite loops)
      -t <n>                 Trace: dump PC + watches every N cycles
      -s                     Single-step: print PC every instruction

    ── LLM Profiling Tools ──────────────────────────────────────
      --coverage             Per-function code coverage % report
      --profile              Cycle-exact flat performance profile
      --callgraph            Call graph with per-edge cycle costs + hot path
      --profile-out <file>   Write profiling reports to file (default: stdout)
 */

#ifdef _WIN32
/* Tell win32_compat.h that this is protosim (not a simavr file) */
#define PROTOSIM_BUILD 1
/* win32_compat.h provides basename() — no libgen.h needed on Windows */
#else
#include <libgen.h>
#endif
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "avr_ioport.h"
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

/* ═══════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════ */

#define MAX_BREAKPOINTS 32
#define MAX_WATCHES 32
#define MAX_SRAM_DUMPS 8
#define MAX_FUNCTIONS 512
#define MAX_CG_EDGES 4096
#define MAX_CALL_DEPTH 128

/* ATmega328P: 32 KB flash = 16 K words.  One entry per flash word. */
#define FLASH_WORDS_MAX (32768 / 2)

/* ═══════════════════════════════════════════════════════════════
   AVR opcode detection
   All instructions are little-endian 16-bit words in flash.

   CALL  : 1001 010k kkkk 111k  (word 1 of 2-word instruction)
   RCALL : 1101 kkkk kkkk kkkk
   ICALL : 1001 0101 0000 1001  (exact)
   RET   : 1001 0101 0000 1000  (exact)
   RETI  : 1001 0101 0001 1000  (exact)
   ═══════════════════════════════════════════════════════════════ */

static inline uint16_t flash_read16(avr_t *a, uint32_t pc) {
  return (uint16_t)(a->flash[pc]) | ((uint16_t)(a->flash[pc + 1]) << 8);
}

static inline int is_CALL(uint16_t op) { return (op & 0xFE0E) == 0x940E; }
static inline int is_RCALL(uint16_t op) { return (op & 0xF000) == 0xD000; }
static inline int is_ICALL(uint16_t op) { return op == 0x9509; }
static inline int is_RET(uint16_t op) { return op == 0x9508; }
static inline int is_RETI(uint16_t op) { return op == 0x9518; }

static inline int is_any_call(uint16_t op) {
  return is_CALL(op) || is_RCALL(op) || is_ICALL(op);
}
static inline int is_any_ret(uint16_t op) { return is_RET(op) || is_RETI(op); }

/* ═══════════════════════════════════════════════════════════════
   Debug structures
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  uint32_t addr;
  char label[64];
  uint64_t hit_count;
} breakpoint_t;

typedef struct {
  uint32_t sram_addr;
  uint8_t size;
  char name[32];
} watch_t;

typedef struct {
  uint32_t sram_addr;
  uint32_t length;
} sram_dump_t;

/* ═══════════════════════════════════════════════════════════════
   Profiling structures
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
  char name[64];
  uint32_t start;        /* flash byte address, inclusive       */
  uint32_t end;          /* flash byte address, exclusive       */
  uint64_t total_cycles; /* total cycles inside this function   */
  uint64_t call_count;
  uint64_t self_cycles; /* total minus time in callees         */
} func_profile_t;

typedef struct {
  uint32_t caller_func;
  uint32_t callee_func;
  uint64_t call_count;
  uint64_t total_cycles;
} cg_edge_t;

typedef struct {
  uint32_t func_idx;
  uint32_t caller_func;
  uint64_t entry_cycle;
  uint64_t callee_cycles; /* time we spent in callees from here  */
} call_frame_t;

/* ═══════════════════════════════════════════════════════════════
   Globals
   ═══════════════════════════════════════════════════════════════ */

static breakpoint_t breakpoints[MAX_BREAKPOINTS];
static int n_breakpoints = 0;
static watch_t watches[MAX_WATCHES];
static int n_watches = 0;
static sram_dump_t sram_dumps[MAX_SRAM_DUMPS];
static int n_sram_dumps = 0;

static int opt_dump_regs = 0;
static int opt_single_step = 0;
static long opt_max_steps = 0;
static long opt_trace_every = 0;
static int opt_coverage = 0;
static int opt_profile = 0;
static int opt_callgraph = 0;
static const char *opt_profile_out = NULL;
static const char *opt_bootloader = NULL;
static uint32_t bl_region_start = 0; /* inclusive byte addr of bootloader */
static uint32_t bl_region_end   = 0; /* exclusive byte addr (0 = no bootloader) */

static uint8_t *coverage_map = NULL;
static uint64_t *pc_cycles = NULL;
static func_profile_t func_table[MAX_FUNCTIONS];
static int n_funcs = 0;
static cg_edge_t cg_edges[MAX_CG_EDGES];
static int n_cg_edges = 0;
static call_frame_t call_stack[MAX_CALL_DEPTH];
static int call_depth = 0;

#ifdef _WIN32
uart_com_t uart_com;
#else
uart_pty_t uart_pty;
#endif
avr_t *avr = NULL;

/* ═══════════════════════════════════════════════════════════════
   SREG print
   ═══════════════════════════════════════════════════════════════ */

static const char sreg_bits[] = "CZNVSHTI";

static void print_sreg(uint8_t sreg) {
  printf("  SREG: ");
  for (int i = 7; i >= 0; i--)
    printf("%c:%d ", sreg_bits[i], (sreg >> i) & 1);
  printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   Debug dump helpers
   ═══════════════════════════════════════════════════════════════ */

static void dump_registers(avr_t *a) {
  printf("[DBG] === Register Dump @ PC=0x%04x (cycle=%" PRIu64 ") ===\n", a->pc,
         a->cycle);
  for (int r = 0; r < 32; r++) {
    uint8_t v = a->data[r];
    printf("  r%-2d = 0x%02x  %3d  %c\n", r, v, v, isprint(v) ? v : '.');
  }
  uint16_t X = a->data[26] | ((uint16_t)a->data[27] << 8);
  uint16_t Y = a->data[28] | ((uint16_t)a->data[29] << 8);
  uint16_t Z = a->data[30] | ((uint16_t)a->data[31] << 8);
  printf("  X=0x%04x  Y=0x%04x  Z=0x%04x\n", X, Y, Z);
  uint16_t sp = a->data[R_SPL] | ((uint16_t)a->data[R_SPH] << 8);
  printf("  SP=0x%04x\n", sp);
  print_sreg(a->sreg[0]);
  printf("[DBG] =========================================\n");
}

static void dump_watches(avr_t *a) {
  if (n_watches == 0)
    return;
  printf("[DBG] --- Watched Variables ---\n");
  for (int i = 0; i < n_watches; i++) {
    watch_t *w = &watches[i];
    uint32_t val = 0;
    for (int b = (int)w->size - 1; b >= 0; b--)
      val = (val << 8) | a->data[w->sram_addr + b];
    printf("  %-20s @ SRAM[0x%04x] size=%d = 0x%0*x (%u)\n", w->name,
           w->sram_addr, w->size, w->size * 2, val, val);
  }
}

static void dump_sram_range(avr_t *a, uint32_t base, uint32_t len) {
  printf("[DBG] --- SRAM 0x%04x .. 0x%04x ---\n", base, base + len - 1);
  for (uint32_t off = 0; off < len; off++) {
    if (off % 16 == 0)
      printf("  %04x: ", base + off);
    printf("%02x ", a->data[base + off]);
    if (off % 16 == 15 || off == len - 1) {
      uint32_t row_start = (off / 16) * 16;
      uint32_t row_end = (off < len - 1) ? off : len - 1;
      for (uint32_t pad = row_end % 16; pad < 15; pad++)
        printf("   ");
      printf(" |");
      for (uint32_t k = row_start; k <= row_end; k++) {
        uint8_t c = a->data[base + k];
        printf("%c", isprint(c) ? c : '.');
      }
      printf("|\n");
    }
  }
}

static void on_breakpoint_hit(avr_t *a, const char *label) {
  printf("[DBG] BREAKPOINT HIT: %s  PC=0x%04x  cycle=%" PRIu64 "\n", label,
         a->pc, a->cycle);
  if (opt_dump_regs)
    dump_registers(a);
  dump_watches(a);
  for (int i = 0; i < n_sram_dumps; i++)
    dump_sram_range(a, sram_dumps[i].sram_addr, sram_dumps[i].length);
  fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
   ELF / symbol helpers
   ═══════════════════════════════════════════════════════════════ */

static uint32_t resolve_symbol(elf_firmware_t *f, const char *sym) {
  if (!f)
    return UINT32_MAX;
  for (int i = 0; i < (int)f->symbolcount; i++)
    if (strcmp(f->symbol[i]->symbol, sym) == 0)
      return f->symbol[i]->addr;
  return UINT32_MAX;
}

static int pc_to_func(uint32_t pc) {
  for (int i = 0; i < n_funcs; i++)
    if (pc >= func_table[i].start && pc < func_table[i].end)
      return i;
  return -1;
}

/* ═══════════════════════════════════════════════════════════════
   Build function table from ELF symbols
   ═══════════════════════════════════════════════════════════════ */

static int cmp_sym_addr(const void *a, const void *b) {
  const avr_symbol_t *sa = *(const avr_symbol_t **)a;
  const avr_symbol_t *sb = *(const avr_symbol_t **)b;
  return (sa->addr > sb->addr) - (sa->addr < sb->addr);
}

static void build_func_table(elf_firmware_t *f, uint32_t flashend) {
  if (!f || f->symbolcount == 0)
    return;

  avr_symbol_t **sorted = malloc(f->symbolcount * sizeof(avr_symbol_t *));
  if (!sorted)
    return;
  for (int i = 0; i < (int)f->symbolcount; i++)
    sorted[i] = f->symbol[i];
  qsort(sorted, f->symbolcount, sizeof(avr_symbol_t *), cmp_sym_addr);

  n_funcs = 0;
  for (int i = 0; i < (int)f->symbolcount && n_funcs < MAX_FUNCTIONS; i++) {
    uint32_t addr = sorted[i]->addr;
    const char *name = sorted[i]->symbol;

    if (!name || !name[0])
      continue;
    if (addr >= flashend)
      continue; /* data/bss, not text  */
    if (name[0] == '.' || name[0] == '$')
      continue; /* internal */

    func_profile_t *fp = &func_table[n_funcs++];
    strncpy(fp->name, name, sizeof(fp->name) - 1);
    fp->start = addr;
    fp->total_cycles = 0;
    fp->call_count = 0;
    fp->self_cycles = 0;

    /* Extent = up to next symbol in flash */
    uint32_t next = flashend;
    for (int j = i + 1; j < (int)f->symbolcount; j++) {
      if (sorted[j]->addr > addr && sorted[j]->addr < flashend) {
        next = sorted[j]->addr;
        break;
      }
    }
    fp->end = next;
  }
  free(sorted);
}

/* ═══════════════════════════════════════════════════════════════
   Allocate per-PC profiling arrays
   ═══════════════════════════════════════════════════════════════ */

static void profiling_alloc(uint32_t flashend) {
  uint32_t words = (flashend + 1) / 2;
  if (words > FLASH_WORDS_MAX)
    words = FLASH_WORDS_MAX;

  if (opt_coverage) {
    coverage_map = calloc(words, 1);
    if (!coverage_map) {
      fprintf(stderr, "WARN: coverage_map alloc failed\n");
      opt_coverage = 0;
    }
  }
  if (opt_profile) {
    pc_cycles = calloc(words, sizeof(uint64_t));
    if (!pc_cycles) {
      fprintf(stderr, "WARN: pc_cycles alloc failed\n");
      opt_profile = 0;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════
   Per-instruction profiling hook — called every step
   pre_pc / pre_cycle: state BEFORE avr_run()
   post_pc / post_cycle: state AFTER avr_run()
   ═══════════════════════════════════════════════════════════════ */

static void profiling_step(uint32_t pre_pc, uint64_t pre_cycle,
                           uint32_t post_pc, uint64_t post_cycle) {
  uint64_t elapsed = post_cycle - pre_cycle;
  uint32_t word_idx = pre_pc >> 1;

  /* Coverage: mark word executed */
  if (opt_coverage && coverage_map && word_idx < FLASH_WORDS_MAX)
    coverage_map[word_idx] = 1;

  /* Flat profile: accumulate cycles at this word */
  if (opt_profile && pc_cycles && word_idx < FLASH_WORDS_MAX)
    pc_cycles[word_idx] += elapsed;

  /* Call graph: detect CALL and RET instructions */
  if (!opt_callgraph && !opt_profile)
    return;

  uint16_t opcode = flash_read16(avr, pre_pc);
  int callee_fi = pc_to_func(post_pc);
  int caller_fi = (call_depth > 0) ? (int)call_stack[call_depth - 1].func_idx
                                   : pc_to_func(pre_pc);

  if (is_any_call(opcode)) {
    /* post_pc is the callee's entry point */
    if (call_depth < MAX_CALL_DEPTH) {
      call_frame_t *fr = &call_stack[call_depth++];
      fr->func_idx = (callee_fi >= 0) ? (uint32_t)callee_fi : UINT32_MAX;
      fr->caller_func = (caller_fi >= 0) ? (uint32_t)caller_fi : UINT32_MAX;
      fr->entry_cycle = post_cycle;
      fr->callee_cycles = 0;

      if (callee_fi >= 0)
        func_table[callee_fi].call_count++;
    }

  } else if (is_any_ret(opcode) && call_depth > 0) {
    call_frame_t *fr = &call_stack[--call_depth];
    uint64_t frame_cycles = post_cycle - fr->entry_cycle;

    if (fr->func_idx != UINT32_MAX)
      func_table[fr->func_idx].total_cycles += frame_cycles;

    if (call_depth > 0)
      call_stack[call_depth - 1].callee_cycles += frame_cycles;

    if (opt_callgraph && fr->func_idx != UINT32_MAX &&
        fr->caller_func != UINT32_MAX) {
      cg_edge_t *edge = NULL;
      for (int e = 0; e < n_cg_edges; e++) {
        if (cg_edges[e].caller_func == fr->caller_func &&
            cg_edges[e].callee_func == fr->func_idx) {
          edge = &cg_edges[e];
          break;
        }
      }
      if (!edge && n_cg_edges < MAX_CG_EDGES) {
        edge = &cg_edges[n_cg_edges++];
        edge->caller_func = fr->caller_func;
        edge->callee_func = fr->func_idx;
        edge->call_count = 0;
        edge->total_cycles = 0;
      }
      if (edge) {
        edge->call_count++;
        edge->total_cycles += frame_cycles;
      }
    }
  } else if (callee_fi != pc_to_func(pre_pc)) {
    /* We changed functions without a CALL or RET */
    if (callee_fi >= 0 &&
        strcmp(func_table[callee_fi].name, "__vectors") == 0) {
      /* Interrupt vectoring! PUSH frame */
      if (call_depth < MAX_CALL_DEPTH) {
        call_frame_t *fr = &call_stack[call_depth++];
        fr->func_idx = (uint32_t)callee_fi;
        fr->caller_func = (caller_fi >= 0) ? (uint32_t)caller_fi : UINT32_MAX;
        fr->entry_cycle = post_cycle;
        fr->callee_cycles = 0;
        func_table[callee_fi].call_count++;
      }
    } else if (call_depth > 0) {
      /* Tail call, or JMP from __vectors to an ISR. Replace current frame. */
      call_stack[call_depth - 1].func_idx =
          (callee_fi >= 0) ? (uint32_t)callee_fi : UINT32_MAX;
      if (callee_fi >= 0)
        func_table[callee_fi].call_count++;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════
   Sorting comparators
   ═══════════════════════════════════════════════════════════════ */

static int cmp_func_self_desc(const void *a, const void *b) {
  const func_profile_t *fa = (const func_profile_t *)a;
  const func_profile_t *fb = (const func_profile_t *)b;
  return (fa->self_cycles < fb->self_cycles) -
         (fa->self_cycles > fb->self_cycles);
}

static int cmp_edge_cycles_desc(const void *a, const void *b) {
  const cg_edge_t *ea = (const cg_edge_t *)a;
  const cg_edge_t *eb = (const cg_edge_t *)b;
  return (ea->total_cycles < eb->total_cycles) -
         (ea->total_cycles > eb->total_cycles);
}

/* ═══════════════════════════════════════════════════════════════
   Compute self_cycles from pc_cycles[] array
   ═══════════════════════════════════════════════════════════════ */

static void finalize_call_stack(uint64_t post_cycle) {
  while (call_depth > 0) {
    call_frame_t *fr = &call_stack[--call_depth];
    uint64_t frame_cycles = post_cycle - fr->entry_cycle;

    if (fr->func_idx != UINT32_MAX)
      func_table[fr->func_idx].total_cycles += frame_cycles;

    if (call_depth > 0)
      call_stack[call_depth - 1].callee_cycles += frame_cycles;

    if (opt_callgraph && fr->func_idx != UINT32_MAX &&
        fr->caller_func != UINT32_MAX) {
      cg_edge_t *edge = NULL;
      for (int e = 0; e < n_cg_edges; e++) {
        if (cg_edges[e].caller_func == fr->caller_func &&
            cg_edges[e].callee_func == fr->func_idx) {
          edge = &cg_edges[e];
          break;
        }
      }
      if (!edge && n_cg_edges < MAX_CG_EDGES) {
        edge = &cg_edges[n_cg_edges++];
        edge->caller_func = fr->caller_func;
        edge->callee_func = fr->func_idx;
        edge->call_count = 0;
        edge->total_cycles = 0;
      }
      if (edge) {
        // We don't increment call_count here because it was already incremented
        // on push
        edge->total_cycles += frame_cycles;
      }
    }
  }
}

static void finalize_self_cycles(void) {
  if (!pc_cycles)
    return;
  for (int fi = 0; fi < n_funcs; fi++) {
    func_profile_t *fp = &func_table[fi];
    uint64_t self = 0;
    for (uint32_t pc = fp->start; pc < fp->end; pc += 2) {
      uint32_t wi = pc >> 1;
      if (wi < FLASH_WORDS_MAX)
        self += pc_cycles[wi];
    }
    fp->self_cycles = self;
  }
}

/* ═══════════════════════════════════════════════════════════════
   Report: Code Coverage
   ═══════════════════════════════════════════════════════════════ */

static void report_coverage(FILE *out, uint64_t total_cycles, uint32_t freq) {
  fprintf(out,
          "\n[COV] ════════════════════════════════════════════════════════\n"
          "[COV]  Code Coverage Report\n"
          "[COV] ════════════════════════════════════════════════════════\n");

  if (!coverage_map || n_funcs == 0) {
    fprintf(out, "[COV]  (no data — requires --coverage with an ELF input)\n");
    return;
  }

  uint32_t total_words = 0, hit_words = 0;

  fprintf(out, "[COV]  %-32s  %6s  %6s  %8s\n", "Function", "Words", "Hit",
          "Coverage");
  fprintf(out, "[COV]  %-32s  %6s  %6s  %8s\n",
          "--------------------------------", "------", "------", "--------");

  for (int fi = 0; fi < n_funcs; fi++) {
    func_profile_t *fp = &func_table[fi];
    uint32_t fwords = 0, fhit = 0;
    for (uint32_t pc = fp->start; pc < fp->end; pc += 2) {
      uint32_t wi = pc >> 1;
      if (wi >= FLASH_WORDS_MAX)
        break;
      fwords++;
      if (coverage_map[wi])
        fhit++;
    }
    if (fwords == 0)
      continue;
    total_words += fwords;
    hit_words += fhit;
    double pct = 100.0 * fhit / fwords;
    const char *tag = (fhit == 0)    ? "  ← NEVER REACHED"
                      : (pct < 50.0) ? "  ← PARTIAL"
                                     : "";
    fprintf(out, "[COV]  %-32s  %6u  %6u  %7.1f%%%s\n", fp->name, fwords, fhit,
            pct, tag);
  }

  fprintf(out, "[COV]  %-32s  %6s  %6s  %8s\n",
          "--------------------------------", "------", "------", "--------");
  double tot_pct = total_words ? 100.0 * hit_words / total_words : 0.0;
  fprintf(out, "[COV]  %-32s  %6u  %6u  %7.1f%%\n", "TOTAL", total_words,
          hit_words, tot_pct);

  if (freq > 0) {
    double ms = (double)total_cycles * 1000.0 / freq;
    fprintf(out,
            "[COV]\n[COV]  Simulated time : %.3f ms @ %u Hz  (%" PRIu64
            " cycles)\n",
            ms, freq, total_cycles);
  }
  fprintf(out,
          "[COV] ════════════════════════════════════════════════════════\n");
}

/* ═══════════════════════════════════════════════════════════════
   Report: Flat Profile
   ═══════════════════════════════════════════════════════════════ */

static void report_profile(FILE *out, uint64_t total_cycles, uint32_t freq) {
  fprintf(
      out,
      "\n[PROF] "
      "═══════════════════════════════════════════════════════════════════\n"
      "[PROF]  Flat Performance Profile  (cycle-exact — firmware unmodified)\n"
      "[PROF] "
      "═══════════════════════════════════════════════════════════════════\n");

  if (!pc_cycles || n_funcs == 0) {
    fprintf(out, "[PROF]  (no data — requires --profile with an ELF input)\n");
    return;
  }

  finalize_self_cycles();

  func_profile_t sorted[MAX_FUNCTIONS];
  int n = n_funcs;
  memcpy(sorted, func_table, n * sizeof(func_profile_t));
  qsort(sorted, n, sizeof(func_profile_t), cmp_func_self_desc);

  fprintf(out, "[PROF]  %4s  %-28s  %14s  %7s  %10s  %12s\n", "Rank",
          "Function", "Self Cycles", "Self %", "Calls", "Cyc/Call");
  fprintf(out, "[PROF]  %4s  %-28s  %14s  %7s  %10s  %12s\n", "----",
          "----------------------------", "--------------", "-------",
          "----------", "------------");

  for (int i = 0; i < n; i++) {
    func_profile_t *fp = &sorted[i];
    if (fp->self_cycles == 0 && fp->call_count == 0)
      continue;
    double pct = total_cycles ? 100.0 * fp->self_cycles / total_cycles : 0.0;
    uint64_t cpc = fp->call_count ? fp->self_cycles / fp->call_count : 0;
    const char *hot = (pct >= 20.0)  ? "  ◄ HOT"
                      : (pct >= 5.0) ? "  ◄ warm"
                                     : "";
    fprintf(out,
            "[PROF]  %4d  %-28s  %14" PRIu64 "  %6.2f%%  %10" PRIu64
            "  %12" PRIu64 "%s\n",
            i + 1, fp->name, fp->self_cycles, pct, fp->call_count, cpc, hot);
  }

  fprintf(out, "[PROF]\n");
  if (freq > 0) {
    double ms = (double)total_cycles * 1000.0 / freq;
    fprintf(out,
            "[PROF]  Total cycles   : %" PRIu64 "\n"
            "[PROF]  Simulated time : %.3f ms @ %u Hz\n",
            total_cycles, ms, freq);
  } else {
    fprintf(out, "[PROF]  Total cycles   : %" PRIu64 "\n", total_cycles);
  }
  fprintf(
      out,
      "[PROF] "
      "═══════════════════════════════════════════════════════════════════\n");
}

/* ═══════════════════════════════════════════════════════════════
   Call graph: recursive subtree printer
   ═══════════════════════════════════════════════════════════════ */

static void print_cg_subtree(FILE *out, int fi, int depth,
                             uint64_t total_cycles, uint8_t *visited,
                             int max_depth) {
  if (depth > max_depth || fi < 0 || fi >= n_funcs)
    return;
  if (visited[fi])
    return;
  visited[fi] = 1;

  /* Collect + sort children */
  cg_edge_t children[MAX_CG_EDGES];
  int nc = 0;
  for (int e = 0; e < n_cg_edges; e++)
    if ((int)cg_edges[e].caller_func == fi && nc < MAX_CG_EDGES)
      children[nc++] = cg_edges[e];
  qsort(children, nc, sizeof(cg_edge_t), cmp_edge_cycles_desc);

  for (int c = 0; c < nc; c++) {
    int callee = (int)children[c].callee_func;
    if (callee < 0 || callee >= n_funcs)
      continue;
    double pct =
        total_cycles ? 100.0 * children[c].total_cycles / total_cycles : 0.0;
    uint64_t avg = children[c].call_count
                       ? children[c].total_cycles / children[c].call_count
                       : 0;

    fprintf(out, "[CG]   ");
    for (int d = 0; d < depth; d++)
      fprintf(out, "  ");
    fprintf(out,
            "→ %-26s  %7" PRIu64 " calls  %14" PRIu64
            " cyc  %6.2f%%  avg %10" PRIu64 "\n",
            func_table[callee].name, children[c].call_count,
            children[c].total_cycles, pct, avg);

    print_cg_subtree(out, callee, depth + 1, total_cycles, visited, max_depth);
  }
}

/* ═══════════════════════════════════════════════════════════════
   Report: Call Graph
   ═══════════════════════════════════════════════════════════════ */

static void report_callgraph(FILE *out, uint64_t total_cycles, uint32_t freq) {
  fprintf(
      out,
      "\n[CG] "
      "═══════════════════════════════════════════════════════════════════\n"
      "[CG]  Call Graph  (cycle costs per call edge)\n"
      "[CG] "
      "═══════════════════════════════════════════════════════════════════\n");

  if (n_cg_edges == 0) {
    fprintf(
        out,
        "[CG]  (no call graph data — no matched CALL/RET pairs in this run)\n"
        "[CG]  Tip: increase --max-steps or run --coverage to verify "
        "reachability.\n");
    return;
  }

  /* Sorted copy of edges */
  cg_edge_t sorted[MAX_CG_EDGES];
  memcpy(sorted, cg_edges, n_cg_edges * sizeof(cg_edge_t));
  qsort(sorted, n_cg_edges, sizeof(cg_edge_t), cmp_edge_cycles_desc);

  /* ── Edge summary table ──────────────────────────────────── */
  fprintf(out, "[CG]\n[CG]  Edge Summary (sorted by total cycles consumed):\n");
  fprintf(out, "[CG]  %-24s  %-24s  %8s  %14s  %7s  %12s\n", "Caller", "Callee",
          "Calls", "Total Cycles", "% Time", "Avg Cycles");
  fprintf(out, "[CG]  %-24s  %-24s  %8s  %14s  %7s  %12s\n",
          "------------------------", "------------------------", "--------",
          "--------------", "-------", "------------");

  for (int e = 0; e < n_cg_edges; e++) {
    cg_edge_t *edge = &sorted[e];
    if (edge->callee_func >= (uint32_t)n_funcs ||
        edge->caller_func >= (uint32_t)n_funcs)
      continue;
    double pct = total_cycles ? 100.0 * edge->total_cycles / total_cycles : 0.0;
    uint64_t avg = edge->call_count ? edge->total_cycles / edge->call_count : 0;
    const char *hot = (pct >= 20.0) ? " ◄ HOT" : (pct >= 5.0) ? " ◄ warm" : "";
    fprintf(out,
            "[CG]  %-24s  %-24s  %8" PRIu64 "  %14" PRIu64
            "  %6.2f%%  %12" PRIu64 "%s\n",
            func_table[edge->caller_func].name,
            func_table[edge->callee_func].name, edge->call_count,
            edge->total_cycles, pct, avg, hot);
  }

  /* ── Call tree ───────────────────────────────────────────── */
  fprintf(out, "[CG]\n[CG]  Call Tree:\n");
  fprintf(out, "[CG]   %-28s  %7s  %14s  %7s  %10s\n", "Function", "Calls",
          "Total Cycles", "% Time", "Avg Cyc");
  fprintf(out, "[CG]   %-28s  %7s  %14s  %7s  %10s\n",
          "----------------------------", "-------", "--------------",
          "-------", "----------");

  /* Find root functions: not a callee in any edge */
  uint8_t is_callee[MAX_FUNCTIONS] = {0};
  for (int e = 0; e < n_cg_edges; e++)
    if (cg_edges[e].callee_func < (uint32_t)MAX_FUNCTIONS)
      is_callee[cg_edges[e].callee_func] = 1;

  uint8_t visited[MAX_FUNCTIONS] = {0};
  for (int fi = 0; fi < n_funcs; fi++) {
    if (is_callee[fi])
      continue;

    uint8_t has_outgoing = 0;
    for (int e = 0; e < n_cg_edges; e++) {
      if (cg_edges[e].caller_func == (uint32_t)fi) {
        has_outgoing = 1;
        break;
      }
    }

    if (func_table[fi].total_cycles == 0 && func_table[fi].call_count == 0 &&
        !has_outgoing)
      continue;

    double pct =
        total_cycles ? 100.0 * func_table[fi].total_cycles / total_cycles : 0.0;
    uint64_t avg = func_table[fi].call_count
                       ? func_table[fi].total_cycles / func_table[fi].call_count
                       : 0;
    fprintf(out,
            "[CG]   %-28s  %7" PRIu64 "  %14" PRIu64 "  %6.2f%%  %10" PRIu64
            "\n",
            func_table[fi].name, func_table[fi].call_count,
            func_table[fi].total_cycles, pct, avg);
    print_cg_subtree(out, fi, 1, total_cycles, visited, 8);
  }

  /* ── Hot path ────────────────────────────────────────────── */
  fprintf(out, "[CG]\n[CG]  Hot Path (highest-cost execution chain):\n[CG]  ");

  if (n_cg_edges > 0) {
    /* Trace back from hottest edge to find true root */
    int root = (int)sorted[0].caller_func;
    for (int guard = 0; guard < n_funcs; guard++) {
      int parent = -1;
      for (int e = 0; e < n_cg_edges; e++)
        if ((int)cg_edges[e].callee_func == root) {
          parent = (int)cg_edges[e].caller_func;
          break;
        }
      if (parent < 0)
        break;
      root = parent;
    }

    /* Walk forward always picking the hottest outgoing edge */
    int cur = root;
    int path_len = 0;
    uint8_t path_seen[MAX_FUNCTIONS] = {0};
    while (cur >= 0 && cur < n_funcs && !path_seen[cur]) {
      path_seen[cur] = 1;
      if (path_len > 0)
        fprintf(out, " → ");
      fprintf(out, "%s", func_table[cur].name);
      path_len++;
      int next = -1;
      uint64_t best = 0;
      for (int e = 0; e < n_cg_edges; e++)
        if ((int)cg_edges[e].caller_func == cur &&
            cg_edges[e].total_cycles > best) {
          best = cg_edges[e].total_cycles;
          next = (int)cg_edges[e].callee_func;
        }
      cur = next;
    }
    double path_pct =
        total_cycles ? 100.0 * sorted[0].total_cycles / total_cycles : 0.0;
    fprintf(out, "\n[CG]  (hottest single edge: %.1f%% of total runtime)\n",
            path_pct);
  }

  if (freq > 0) {
    double ms = (double)total_cycles * 1000.0 / freq;
    fprintf(out, "[CG]\n[CG]  Total cycles : %" PRIu64 "  (%.3f ms @ %u Hz)\n",
            total_cycles, ms, freq);
  }
  fprintf(
      out,
      "[CG] "
      "═══════════════════════════════════════════════════════════════════\n");
}

/* ═══════════════════════════════════════════════════════════════
   Argument parsers
   ═══════════════════════════════════════════════════════════════ */

static void add_breakpoint_arg(const char *arg) {
  if (n_breakpoints >= MAX_BREAKPOINTS) {
    fprintf(stderr, "WARN: too many breakpoints (max %d)\n", MAX_BREAKPOINTS);
    return;
  }
  breakpoint_t *bp = &breakpoints[n_breakpoints++];
  bp->hit_count = 0;
  strncpy(bp->label, arg, sizeof(bp->label) - 1);
  char *end;
  long addr = strtol(arg, &end, 0);
  bp->addr = (*end == '\0' && end != arg) ? (uint32_t)addr : UINT32_MAX;
}

static void add_watch_arg(const char *arg) {
  if (n_watches >= MAX_WATCHES) {
    fprintf(stderr, "WARN: too many watches (max %d)\n", MAX_WATCHES);
    return;
  }
  char tmp[128];
  strncpy(tmp, arg, sizeof(tmp) - 1);
  char *addr_s = strtok(tmp, ":");
  char *size_s = strtok(NULL, ":");
  char *name_s = strtok(NULL, ":");
  if (!addr_s || !size_s || !name_s) {
    fprintf(stderr, "WARN: -w format: <hex_addr>:<bytes>:<name>\n");
    return;
  }
  watch_t *w = &watches[n_watches++];
  w->sram_addr = (uint32_t)strtol(addr_s, NULL, 0);
  w->size = (uint8_t)atoi(size_s);
  if (w->size < 1 || w->size > 4)
    w->size = 1;
  strncpy(w->name, name_s, sizeof(w->name) - 1);
}

static void add_sram_dump_arg(const char *arg) {
  if (n_sram_dumps >= MAX_SRAM_DUMPS) {
    fprintf(stderr, "WARN: too many sram dumps (max %d)\n", MAX_SRAM_DUMPS);
    return;
  }
  char tmp[64];
  strncpy(tmp, arg, sizeof(tmp) - 1);
  char *addr_s = strtok(tmp, ":");
  char *len_s = strtok(NULL, ":");
  if (!addr_s || !len_s) {
    fprintf(stderr, "WARN: --dump-sram format: <hex_addr>:<len>\n");
    return;
  }
  sram_dumps[n_sram_dumps].sram_addr = (uint32_t)strtol(addr_s, NULL, 0);
  sram_dumps[n_sram_dumps].length = (uint32_t)strtol(len_s, NULL, 0);
  n_sram_dumps++;
}

/* ═══════════════════════════════════════════════════════════════
   Custom IHEX loader (handles type-03/05 records gracefully)
   The pre-compiled libsimavr read_ihex_file() returns NULL when it
   encounters an Intel HEX record type 03 (Start Segment Address),
   which is present in real-world bootloader .hex files such as the
   Optiboot/simduino bootloader.  This replacement silently ignores
   types 03 and 05 and continues loading the data records.
   ═══════════════════════════════════════════════════════════════ */

/* Parse a 2-character hex nibble pair from src, advancing src by 2.
   Returns the byte value. */
static inline uint8_t ihex_byte(const char **src) {
  uint8_t hi = (uint8_t)(**src >= 'a' ? **src - 'a' + 10
                        : **src >= 'A' ? **src - 'A' + 10
                                       : **src - '0');
  (*src)++;
  uint8_t lo = (uint8_t)(**src >= 'a' ? **src - 'a' + 10
                        : **src >= 'A' ? **src - 'A' + 10
                                       : **src - '0');
  (*src)++;
  return (uint8_t)((hi << 4) | lo);
}

/*
 * protosim_read_ihex()
 *
 * Reads an Intel HEX file, loading all data records into a freshly
 * malloc'd buffer.  Supports:
 *   type 00 – data
 *   type 01 – end of file
 *   type 02 – extended segment address
 *   type 03 – start segment address (silently skipped)
 *   type 04 – extended linear address
 *   type 05 – start linear address (silently skipped)
 *
 * On success, *out_data is the buffer (caller must free()), *out_size
 * is the byte count, and *out_base is the lowest flash address seen.
 * Returns 0 on success, -1 on error.
 */
static int
protosim_read_ihex(const char *fname,
                   uint8_t   **out_data,
                   uint32_t   *out_size,
                   uint32_t   *out_base)
{
  FILE *f = fopen(fname, "r");
  if (!f) {
    perror(fname);
    return -1;
  }

  /* First pass: find address range so we can allocate one flat buffer. */
  uint32_t segment = 0;
  uint32_t lo = 0xFFFFFFFF, hi = 0;
  char line[544];

  while (fgets(line, sizeof(line) - 1, f)) {
    if (line[0] != ':') continue;
    const char *p = line + 1;
    uint8_t bytecount = ihex_byte(&p);
    uint8_t addrhi    = ihex_byte(&p);
    uint8_t addrlo    = ihex_byte(&p);
    uint8_t rectype   = ihex_byte(&p);
    uint32_t addr = segment | ((uint32_t)addrhi << 8) | addrlo;

    if (rectype == 0x00) {
      if (addr < lo) lo = addr;
      if (addr + bytecount > hi) hi = addr + bytecount;
    } else if (rectype == 0x01) {
      segment = 0;
    } else if (rectype == 0x02) {
      uint8_t s0 = ihex_byte(&p);
      uint8_t s1 = ihex_byte(&p);
      segment = ((uint32_t)s0 << 8 | s1) << 4;
    } else if (rectype == 0x04) {
      uint8_t s0 = ihex_byte(&p);
      uint8_t s1 = ihex_byte(&p);
      segment = ((uint32_t)s0 << 8 | s1) << 16;
    }
    /* types 03 and 05 – silently skip */
  }

  if (lo == 0xFFFFFFFF || hi == 0 || hi <= lo) {
    fclose(f);
    fprintf(stderr, "protosim_read_ihex: no data records found in %s\n", fname);
    return -1;
  }

  uint32_t buf_size = hi - lo;
  uint8_t *buf = (uint8_t *)calloc(1, buf_size);
  if (!buf) {
    fclose(f);
    return -1;
  }

  /* Second pass: load data. */
  rewind(f);
  segment = 0;
  while (fgets(line, sizeof(line) - 1, f)) {
    if (line[0] != ':') continue;
    const char *p = line + 1;
    uint8_t bytecount = ihex_byte(&p);
    uint8_t addrhi    = ihex_byte(&p);
    uint8_t addrlo    = ihex_byte(&p);
    uint8_t rectype   = ihex_byte(&p);
    uint32_t addr = segment | ((uint32_t)addrhi << 8) | addrlo;

    if (rectype == 0x00) {
      for (int i = 0; i < (int)bytecount; i++)
        buf[addr - lo + i] = ihex_byte(&p);
    } else if (rectype == 0x01) {
      segment = 0;
    } else if (rectype == 0x02) {
      uint8_t s0 = ihex_byte(&p);
      uint8_t s1 = ihex_byte(&p);
      segment = ((uint32_t)s0 << 8 | s1) << 4;
    } else if (rectype == 0x04) {
      uint8_t s0 = ihex_byte(&p);
      uint8_t s1 = ihex_byte(&p);
      segment = ((uint32_t)s0 << 8 | s1) << 16;
    }
    /* types 03 and 05 – silently skip */
  }
  fclose(f);

  *out_data = buf;
  *out_size = buf_size;
  *out_base = lo;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
  char *mmcu = "atmega328p";
  const char *filename = NULL;
  uint32_t freq = 16000000;
  int debug = 0;
  int verbose = 0;

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
        "                         The app firmware is still loaded at 0x0000.\n"
        "\nLLM Debug tools:\n"
        "  -b <addr|symbol>       Breakpoint at hex addr or ELF symbol "
        "(repeatable)\n"
        "  -w <addr:sz:name>      Watch SRAM variable at breakpoints "
        "(sz=1|2|4)\n"
        "  --dump-regs            Dump all 32 CPU registers at each "
        "breakpoint\n"
        "  --dump-sram <addr:len> Hex+ASCII SRAM dump at each breakpoint\n"
        "  --max-steps <n>        Exit after N steps (prevents hangs — always "
        "use!)\n"
        "  -t <n>                 Trace: print PC + watches every N cycles\n"
        "  -s                     Single-step: print PC for every instruction\n"
        "\nLLM Profiling tools (ELF only — symbol names required):\n"
        "  --coverage             Per-function code coverage report\n"
        "  --profile              Cycle-exact flat performance profile\n"
        "  --callgraph            Call graph with per-edge cycle costs + hot "
        "path\n"
        "  --profile-out <file>   Write profiling reports to file (default: "
        "stdout)\n"
        "\nExamples:\n"
        "  %s fw.elf --coverage --profile --callgraph --max-steps 2000000\n"
        "  %s fw.elf -b loop -w 0x200:2:cnt --dump-regs --max-steps 500000\n"
        "  %s fw.elf --bootloader optiboot.hex --max-steps 5000000\n",
        argv[0], argv[0], argv[0], argv[0]);
    exit(1);
  }

  filename = argv[1];

  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-m")) {
      if (i + 1 < argc)
        mmcu = argv[++i];
    } else if (!strcmp(argv[i], "-f")) {
      if (i + 1 < argc)
        freq = (uint32_t)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "-d")) {
      debug++;
    } else if (!strcmp(argv[i], "-v")) {
      verbose++;
    } else if (!strcmp(argv[i], "-b")) {
      if (i + 1 < argc)
        add_breakpoint_arg(argv[++i]);
    } else if (!strcmp(argv[i], "-w")) {
      if (i + 1 < argc)
        add_watch_arg(argv[++i]);
    } else if (!strcmp(argv[i], "--dump-regs")) {
      opt_dump_regs = 1;
    } else if (!strcmp(argv[i], "--dump-sram")) {
      if (i + 1 < argc)
        add_sram_dump_arg(argv[++i]);
    } else if (!strcmp(argv[i], "--max-steps")) {
      if (i + 1 < argc)
        opt_max_steps = atol(argv[++i]);
    } else if (!strcmp(argv[i], "-t")) {
      if (i + 1 < argc)
        opt_trace_every = atol(argv[++i]);
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
      if (i + 1 < argc)
        opt_profile_out = argv[++i];
    } else if (!strcmp(argv[i], "--bootloader")) {
      if (i + 1 < argc)
        opt_bootloader = argv[++i];
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
      fprintf(stderr, "%s: Cannot load HEX %s\n", argv[0], filename);
      exit(1);
    }
    printf("Loaded HEX: %u bytes at 0x%04x\n", hex_size, hex_base);
    if (opt_coverage || opt_profile || opt_callgraph)
      fprintf(
          stderr,
          "WARN: --coverage/--profile/--callgraph need ELF (symbol names).\n"
          "      HEX has no symbol table; reports will use raw addresses.\n");
  } else {
    if (elf_read_firmware(filename, &f) < 0) {
      fprintf(stderr, "%s: Cannot load ELF %s\n", argv[0], filename);
      exit(1);
    }
    if (f.mmcu[0])
      mmcu = f.mmcu;
    if (f.frequency)
      freq = f.frequency;
  }

  /* ── Resolve symbol breakpoints ──────────────────────────── */
  for (int i = 0; i < n_breakpoints; i++) {
    if (breakpoints[i].addr != UINT32_MAX)
      continue;
    if (is_hex) {
      fprintf(stderr, "WARN: symbol breakpoints need ELF, skipping '%s'\n",
              breakpoints[i].label);
      breakpoints[i] = breakpoints[--n_breakpoints];
      i--;
    } else {
      uint32_t a = resolve_symbol(&f, breakpoints[i].label);
      if (a == UINT32_MAX) {
        fprintf(stderr, "WARN: symbol '%s' not found — skipping\n",
                breakpoints[i].label);
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
    fprintf(stderr, "%s: Cannot create AVR core for %s\n", argv[0], mmcu);
    exit(1);
  }
  avr_init(avr);
  avr->frequency = freq;

  if (is_hex) {
    memcpy(avr->flash + hex_base, hex_data, hex_size);
    free(hex_data);
    avr->pc = hex_base;
    avr->codeend = avr->flashend;
  } else {
    avr_load_firmware(avr, &f);
  }

  /* ── Snapshot the app binary BEFORE bootloader overlay ─────── */
  /* We need the app binary to feed to the STK500 uploader thread.
   * The app occupies 0x0000 .. bl_region_start-1 once we know the
   * bootloader base.  For now snapshot the whole flash; the uploader
   * will send avr->codeend bytes starting at 0x0000. */
  uint8_t *stk_app_binary = NULL;
  size_t   stk_app_size   = 0;
  if (opt_bootloader) {
    /* Conservative: snapshot the full flash before overlay.  We will
     * trim to codeend if it is set. */
    stk_app_size   = (avr->codeend > 0) ? avr->codeend : (avr->flashend + 1);
    stk_app_binary = (uint8_t *)malloc(stk_app_size);
    if (!stk_app_binary) { perror("malloc stk_app_binary"); exit(1); }
    memcpy(stk_app_binary, avr->flash, stk_app_size);
    printf("[STK] Captured app binary: %zu bytes from flash\n",
           stk_app_size);
  }

  /* ── Bootloader overlay ──────────────────────────────────── */
  if (opt_bootloader) {
    int bl_is_hex = (strstr(opt_bootloader, ".hex")  != NULL ||
                     strstr(opt_bootloader, ".ihex") != NULL);
    uint32_t bl_size = 0, bl_base = 0;
    uint8_t *bl_data = NULL;
    uint32_t bl_entry = 0;

    if (bl_is_hex) {
      if (protosim_read_ihex(opt_bootloader, &bl_data, &bl_size, &bl_base) < 0
          || !bl_data) {
        fprintf(stderr, "ERROR: Cannot load bootloader HEX: %s\n",
                opt_bootloader);
        exit(1);
      }
      if (bl_base + bl_size > avr->flashend + 1) {
        fprintf(stderr,
                "ERROR: Bootloader HEX (0x%04x..0x%04x) exceeds flash "
                "(0x0000..0x%04x)\n",
                bl_base, bl_base + bl_size - 1, avr->flashend);
        free(bl_data);
        exit(1);
      }
      memcpy(avr->flash + bl_base, bl_data, bl_size);
      free(bl_data);
      bl_entry = bl_base; /* IHEX start record or base address */
      bl_region_start = bl_base;
      bl_region_end   = bl_base + bl_size;
      printf("[BL] Loaded bootloader HEX: %u bytes at 0x%04x\n", bl_size,
             bl_base);
    } else {
      /* ELF bootloader */
      elf_firmware_t bl_fw = {0};
      if (elf_read_firmware(opt_bootloader, &bl_fw) < 0) {
        fprintf(stderr, "ERROR: Cannot load bootloader ELF: %s\n",
                opt_bootloader);
        exit(1);
      }
      /* Temporarily create a scratch AVR to let avr_load_firmware write
       * the data, then copy just the flash bytes into our real AVR. */
      avr_t *tmp = avr_make_mcu_by_name(mmcu);
      avr_init(tmp);
      avr_load_firmware(tmp, &bl_fw);
      /* The ELF entry point is tmp->pc after load */
      bl_entry = tmp->pc;
      bl_region_start = bl_entry;
      bl_region_end   = tmp->flashend + 1;
      bl_size  = tmp->flashend + 1;
      memcpy(avr->flash, tmp->flash, bl_size);
      /* avr_make_mcu_by_name allocates; just leak the tiny struct — this
       * is a one-shot setup path and the program exits shortly anyway. */
      printf("[BL] Loaded bootloader ELF: %s  entry=0x%04x\n",
             opt_bootloader, bl_entry);
    }

    /* Redirect execution to the bootloader entry point */
    avr->pc = bl_entry;
    printf("[BL] Initial PC set to bootloader entry 0x%04x\n", bl_entry);
  }

  avr->log = 1 + verbose;

  if (debug) {
    avr->gdb_port = 1234;
    avr->state = cpu_Stopped;
    avr_gdb_init(avr);
    printf("GDB server listening on port 1234\n");
  }

  /* ── Build profiling tables ───────────────────────────────── */
  if (opt_coverage || opt_profile || opt_callgraph) {
    if (!is_hex)
      build_func_table(&f, avr->flashend);
    profiling_alloc(avr->flashend);
    printf("[PROF] Profiling enabled: coverage=%d profile=%d callgraph=%d  "
           "(%d functions mapped)\n",
           opt_coverage, opt_profile, opt_callgraph, n_funcs);
  }

  /* ── PTY/COM UART ────────────────────────────────────────────── */
#ifdef _WIN32
  uart_com_init(avr, &uart_com);
  uart_com_connect(&uart_com, '0');
#else
  uart_pty_init(avr, &uart_pty);
  uart_pty_connect(&uart_pty, '0');
#endif

  /* ── STK500 auto-upload (when --bootloader is active) ────────── */
  if (opt_bootloader && stk_app_binary) {
    static stk500_args_t stk_args;
#ifdef _WIN32
    stk500_args_init(&stk_args, stk_app_binary, stk_app_size,
                     uart_com.port.port);
#else
    stk500_args_init(&stk_args, stk_app_binary, stk_app_size, 0);
#endif
    free(stk_app_binary);
    stk_app_binary = NULL;
    stk500_start_upload_thread(&stk_args);
    printf("[STK] Auto-upload thread started (port %d)\n",
           stk_args.tcp_port);
  }

  printf("protosim running %s on %s at %u Hz\n", filename, mmcu,
         (unsigned)freq);

  if (n_breakpoints > 0) {
    printf("[DBG] %d breakpoint(s):\n", n_breakpoints);
    for (int i = 0; i < n_breakpoints; i++)
      printf("[DBG]   #%d  '%s'  PC=0x%04x\n", i, breakpoints[i].label,
             breakpoints[i].addr);
  }
  if (opt_max_steps)
    printf("[DBG] Will exit after %ld steps.\n", opt_max_steps);
  if (!n_breakpoints && !opt_single_step && !opt_trace_every && !opt_coverage &&
      !opt_profile && !opt_callgraph)
    printf("Press Ctrl+C to stop.\n");

  /* ══════════════════════════════════════════════════════════
     Main simulation loop
     ══════════════════════════════════════════════════════════ */
  long steps = 0;
  uint64_t last_trace_cyc = 0;

  while (1) {
    uint32_t pre_pc = avr->pc;
    uint64_t pre_cycle = avr->cycle;

    int state = avr_run(avr);

    /* Cycle-exact profiling — called for every instruction */
    if (opt_coverage || opt_profile || opt_callgraph)
      profiling_step(pre_pc, pre_cycle, avr->pc, avr->cycle);

    if (state == cpu_Done || state == cpu_Crashed) {
      printf("[DBG] CPU %s at PC=0x%04x after %ld steps\n",
             state == cpu_Done ? "DONE" : "CRASHED", avr->pc, steps);
      break;
    }

    /* Only count steps executed inside the app (not the bootloader) */
    if (bl_region_end == 0 ||
        pre_pc < bl_region_start || pre_pc >= bl_region_end)
      steps++;

    /* Max-steps guard */
    if (opt_max_steps && steps >= opt_max_steps) {
      printf("[DBG] MAX-STEPS (%ld) reached — stopping.\n", opt_max_steps);
      printf("[DBG] Final PC=0x%04x  cycle=%" PRIu64 "\n", avr->pc, avr->cycle);
      if (opt_dump_regs)
        dump_registers(avr);
      dump_watches(avr);
      for (int i = 0; i < n_sram_dumps; i++)
        dump_sram_range(avr, sram_dumps[i].sram_addr, sram_dumps[i].length);
      break;
    }

    /* Single-step trace */
    if (opt_single_step) {
      printf("[DBG] STEP  PC=0x%04x  cycle=%" PRIu64 "\n", avr->pc, avr->cycle);
      dump_watches(avr);
      fflush(stdout);
    }

    /* Periodic cycle trace */
    if (opt_trace_every &&
        (avr->cycle - last_trace_cyc) >= (uint64_t)opt_trace_every) {
      last_trace_cyc = avr->cycle;
      printf("[DBG] TRACE PC=0x%04x  cycle=%" PRIu64 "\n", avr->pc, avr->cycle);
      if (opt_dump_regs)
        dump_registers(avr);
      dump_watches(avr);
      fflush(stdout);
    }

    /* Breakpoint check */
    for (int i = 0; i < n_breakpoints; i++) {
      if (avr->pc == breakpoints[i].addr) {
        breakpoints[i].hit_count++;
        on_breakpoint_hit(avr, breakpoints[i].label);
      }
    }
  }

  /* ── Debug summary ───────────────────────────────────────── */
  printf("[DBG] === Simulation Summary ===\n");
  printf("[DBG]   Total steps : %ld\n", steps);
  printf("[DBG]   Total cycles: %" PRIu64 "\n", avr->cycle);
  for (int i = 0; i < n_breakpoints; i++)
    printf("[DBG]   BP '%s' hit %" PRIu64 " time(s)\n", breakpoints[i].label,
           breakpoints[i].hit_count);
  printf("[DBG] ==============================\n");

  /* ── Profiling reports ───────────────────────────────────── */
  if (opt_coverage || opt_profile || opt_callgraph) {
    finalize_call_stack(avr->cycle);
    FILE *pout = stdout;
    if (opt_profile_out) {
      pout = fopen(opt_profile_out, "w");
      if (!pout) {
        perror("Cannot open --profile-out file");
        pout = stdout;
      }
    }

    if (opt_coverage)
      report_coverage(pout, avr->cycle, freq);
    if (opt_profile)
      report_profile(pout, avr->cycle, freq);
    if (opt_callgraph)
      report_callgraph(pout, avr->cycle, freq);

    if (pout != stdout) {
      fclose(pout);
      printf("[PROF] Reports written to: %s\n", opt_profile_out);
    }

    free(coverage_map);
    free(pc_cycles);
  }

  return 0;
}
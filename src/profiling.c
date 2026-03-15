#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "profiling.h"
#include "protosim_core.h"

/* ═══════════════════════════════════════════════════════════════
   Profiling structures (internal to this module)
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
   Globals (Profiling)
   ═══════════════════════════════════════════════════════════════ */

int opt_coverage = 0;
int opt_profile = 0;
int opt_callgraph = 0;
const char *opt_profile_out = NULL;

static uint8_t *coverage_map = NULL;
static uint64_t *pc_cycles = NULL;
static func_profile_t func_table[MAX_FUNCTIONS];
int n_funcs = 0;
static cg_edge_t cg_edges[MAX_CG_EDGES];
static int n_cg_edges = 0;
static call_frame_t call_stack[MAX_CALL_DEPTH];
static int call_depth = 0;

/* ═══════════════════════════════════════════════════════════════
   Internal Helpers
   ═══════════════════════════════════════════════════════════════ */

static int pc_to_func(uint32_t pc) {
  for (int i = 0; i < n_funcs; i++)
    if (pc >= func_table[i].start && pc < func_table[i].end)
      return i;
  return -1;
}

static int cmp_sym_addr(const void *a, const void *b) {
  const avr_symbol_t *sa = *(const avr_symbol_t **)a;
  const avr_symbol_t *sb = *(const avr_symbol_t **)b;
  return (sa->addr > sb->addr) - (sa->addr < sb->addr);
}

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
   Public API
   ═══════════════════════════════════════════════════════════════ */

void build_func_table(elf_firmware_t *f, uint32_t flashend) {
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

void profiling_alloc(uint32_t flashend) {
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

void profiling_step(uint32_t pre_pc, uint64_t pre_cycle,
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

void finalize_call_stack(uint64_t post_cycle) {
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
        edge->total_cycles += frame_cycles;
      }
    }
  }
}

void finalize_self_cycles(void) {
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

void report_coverage(FILE *out, uint64_t total_cycles, uint32_t freq) {
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

void report_profile(FILE *out, uint64_t total_cycles, uint32_t freq) {
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

static void print_cg_subtree(FILE *out, int fi, int depth,
                             uint64_t total_cycles, uint8_t *visited,
                             int max_depth) {
  if (depth > max_depth || fi < 0 || fi >= n_funcs)
    return;
  if (visited[fi])
    return;
  visited[fi] = 1;

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

void report_callgraph(FILE *out, uint64_t total_cycles, uint32_t freq) {
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
        "[CG]  (no call graph data — no matched CALL/RET pairs in this run)\n");
    return;
  }

  cg_edge_t sorted[MAX_CG_EDGES];
  memcpy(sorted, cg_edges, n_cg_edges * sizeof(cg_edge_t));
  qsort(sorted, n_cg_edges, sizeof(cg_edge_t), cmp_edge_cycles_desc);

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

  fprintf(out, "[CG]\n[CG]  Call Tree:\n");
  fprintf(out, "[CG]   %-28s  %7s  %14s  %7s  %10s\n", "Function", "Calls",
          "Total Cycles", "% Time", "Avg Cyc");
  fprintf(out, "[CG]   %-28s  %7s  %14s  %7s  %10s\n",
          "----------------------------", "-------", "--------------",
          "-------", "----------");

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

  fprintf(out, "[CG]\n[CG]  Hot Path (highest-cost execution chain):\n[CG]  ");
  if (n_cg_edges > 0) {
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

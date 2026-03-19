#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "debug.h"
#include "protosim_core.h"
#include "avr_ioport.h"

typedef struct {
  uint32_t sram_addr;
  uint8_t size;
  char name[32];
} watch_t;

typedef struct {
  uint32_t sram_addr;
  uint32_t length;
} sram_dump_t;

breakpoint_t breakpoints[MAX_BREAKPOINTS];
int n_breakpoints = 0;

static watch_t watches[MAX_WATCHES];
static int n_watches = 0;

static sram_dump_t sram_dumps[MAX_SRAM_DUMPS];
int n_sram_dumps = 0;

static const char sreg_bits[] = "CZNVSHTI";

static void print_sreg(uint8_t sreg) {
  printf("  SREG: ");
  for (int i = 7; i >= 0; i--)
    printf("%c:%d ", sreg_bits[i], (sreg >> i) & 1);
  printf("\n");
}

void dump_registers(avr_t *a) {
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

void dump_watches(avr_t *a) {
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

void dump_sram_range(avr_t *a, uint32_t base, uint32_t len) {
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

void on_breakpoint_hit(avr_t *a, const char *label) {
  printf("[DBG] BREAKPOINT HIT: %s  PC=0x%04x  cycle=%" PRIu64 "\n", label,
         a->pc, a->cycle);
  if (opt_dump_regs)
    dump_registers(a);
  dump_watches(a);
  for (int i = 0; i < n_sram_dumps; i++)
    dump_sram_range(a, sram_dumps[i].sram_addr, sram_dumps[i].length);
  fflush(stdout);
}

void add_breakpoint_arg(const char *arg) {
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

void add_watch_arg(const char *arg) {
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

void add_sram_dump_arg(const char *arg) {
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

uint32_t resolve_symbol(elf_firmware_t *f, const char *sym) {
  if (!f) return UINT32_MAX;
  
  int is_wildcard = (strchr(sym, '*') != NULL);
  char stripped_sym[128] = {0};
  
  if (is_wildcard) {
    int j = 0;
    for (int i = 0; sym[i] && j < sizeof(stripped_sym) - 1; i++) {
      if (sym[i] != '*') stripped_sym[j++] = sym[i];
    }
  }

  for (int i = 0; i < (int)f->symbolcount; i++) {
    if (is_wildcard) {
      if (strstr(f->symbol[i]->symbol, stripped_sym) != NULL) {
        return f->symbol[i]->addr;
      }
    } else {
      if (strcmp(f->symbol[i]->symbol, sym) == 0) {
        return f->symbol[i]->addr;
      }
    }
  }
  return UINT32_MAX;
}

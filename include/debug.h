#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "protosim_core.h"

typedef struct {
  uint32_t addr;
  char label[64];
  uint64_t hit_count;
} breakpoint_t;

extern breakpoint_t breakpoints[MAX_BREAKPOINTS];
extern int n_breakpoints;

void add_breakpoint_arg(const char *arg);
void add_watch_arg(const char *arg);
void add_sram_dump_arg(const char *arg);

void dump_registers(avr_t *a);
void dump_watches(avr_t *a);
void dump_sram_range(avr_t *a, uint32_t base, uint32_t len);
void on_breakpoint_hit(avr_t *a, const char *label);

uint32_t resolve_symbol(elf_firmware_t *f, const char *sym);

#endif /* DEBUG_H */

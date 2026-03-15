#ifndef PROFILING_H
#define PROFILING_H

#include <stdint.h>
#include <stdio.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "protosim_core.h"

extern int opt_coverage;
extern int opt_profile;
extern int opt_callgraph;
extern const char *opt_profile_out;
extern int n_funcs;

void build_func_table(elf_firmware_t *f, uint32_t flashend);
void profiling_alloc(uint32_t flashend);
void profiling_step(uint32_t pre_pc, uint64_t pre_cycle, uint32_t post_pc, uint64_t post_cycle);
void finalize_call_stack(uint64_t post_cycle);
void finalize_self_cycles(void);

void report_coverage(FILE *out, uint64_t total_cycles, uint32_t freq);
void report_profile(FILE *out, uint64_t total_cycles, uint32_t freq);
void report_callgraph(FILE *out, uint64_t total_cycles, uint32_t freq);

#endif /* PROFILING_H */

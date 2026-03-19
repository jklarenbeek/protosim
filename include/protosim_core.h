#ifndef PROTOSIM_CORE_H
#define PROTOSIM_CORE_H

#include <stdint.h>
#include <stdio.h>
#include "sim_avr.h"

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
   Globals (declared extern for modules)
   ═══════════════════════════════════════════════════════════════ */

extern avr_t *avr;
extern int verbose;
extern int opt_dump_regs;
extern int opt_single_step;
extern long opt_max_steps;
extern long opt_trace_every;

extern char *opt_uart0_in;
extern char *opt_uart0_out;
extern char *opt_exit_on_uart;
extern int opt_wait_tcp;

/* ═══════════════════════════════════════════════════════════════
   Opcode Utilities (Shared by main loop and profiling)
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

#endif /* PROTOSIM_CORE_H */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ihex_loader.h"

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

int protosim_read_ihex(const char *fname,
                   uint8_t   **out_data,
                   uint32_t   *out_size,
                   uint32_t   *out_base)
{
  FILE *f = fopen(fname, "r");
  if (!f) {
    perror(fname);
    return -1;
  }

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
  }
  fclose(f);

  *out_data = buf;
  *out_size = buf_size;
  *out_base = lo;
  return 0;
}

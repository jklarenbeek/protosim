#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "ihex_loader.h"
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    uint32_t size = 0, base = 0; uint8_t *d = NULL;
    int rc = protosim_read_ihex(argv[i], &d, &size, &base);
    printf("%-13s -> return=%d base=0x%x size=%u", argv[i], rc, base, size);
    if (d && size) printf(" first=0x%02x last=0x%02x", d[0], d[size - 1]);
    printf("\n");
  }
  return 0;
}

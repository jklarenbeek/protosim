#ifndef IHEX_LOADER_H
#define IHEX_LOADER_H

#include <stdint.h>

int protosim_read_ihex(const char *fname,
                       uint8_t   **out_data,
                       uint32_t   *out_size,
                       uint32_t   *out_base);

#endif /* IHEX_LOADER_H */

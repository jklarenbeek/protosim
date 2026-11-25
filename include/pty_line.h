#ifndef PTY_LINE_H
#define PTY_LINE_H

#include <stdint.h>

/* A pseudo terminal for one UART, linked at /tmp/simavr-uart<uart>.
   Read and write without blocking, from the simulation loop. */
int pty_line_open(char uart);
int pty_line_read(uint8_t *byte); /* 1 when a byte was there */
void pty_line_write(uint8_t byte);
void pty_line_close(void);

#endif /* PTY_LINE_H */

/*
    pty_line.c
    A pseudo terminal for a UART of a backend that has no PTY bridge of its
    own (Bramble). The master side is read and written without blocking
    from the simulation loop; the slave is linked at /tmp/simavr-uart0 like
    the simavr bridge, so the same tools find it.
 */

#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "pty_line.h"

static int master = -1, slave = -1;

int pty_line_open(char uart) {
  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
    fprintf(stderr, "ERROR: cannot create a pseudo terminal: %s\n", strerror(errno));
    return -1;
  }
  const char *name = ptsname(master);
  /* kept open here, so that the master side does not see a hang-up while
     nobody else has the line open */
  slave = open(name, O_RDWR | O_NOCTTY);
  if (slave >= 0) {
    struct termios t;
    if (tcgetattr(slave, &t) == 0) {
      cfmakeraw(&t);
      tcsetattr(slave, TCSANOW, &t);
    }
  }
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

  char link[64];
  snprintf(link, sizeof(link), "/tmp/simavr-uart%c", uart);
  unlink(link);
  printf("pty_line_open %s on port *** %s ***\n", "bridge", name);
  if (symlink(name, link) != 0) {
    fprintf(stderr, "WARN %s: Can't create %s: %s", __func__, link, strerror(errno));
  } else {
    printf("%s: %s now points to %s\n", __func__, link, name);
    printf("\nTo connect:\n");
    printf("  picocom -b 115200 %s\n\n", name);
  }
  return 0;
}

int pty_line_read(uint8_t *byte) {
  if (master < 0) return 0;
  return read(master, byte, 1) == 1;
}

void pty_line_write(uint8_t byte) {
  if (master < 0) return;
  /* nobody reads: what does not fit is lost, as on a line nobody listens to */
  if (write(master, &byte, 1) < 0) return;
}

void pty_line_close(void) {
  if (slave >= 0) close(slave);
  if (master >= 0) close(master);
  master = slave = -1;
}

/*
 * win32_compat.h
 *
 * POSIX compatibility shim for compiling simavr on Windows 11 with
 * MSYS2 UCRT64 MinGW toolchain.
 *
 * INJECTED via: gcc -include include/win32_compat.h
 * This means it runs BEFORE the source file's own includes.
 *
 * DESIGN PRINCIPLE: MSYS2 UCRT64 already provides most POSIX functions
 * (usleep, gettimeofday, getpid, sys/time.h, unistd.h, pthread.h via
 * winpthreads, etc.). We ONLY shim what is truly missing or broken.
 *
 * What we fix:
 *   1. close() on SOCKET fds in sim_gdb.c → closesocket()
 *      (only when compiled with -DSIM_GDB_COMPILE)
 *   2. CLOCK_MONOTONIC_RAW — suppress so sim_avr.c uses gettimeofday path
 *   3. ssize_t — ensure it's defined
 *   4. basename() / libgen.h — not in MinGW, used by protosim.c
 *
 * Copyright (c) protosim project — MIT licence
 */

#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#ifdef __MINGW32__

/*
 * ── 1. Winsock must be included BEFORE windows.h ────────────────────────────
 * sim_network.h already does this inside its __MINGW32__ block.
 * We mirror it here so that our close() shim for sim_gdb.c has access
 * to closesocket() without including sim_network.h directly.
 */
#ifndef _WINSOCK2API_
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>

/*
 * ── 2. close() disambiguation ────────────────────────────────────────────────
 *
 * Problem:
 *   sim_gdb.c uses  close(socket_fd)  → needs closesocket()
 *   sim_elf.c uses  close(file_fd)    → needs _close() / close() from <io.h>
 *
 * Solution:
 *   Compile sim_gdb.c with -DSIM_GDB_COMPILE (handled in build-simavr-win.bat).
 *   When that macro is set, we remap close() → closesocket().
 *   In all other files, we leave close() alone (MSYS2 provides it via <io.h>
 *   or as a POSIX alias in ucrt64).
 */
#ifdef SIM_GDB_COMPILE
#include <io.h>
static inline int _win32_closesocket(int fd) {
  if (fd < 0)
    return 0;
  return closesocket((SOCKET)(uintptr_t)(unsigned int)fd);
}
#ifdef close
#undef close
#endif
#define close(fd) _win32_closesocket(fd)
#endif /* SIM_GDB_COMPILE */

/*
 * ── 3. CLOCK_MONOTONIC_RAW ───────────────────────────────────────────────────
 *
 * sim_avr.c uses:
 *   #ifndef CLOCK_MONOTONIC_RAW
 *     // use gettimeofday()  ← this branch works on Windows
 *   #else
 *     // use clock_gettime(CLOCK_MONOTONIC_RAW, ...)
 *   #endif
 *
 * CLOCK_MONOTONIC_RAW is Linux-specific. Ensure we take the gettimeofday
 * branch by making sure it's not defined.
 */
#ifdef CLOCK_MONOTONIC_RAW
#undef CLOCK_MONOTONIC_RAW
#endif

/*
 * ── 4. ssize_t ───────────────────────────────────────────────────────────────
 * MSYS2 UCRT64 provides ssize_t but the guard may not be active in all modes.
 */
#include <sys/types.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

/*
 * ── 5. basename() / libgen.h ─────────────────────────────────────────────────
 *
 * protosim.c includes <libgen.h> for basename().
 * MSYS2 MinGW does NOT have <libgen.h>.
 * Provide a simple inline implementation.
 */
#ifndef HAVE_BASENAME
#define HAVE_BASENAME
#include <string.h>
static inline char *basename(char *path) {
  if (!path || path[0] == '\0')
    return (char *)".";
  size_t len = strlen(path);
  char *p = path + len;
  /* strip trailing slashes */
  while (p > path && (p[-1] == '/' || p[-1] == '\\'))
    --p;
  if (p == path)
    return path; /* root-only path */
  /* walk back to previous separator */
  while (p > path && p[-1] != '/' && p[-1] != '\\')
    --p;
  return p;
}
#endif /* HAVE_BASENAME */

#endif /* __MINGW32__ */

#endif /* WIN32_COMPAT_H */

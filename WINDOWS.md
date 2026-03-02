# protosim on Windows 11 — Setup Guide

Built and tested with **MSYS2 UCRT64 + MinGW** on Windows 11.
simavr's source code is **not modified** — a single compatibility header is injected at compile time.

---

## Quick reference

| Component | Path |
|-----------|------|
| MSYS2 root | `C:\Tools\msys64` |
| gcc / make | `C:\Tools\msys64\ucrt64\bin` |
| libsimavr.a | `libraries\simavr\simavr\obj-x86_64-w64-mingw32\libsimavr.a` |
| Win32 shim | `include\win32_compat.h` |
| Build script | `scripts\build-simavr-win.bat` |
| Binary | `bin\protosim.exe` |

---

## Step 1 — Install MSYS2

Download and run the installer from [msys2.org](https://www.msys2.org/). Install to **`C:\Tools\msys64`**.

Open the **UCRT64** shell and install dependencies:

```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-make \
          mingw-w64-ucrt-x86_64-libelf \
          mingw-w64-ucrt-x86_64-python
```

> [!IMPORTANT]
> Use the **UCRT64** variant — not MINGW64 or MSYS. The shell shortcut is "MSYS2 UCRT64".

---

## Step 2 — Clone and initialise the repo

Open a normal Windows Command Prompt or PowerShell (not the MSYS2 shell):

```bat
git clone https://github.com/jklarenbeek/protosim.git
cd protosim
git submodule update --init libraries/simavr
```

---

## Step 3 — Build libsimavr.a

```bat
scripts\build-simavr-win.bat
```

Expected output:

```
==========================================
 Building simavr core on Windows...
==========================================
Generating sim core headers...
[1/3] Compiling sim/*.c files...  (13 files + sim_gdb.c)
[2/3] Compiling avr/*.c files...  (15 files)
[3/3] Compiling cores/sim_*.c ... (40 AVR MCU cores)
Archiving libsimavr.a...
==========================================
 SUCCESS: simavr built
 > libraries\simavr\simavr\obj-x86_64-w64-mingw32\libsimavr.a
==========================================
```

---

## Step 4 — Build protosim.exe

Add the UCRT64 bin directory to your PATH (needed for `mingw32-make` and `gcc`):

```bat
set PATH=C:\Tools\msys64\ucrt64\bin;%PATH%
mingw32-make
```

Expected output:

```
gcc ... -o bin/protosim.exe src/protosim.c src/uart_com.c ...
```

Verify:

```bat
bin\protosim.exe
```

This prints the usage banner and exits.

---

## Step 5 — First run

```bat
bin\protosim.exe examples\demo\demo.elf -m atmega328p -f 16000000 --max-steps 50000
```

Expected:

```
uart_com_init listening on TCP 127.0.0.1:4000
uart_com_connect: UART0 is connected to TCP port 4000

To connect:
  putty.exe -telnet 127.0.0.1 4000

protosim running examples\demo\demo.elf on atmega328p at 16000000 Hz
[DBG] Will exit after 50000 steps.
[DBG] MAX-STEPS (50000) reached — stopping.
[DBG] Final PC=0x00b4  cycle=83389
[DBG] === Simulation Summary ===
[DBG]   Total steps : 50000
[DBG]   Total cycles: 83389
[DBG] ==============================
```

---

## How the Windows compatibility works

simavr is written for Linux and uses several POSIX APIs that do not exist — or behave differently — on Windows. Rather than patching simavr's source, protosim injects a single compatibility header into every simavr compile unit via `gcc -include`:

```
gcc -include include/win32_compat.h -c simavr/sim/sim_avr.c ...
```

### What `include/win32_compat.h` shims

| # | Problem | Fix |
|---|---------|-----|
| 1 | `sim_gdb.c` calls `close(socket_fd)` | `close()` → `closesocket()` — only active when compiled with `-DSIM_GDB_COMPILE` |
| 2 | `sim_avr.c` uses `clock_gettime(CLOCK_MONOTONIC_RAW, …)` | `#undef CLOCK_MONOTONIC_RAW` so it falls into the `gettimeofday()` branch (already provided by MSYS2 UCRT64) |
| 3 | `ssize_t` may not be defined in all MSYS2 build modes | Typedef guard: `typedef long ssize_t` under `_SSIZE_T_DEFINED` |
| 4 | `protosim.c` includes `<libgen.h>` for `basename()` — not available in MinGW | Inline `basename()` implementation |

MSYS2 UCRT64 already provides everything else simavr needs: `usleep()`, `gettimeofday()`, `pthread.h` (via winpthreads), `sys/time.h`, `unistd.h`. Far less shimming was required than expected.

### `close()` disambiguation detail

`sim_gdb.c` calls `close()` on **socket** file descriptors — this must map to `closesocket()`.
Other simavr files call `close()` on **regular file** descriptors — this must stay as POSIX `close()`.

The build script compiles `sim_gdb.c` separately with `-DSIM_GDB_COMPILE`:

```bat
gcc %CFLAGS% -DSIM_GDB_COMPILE -c sim_gdb.c -o sim_gdb.o
```

The shim activates the `close()` → `closesocket()` remap only when that macro is defined, leaving all other files unaffected.

### Windows UART

Linux protosim opens a PTY (`/dev/pts/N`). That does not exist on Windows. Instead, protosim uses `src/uart_com.c` which starts a Winsock TCP listener on `127.0.0.1:4000`. The Makefile selects the right source file automatically:

```makefile
ifeq ($(OS),Windows_NT)
    SRCS = src/protosim.c src/uart_com.c
else
    SRCS = src/protosim.c src/uart_pty.c
endif
```

Connect to the virtual UART from a second terminal:

```bat
putty.exe -telnet 127.0.0.1 4000
```

---

## Compiling test firmware (optional)

To compile the demo firmware included in `examples/demo/`:

```bat
rem Using PlatformIO's bundled avr-gcc:
set AVRGCC=%USERPROFILE%\.platformio\packages\toolchain-atmelavr\bin\avr-gcc.exe
%AVRGCC% -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o examples\demo\demo.elf examples\demo\demo.c

rem Or avr-gcc from https://www.tindie.com/products/tinyavr/avr-gcc-for-windows/:
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o examples\demo\demo.elf examples\demo\demo.c
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `gcc not found` in `build-simavr-win.bat` | Run `set PATH=C:\Tools\msys64\ucrt64\bin;%PATH%` first |
| `libsimavr.a not found` when running `mingw32-make` | Run `scripts\build-simavr-win.bat` first |
| `avr-gcc` not found | Install from PlatformIO (`pio pkg install -g -t toolchain-atmelavr`) or WinAVR |
| MSYS2 wrong variant | Use UCRT64 shell — not MINGW64 |
| `undefined reference to pthread_*` | Add `-lpthread` (already in Makefile `LDFLAGS`) |
| `undefined reference to elf_*` | Add `-lelf` and `-LC:/Tools/msys64/ucrt64/lib` (already in Makefile) |

# protosim Makefile

CC = gcc
CFLAGS = -Wall -O2 -I./include -I./libraries/simavr/simavr/sim -I./libraries/simavr/simavr/sim/avr

COMMON_SRCS = src/backend_avr.c src/stk500_uploader.c src/debug.c src/profiling.c src/ihex_loader.c src/reset.c src/stimulus.c

ifeq ($(OS),Windows_NT)
    SRCS = src/protosim.c src/uart_com.c $(COMMON_SRCS)
    # Windows build: inject win32_compat.h into every compile unit (-include)
    # and link against libsimavr.a built by build-simavr-win.bat
    CFLAGS += -include ./include/win32_compat.h \
              -DPROTOSIM_BUILD=1 \
              -I$(CURDIR)/libraries/simavr/simavr/sim \
              -I$(CURDIR)/libraries/simavr/simavr/sim/avr \
              -I$(CURDIR)/include
    LDFLAGS = ./libraries/simavr/simavr/obj-x86_64-w64-mingw32/libsimavr.a \
              -lws2_32 \
              -lpthread \
              -lelf \
              -LC:/Tools/msys64/ucrt64/lib
    TARGET = bin/protosim.exe
else
    SRCS = src/protosim.c src/uart_pty.c $(COMMON_SRCS)
    LDFLAGS = ./libraries/simavr/simavr/obj-x86_64-linux-gnu/libsimavr.a -lpthread -lelf
    TARGET = bin/protosim
endif

# Bramble, for the RP2040 and RP2350: its sources without its main(), built
# here. On by default where the submodule is checked out (not on Windows);
# BRAMBLE=0 leaves it out.
BRAMBLE_DIR = libraries/bramble
ifeq ($(OS),Windows_NT)
    BRAMBLE ?= 0
else
    BRAMBLE ?= $(if $(wildcard $(BRAMBLE_DIR)/src/cpu.c),1,0)
endif
ifeq ($(BRAMBLE),1)
    BRAMBLE_SRCS = $(filter-out $(BRAMBLE_DIR)/src/main.c,$(wildcard $(BRAMBLE_DIR)/src/*.c $(BRAMBLE_DIR)/src/rp2350_rv/*.c $(BRAMBLE_DIR)/src/rp2350_arm/*.c))
    BRAMBLE_OBJS = $(patsubst $(BRAMBLE_DIR)/src/%.c,build/bramble/%.o,$(BRAMBLE_SRCS))
    BRAMBLE_INC = -I$(BRAMBLE_DIR)/include -I$(BRAMBLE_DIR)/include/rp2350_rv -I$(BRAMBLE_DIR)/include/rp2350_arm
    BRAMBLE_CFLAGS = -O2 -std=gnu99 $(BRAMBLE_INC)
    CFLAGS += -DPROTOSIM_BRAMBLE
    SRCS += src/backend_bramble.c src/pty_line.c
    LDFLAGS += -lm
endif

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

# BRAMBLE changes what protosim.c is built with
$(OBJS) $(BRAMBLE_OBJS): Makefile

$(TARGET): $(OBJS) $(BRAMBLE_OBJS) check_lib
ifeq ($(OS),Windows_NT)
	if not exist bin mkdir bin
else
	mkdir -p bin
endif
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(BRAMBLE_OBJS) $(LDFLAGS)

ifeq ($(OS),Windows_NT)
# Windows: use cmd.exe mkdir
bin:
	if not exist bin mkdir bin
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# the backend sees Bramble's headers, not simavr's
src/backend_bramble.o: src/backend_bramble.c include/sim.h include/pty_line.h
	$(CC) -Wall -O2 -I./include $(BRAMBLE_INC) -DPROTOSIM_BRAMBLE -c $< -o $@

# a UART write of Bramble passes through the backend first (backend_bramble.c)
build/bramble/uart.o: $(BRAMBLE_DIR)/src/uart.c
	@mkdir -p $(dir $@)
	$(CC) $(BRAMBLE_CFLAGS) -Duart_write32=bramble_uart_write32_hw -Duart_read32=bramble_uart_read32_hw -c $< -o $@

# a spinlock is released as on the chip, without an event (backend_bramble.c)
build/bramble/cpu.o: $(BRAMBLE_DIR)/src/cpu.c
	@mkdir -p $(dir $@)
	$(CC) $(BRAMBLE_CFLAGS) -Dspinlock_release=bramble_spinlock_release_hw -c $< -o $@

# the RP2350 TIMER0 reaches the RP2040 timer at its own offsets (backend_bramble.c)
build/bramble/rp2350_rv/rp2350_periph.o: $(BRAMBLE_DIR)/src/rp2350_rv/rp2350_periph.c
	@mkdir -p $(dir $@)
	$(CC) $(BRAMBLE_CFLAGS) -Dtimer_read32=rp2350_timer0_read32 -Dtimer_write32=rp2350_timer0_write32 -c $< -o $@

build/bramble/%.o: $(BRAMBLE_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BRAMBLE_CFLAGS) -c $< -o $@

check_lib:
ifeq ($(OS),Windows_NT)
	@if not exist ".\libraries\simavr\simavr\obj-x86_64-w64-mingw32\libsimavr.a" ( \
		echo Error: libsimavr.a not found. Run: scripts\build-simavr-win.bat & \
		exit 1 )
else
	@if [ ! -f ./libraries/simavr/simavr/obj-x86_64-linux-gnu/libsimavr.a ]; then \
		echo "Error: libsimavr.a not found. Run setup script first."; \
		exit 1; \
	fi
endif

clean:
ifeq ($(OS),Windows_NT)
	-del /Q src\*.o bin\protosim.exe 2>nul
	@echo Cleanup complete.
else
	rm -f src/*.o $(TARGET)
	rm -rf build/bramble
	rm -rf *.elf *.hex
	@echo "Cleanup complete."
endif

.PHONY: all clean check_lib compiledb

compiledb:
ifeq ($(OS),Windows_NT)
	powershell -ExecutionPolicy Bypass -File scripts\generate_compdb.ps1
else
	bash scripts/generate_compdb.sh
endif

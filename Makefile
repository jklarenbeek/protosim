# protosim Makefile

CC = gcc
CFLAGS = -Wall -O2 -I./include -I./libraries/simavr/simavr/sim -I./libraries/simavr/simavr/sim/avr

ifeq ($(OS),Windows_NT)
    SRCS = src/protosim.c src/uart_com.c
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
    SRCS = src/protosim.c src/uart_pty.c
    LDFLAGS = ./libraries/simavr/simavr/obj-x86_64-linux-gnu/libsimavr.a -lpthread -lelf
    TARGET = bin/protosim
endif

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) check_lib
	@if not exist bin mkdir bin 2>nul || mkdir -p bin 2>/dev/null; true
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

ifeq ($(OS),Windows_NT)
# Windows: use cmd.exe mkdir
bin:
	if not exist bin mkdir bin
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -rf *.elf *.hex
	@echo "Cleanup complete."
endif

.PHONY: all clean check_lib

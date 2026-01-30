# protosim Makefile

CC = gcc
CFLAGS = -Wall -O2 -I./include -I./libraries/simavr/simavr/sim -I./libraries/simavr/simavr/sim/avr
LDFLAGS = ./libraries/simavr/simavr/obj-x86_64-linux-gnu/libsimavr.a -lpthread

TARGET = bin/protosim
SRCS = src/protosim.c src/uart_pty.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) check_lib
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

check_lib:
	@if [ ! -f ./libraries/simavr/simavr/obj-x86_64-linux-gnu/libsimavr.a ]; then \
		echo "Error: libsimavr not found. Run ./scripts/setup-simavr.sh first."; \
		exit 1; \
	fi

clean:
	rm -f src/*.o $(TARGET) \
  rm -rf *.elf *.hex \
  rm -rf .pio firmware/* \
  @echo "Cleanup complete."

.PHONY: all clean check_lib

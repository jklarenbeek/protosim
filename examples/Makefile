# Examples Makefile
CC = avr-gcc
OBJCOPY = avr-objcopy
MCU = atmega328p
F_CPU = 16000000UL
CFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -g -Wall

EXAMPLES = demo uart_echo struct_watch deep_callstack uart_interrupt

all: $(EXAMPLES)

$(EXAMPLES):
	$(CC) $(CFLAGS) -o $@/$@.elf $@/$@.c
	$(OBJCOPY) -O ihex -R .eeprom $@/$@.elf $@/$@.hex

clean:
	$(foreach dir, $(EXAMPLES), rm -f $(dir)/$(dir).elf $(dir)/$(dir).hex;)

.PHONY: all clean $(EXAMPLES)

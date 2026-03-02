CC      = avr-gcc
OBJCOPY = avr-objcopy
MCU     = atmega328p
FREQ    = 16000000UL
CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(FREQ) -Os -g -Wall

TARGET = demo
SRCS   = demo.c

all: $(TARGET).elf $(TARGET).hex

$(TARGET).elf: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

clean:
	rm -f *.elf *.hex *.o

.PHONY: all clean

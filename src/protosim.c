/*
	protosim.c
    A simavr runner for Protoduino with PTY UART support.
 */

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <pthread.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "sim_elf.h"
#include "sim_hex.h"
#include "sim_gdb.h"
#include "uart_pty.h"

uart_pty_t uart_pty;
avr_t * avr = NULL;

int main(int argc, char *argv[])
{
	char * mmcu = "atmega328p";
    const char * filename = NULL;
	uint32_t freq = 16000000;
	int debug = 0;
	int verbose = 0;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <firmware.elf|hex> [-m <mcu>] [-f <freq>] [-d] [-v]\n", argv[0]);
        exit(1);
    }

    filename = argv[1];

	for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) {
            if (i + 1 < argc) mmcu = argv[++i];
        } else if (!strcmp(argv[i], "-f")) {
            if (i + 1 < argc) freq = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-d"))
			debug++;
		else if (!strcmp(argv[i], "-v"))
			verbose++;
	}

    int is_hex = (strstr(filename, ".hex") != NULL);
    elf_firmware_t f = {0};
    uint8_t * hex_data = NULL;
    uint32_t hex_size = 0, hex_base = 0;

    if (is_hex) {
        hex_data = read_ihex_file(filename, &hex_size, &hex_base);
        if (!hex_data) {
            fprintf(stderr, "%s: Unable to load HEX %s\n", argv[0], filename);
            exit(1);
        }
        printf("Loaded HEX: %d bytes at 0x%04x\n", hex_size, hex_base);
    } else {
        if (elf_read_firmware(filename, &f) < 0) {
            fprintf(stderr, "%s: Unable to load ELF %s\n", argv[0], filename);
            // Fallback: try hex loading anyway if extension was wrong or if user passed .hex but named it .elf?
            // But usually this fails because libelf is missing.
            exit(1);
        }
        if (f.mmcu[0]) mmcu = f.mmcu;
        if (f.frequency) freq = f.frequency;
    }

	avr = avr_make_mcu_by_name(mmcu);
	if (!avr) {
		fprintf(stderr, "%s: Error creating the AVR core for %s\n", argv[0], mmcu);
		exit(1);
	}

	avr_init(avr);
    avr->frequency = freq;

    if (is_hex) {
        memcpy(avr->flash + hex_base, hex_data, hex_size);
        free(hex_data);
        avr->pc = hex_base;
        avr->codeend = avr->flashend;
    } else {
        avr_load_firmware(avr, &f);
    }

	avr->log = 1 + verbose;

	if (debug) {
		avr->gdb_port = 1234;
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
        printf("GDB server listening on port 1234\n");
	}

	uart_pty_init(avr, &uart_pty);
	uart_pty_connect(&uart_pty, '0');

    printf("protosim running %s on %s at %d Hz\n", filename, mmcu, freq);
    printf("Press Ctrl+C to stop.\n");

	while (1) {
		int state = avr_run(avr);
		if ( state == cpu_Done || state == cpu_Crashed)
			break;
	}
    return 0;
}

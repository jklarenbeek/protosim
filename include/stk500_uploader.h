/*
 * stk500_uploader.h
 *
 * Background thread that speaks the STK500v1 / Optiboot protocol to
 * auto-upload the app firmware through the simulated UART endpoint.
 *
 * On Windows: connects to TCP 127.0.0.1:<port>
 * On Linux:   opens the PTY at /tmp/simavr-uart0
 *
 * Usage:
 *   stk500_args_t args;
 *   stk500_args_init(&args, binary, size, port);   // port ignored on Linux
 *   stk500_start_upload_thread(&args);
 */

#pragma once
#ifndef STK500_UPLOADER_H
#define STK500_UPLOADER_H

#include <stdint.h>
#include <stddef.h>

/* STK500v1 command bytes (Optiboot subset) */
#define STK_GET_SYNC        0x30
#define STK_GET_PARAMETER   0x41
#define STK_ENTER_PROGMODE  0x50
#define STK_LEAVE_PROGMODE  0x51
#define STK_LOAD_ADDRESS    0x55
#define STK_PROG_PAGE       0x64
#define STK_READ_SIGN       0x75
#define CRC_EOP             0x20  /* "end of packet" */
#define STK_INSYNC          0x14
#define STK_OK              0x10

/* Optiboot flash page size (ATmega328P) */
#define OPTIBOOT_PAGE_SIZE  128

typedef struct {
    const uint8_t *binary;   /* raw flash binary of the APP (not bootloader) */
    size_t         size;     /* number of bytes to upload                    */
    int            tcp_port; /* TCP port used by uart_com (Windows); 0=Linux */
} stk500_args_t;

/* Initialise the args struct.  Makes a private copy of the binary data. */
void stk500_args_init(stk500_args_t *a,
                      const uint8_t *binary, size_t size,
                      int tcp_port);

/* Free the private copy of binary data */
void stk500_args_free(stk500_args_t *a);

/*
 * Spawn a background thread that connects to the UART endpoint and
 * performs the full STK500 upload sequence.  Returns immediately.
 * The thread logs progress with the [STK] prefix.
 */
void stk500_start_upload_thread(stk500_args_t *a);

#endif /* STK500_UPLOADER_H */

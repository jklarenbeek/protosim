#ifndef __UART_COM_H___
#define __UART_COM_H___

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>
#include "sim_irq.h"
#include "fifo_declare.h"

enum {
	IRQ_UART_COM_BYTE_IN = 0,
	IRQ_UART_COM_BYTE_OUT,
	IRQ_UART_COM_COUNT
};

DECLARE_FIFO(uint8_t, uart_com_fifo, 512);

typedef struct uart_com_port_t {
	SOCKET s;
	SOCKET listen_s;
	int port;
	uart_com_fifo_t in;
	uart_com_fifo_t out;
	uint8_t buffer[512];
	size_t buffer_len, buffer_done;
} uart_com_port_t;

typedef struct uart_com_t {
	avr_irq_t *	irq;		// irq list
	struct avr_t *avr;		// keep it around so we can pause it

	HANDLE thread;
	DWORD thread_id;
	int xon;
	int stop_thread;

	uart_com_port_t port;
} uart_com_t;

void uart_com_init(struct avr_t * avr, uart_com_t * b);
void uart_com_stop(uart_com_t * p);
void uart_com_connect(uart_com_t * p, char uart);

#endif /* _WIN32 */

#endif /* __UART_COM_H___ */

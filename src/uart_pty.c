/*
	uart_pty.c
    (Copied from simavr examples)
 */

#include "sim_network.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#ifdef __APPLE__
#include <util.h>
#elif defined (__FreeBSD__)
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#else
#include <pty.h>
#endif

#include "uart_pty.h"
#include "avr_uart.h"
#include "sim_time.h"
#include "sim_hex.h"

DEFINE_FIFO(uint8_t,uart_pty_fifo);

//#define TRACE(_w) _w
#ifndef TRACE
#define TRACE(_w)
#endif

static void uart_pty_in_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	uart_pty_t * p = (uart_pty_t*)param;
	TRACE(printf("uart_pty_in_hook %02x\n", value);)
	uart_pty_fifo_write(&p->pty.in, value);

	if (p->tap.s) {
		if (p->tap.crlf && value == '\n')
			uart_pty_fifo_write(&p->tap.in, '\r');
		uart_pty_fifo_write(&p->tap.in, value);
	}
}

static void uart_pty_flush_incoming(uart_pty_t * p)
{
	while (p->xon && !uart_pty_fifo_isempty(&p->pty.out)) {
		TRACE(int r = p->pty.out.read;)
		uint8_t byte = uart_pty_fifo_read(&p->pty.out);
		TRACE(printf("uart_pty_flush_incoming send r %03d:%02x\n", r, byte);)
		avr_raise_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, byte);

		if (p->tap.s) {
			if (p->tap.crlf && byte == '\n')
				uart_pty_fifo_write(&p->tap.in, '\r');
			uart_pty_fifo_write(&p->tap.in, byte);
		}
	}
	if (p->tap.s) {
		while (p->xon && !uart_pty_fifo_isempty(&p->tap.out)) {
			uint8_t byte = uart_pty_fifo_read(&p->tap.out);
			if (p->tap.crlf && byte == '\r') {
				uart_pty_fifo_write(&p->tap.in, '\n');
			}
			if (byte == '\n')
				continue;
			uart_pty_fifo_write(&p->tap.in, byte);
			avr_raise_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, byte);
		}
	}
}

avr_cycle_count_t uart_pty_flush_timer(struct avr_t * avr, avr_cycle_count_t when, void * param)
{
	uart_pty_t * p = (uart_pty_t*)param;
	uart_pty_flush_incoming(p);
	return p->xon ? when + avr_hz_to_cycles(p->avr, 1000) : 0;
}

static void uart_pty_xon_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	uart_pty_t * p = (uart_pty_t*)param;
	TRACE(if (!p->xon) printf("uart_pty_xon_hook\n");)
	p->xon = 1;
	uart_pty_flush_incoming(p);
	if (p->xon)
			avr_cycle_timer_register(p->avr, avr_hz_to_cycles(p->avr, 1000),
						uart_pty_flush_timer, param);
}

static void uart_pty_xoff_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	uart_pty_t * p = (uart_pty_t*)param;
	TRACE(if (p->xon) printf("uart_pty_xoff_hook\n");)
	p->xon = 0;
	avr_cycle_timer_cancel(p->avr, uart_pty_flush_timer, param);
}

static void * uart_pty_thread(void * param)
{
	uart_pty_t * p = (uart_pty_t*)param;

	while (1) {
		fd_set read_set, write_set;
		int max = 0;
		FD_ZERO(&read_set);
		FD_ZERO(&write_set);

		for (int ti = 0; ti < 2; ti++) if (p->port[ti].s) {
			if (p->port[ti].buffer_len == p->port[ti].buffer_done) {
				FD_SET(p->port[ti].s, &read_set);
				max = p->port[ti].s > max ? p->port[ti].s : max;
			}
			if (!uart_pty_fifo_isempty(&p->port[ti].in)) {
				FD_SET(p->port[ti].s, &write_set);
				max = p->port[ti].s > max ? p->port[ti].s : max;
			}
		}

		struct timeval timo = { 0, 500 };
		int ret = select(max+1, &read_set, &write_set, NULL, &timo);

		if (ret < 0)
			break;

		for (int ti = 0; ti < 2; ti++) if (p->port[ti].s) {
			if (FD_ISSET(p->port[ti].s, &read_set)) {
				ssize_t r = read(p->port[ti].s, p->port[ti].buffer,
									sizeof(p->port[ti].buffer)-1);
				p->port[ti].buffer_len = r;
				p->port[ti].buffer_done = 0;
				TRACE(if (!p->port[ti].tap)
						hdump("pty recv", p->port[ti].buffer, r);)
			}
			if (p->port[ti].buffer_done < p->port[ti].buffer_len) {
				while (p->port[ti].buffer_done < p->port[ti].buffer_len &&
						!uart_pty_fifo_isfull(&p->port[ti].out)) {
					int index = p->port[ti].buffer_done++;
					TRACE(int wi = p->port[ti].out.write;)
					uart_pty_fifo_write(&p->port[ti].out,
							p->port[ti].buffer[index]);
					TRACE(printf("w %3d:%02x (%d/%d) %s\n",
								wi, p->port[ti].buffer[index],
								p->port[ti].out.read,
								p->port[ti].out.write,
								p->xon ? "XON" : "XOFF");)
				}
			}
			if (FD_ISSET(p->port[ti].s, &write_set)) {
				uint8_t buffer[512];
				uint8_t * dst = buffer;
				while (!uart_pty_fifo_isempty(&p->port[ti].in) &&
						(dst - buffer) < sizeof(buffer)) {
					*dst = uart_pty_fifo_read(&p->port[ti].in);
					dst++;
				}
				size_t len = dst - buffer;
				TRACE(size_t r =) write(p->port[ti].s, buffer, len);
				TRACE(if (!p->port[ti].tap) hdump("pty send", buffer, r);)
			}
		}
	}
	return NULL;
}

static const char * irq_names[IRQ_UART_PTY_COUNT] = {
	[IRQ_UART_PTY_BYTE_IN] = "8<uart_pty.in",
	[IRQ_UART_PTY_BYTE_OUT] = "8>uart_pty.out",
};

void uart_pty_init(struct avr_t * avr, uart_pty_t * p)
{
	memset(p, 0, sizeof(*p));

	p->avr = avr;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_PTY_COUNT, irq_names);
	avr_irq_register_notify(p->irq + IRQ_UART_PTY_BYTE_IN, uart_pty_in_hook, p);

	const int hastap = (getenv("SIMAVR_UART_TAP") && atoi(getenv("SIMAVR_UART_TAP"))) ||
			(getenv("SIMAVR_UART_XTERM") && atoi(getenv("SIMAVR_UART_XTERM")));
	p->hastap = hastap;

	for (int ti = 0; ti < 1 + hastap; ti++) {
		int m, s;

		if (openpty(&m, &s, p->port[ti].slavename, NULL, NULL) < 0) {
			fprintf(stderr, "%s: Can't create pty: %s", __FUNCTION__, strerror(errno));
			return;
		}
		struct termios tio;
		tcgetattr(m, &tio);
		cfmakeraw(&tio);
		tcsetattr(m, TCSANOW, &tio);
		p->port[ti].s = m;
		p->port[ti].tap = ti != 0;
		p->port[ti].crlf = ti != 0;
		p->port[ti].slavename[sizeof(p->port[ti].slavename)-1] = 0; // ensure null termination
		printf("uart_pty_init %s on port *** %s ***\n",
				ti == 0 ? "bridge" : "tap", p->port[ti].slavename);
	}

	pthread_create(&p->thread, NULL, uart_pty_thread, p);
}

void uart_pty_stop(uart_pty_t * p)
{
	puts(__func__);
	pthread_kill(p->thread, SIGINT);
	for (int ti = 0; ti < 2; ti++)
		if (p->port[ti].s)
			close(p->port[ti].s);
	void * ret;
	pthread_join(p->thread, &ret);
}

void uart_pty_connect(uart_pty_t * p, char uart)
{
	uint32_t f = 0;
	avr_ioctl(p->avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
	f &= ~AVR_UART_FLAG_STDIO;
	avr_ioctl(p->avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

	avr_irq_t * src = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
	avr_irq_t * dst = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
	avr_irq_t * xon = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
	avr_irq_t * xoff = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);
	if (src && dst) {
		avr_connect_irq(src, p->irq + IRQ_UART_PTY_BYTE_IN);
		avr_connect_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, dst);
	}
	if (xon)
		avr_irq_register_notify(xon, uart_pty_xon_hook, p);
	if (xoff)
		avr_irq_register_notify(xoff, uart_pty_xoff_hook, p);

	for (int ti = 0; ti < 1+(p->hastap?1:0); ti++) if (p->port[ti].s) {
		char link[128];
		snprintf(link, sizeof(link), "/tmp/simavr-uart%c%s", uart, ti == 1 ? "-tap" : "");
		unlink(link);
		if (symlink(p->port[ti].slavename, link) != 0) {
			fprintf(stderr, "WARN %s: Can't create %s: %s", __func__, link, strerror(errno));
		} else {
			printf("%s: %s now points to %s\n", __func__, link, p->port[ti].slavename);
			printf("\nTo connect:\n");
			printf("  picocom -b 9600 %s\n", p->port[ti].slavename);
			printf("  putty -serial %s -sercfg 9600,8,n,1,N\n\n", p->port[ti].slavename);
		}
	}
}

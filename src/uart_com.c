#ifdef _WIN32

#include "sim_network.h"
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>


#include "avr_uart.h"
#include "sim_hex.h"
#include "sim_time.h"
#include "uart_com.h"


#pragma comment(lib, "ws2_32.lib")

DEFINE_FIFO(uint8_t, uart_com_fifo);

#ifndef TRACE
#define TRACE(_w)
#endif

static void uart_com_in_hook(struct avr_irq_t *irq, uint32_t value,
                             void *param) {
  uart_com_t *p = (uart_com_t *)param;
  TRACE(printf("uart_com_in_hook %02x\n", value);)
  uart_com_fifo_write(&p->port.in, value);
}

static void uart_com_flush_incoming(uart_com_t *p) {
  while (p->xon && !uart_com_fifo_isempty(&p->port.out)) {
    TRACE(int r = p->port.out.read;)
    uint8_t byte = uart_com_fifo_read(&p->port.out);
    TRACE(printf("uart_com_flush_incoming send r %03d:%02x\n", r, byte);)
    avr_raise_irq(p->irq + IRQ_UART_COM_BYTE_OUT, byte);
  }
}

avr_cycle_count_t uart_com_flush_timer(struct avr_t *avr,
                                       avr_cycle_count_t when, void *param) {
  uart_com_t *p = (uart_com_t *)param;
  uart_com_flush_incoming(p);
  return p->xon ? when + avr_hz_to_cycles(p->avr, 1000) : 0;
}

static void uart_com_xon_hook(struct avr_irq_t *irq, uint32_t value,
                              void *param) {
  uart_com_t *p = (uart_com_t *)param;
  TRACE(if (!p->xon) printf("uart_com_xon_hook\n");)
  p->xon = 1;
  uart_com_flush_incoming(p);
  if (p->xon) {
    avr_cycle_timer_register(p->avr, avr_hz_to_cycles(p->avr, 1000),
                             uart_com_flush_timer, param);
  }
}

static void uart_com_xoff_hook(struct avr_irq_t *irq, uint32_t value,
                               void *param) {
  uart_com_t *p = (uart_com_t *)param;
  TRACE(if (p->xon) printf("uart_com_xoff_hook\n");)
  p->xon = 0;
  avr_cycle_timer_cancel(p->avr, uart_com_flush_timer, param);
}

static DWORD WINAPI uart_com_thread(LPVOID param) {
  uart_com_t *p = (uart_com_t *)param;
  fd_set read_set, write_set;
  struct timeval timo;

  while (!p->stop_thread) {
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);

    if (p->port.s != INVALID_SOCKET) {
      if (p->port.buffer_len == p->port.buffer_done) {
        FD_SET(p->port.s, &read_set);
      }
      if (!uart_com_fifo_isempty(&p->port.in)) {
        FD_SET(p->port.s, &write_set);
      }
    } else if (p->port.listen_s != INVALID_SOCKET) {
      FD_SET(p->port.listen_s, &read_set);
    }

    timo.tv_sec = 0;
    timo.tv_usec = 500000; // 500 ms

    int ret = select(0, &read_set, &write_set, NULL, &timo);

    if (ret < 0) {
      break;
    }

    if (p->port.listen_s != INVALID_SOCKET &&
        FD_ISSET(p->port.listen_s, &read_set)) {
      struct sockaddr_in client_addr;
      int client_len = sizeof(client_addr);
      SOCKET client = accept(p->port.listen_s, (struct sockaddr *)&client_addr,
                             &client_len);
      if (client != INVALID_SOCKET) {
        // We only handle one connection at a time. If one connects, close the
        // old one
        if (p->port.s != INVALID_SOCKET) {
          closesocket(p->port.s);
        }
        p->port.s = client;
        printf("uart_com: Client connected\n");

        // Optional: Switch to non-blocking mode
        u_long mode = 1;
        ioctlsocket(p->port.s, FIONBIO, &mode);
      }
    }

    if (p->port.s != INVALID_SOCKET) {
      if (FD_ISSET(p->port.s, &read_set)) {
        int r = recv(p->port.s, (char *)p->port.buffer,
                     sizeof(p->port.buffer) - 1, 0);
        if (r > 0) {
          p->port.buffer_len = r;
          p->port.buffer_done = 0;
        } else if (r <= 0) {
          // Client disconnected or error
          closesocket(p->port.s);
          p->port.s = INVALID_SOCKET;
          printf("uart_com: Client disconnected\n");
        }
      }

      if (p->port.buffer_done < p->port.buffer_len) {
        while (p->port.buffer_done < p->port.buffer_len &&
               !uart_com_fifo_isfull(&p->port.out)) {
          int index = p->port.buffer_done++;
          uart_com_fifo_write(&p->port.out, p->port.buffer[index]);
        }
      }

      if (FD_ISSET(p->port.s, &write_set)) {
        uint8_t buffer[512];
        uint8_t *dst = buffer;
        while (!uart_com_fifo_isempty(&p->port.in) &&
               (dst - buffer) < sizeof(buffer)) {
          *dst = uart_com_fifo_read(&p->port.in);
          dst++;
        }
        size_t len = dst - buffer;
        if (len > 0) {
          send(p->port.s, (const char *)buffer, len, 0);
        }
      }
    }
  }
  return 0;
}

static const char *irq_names[IRQ_UART_COM_COUNT] = {
    [IRQ_UART_COM_BYTE_IN] = "8<uart_com.in",
    [IRQ_UART_COM_BYTE_OUT] = "8>uart_com.out",
};

void uart_com_init(struct avr_t *avr, uart_com_t *p) {
  WSADATA wsaData;
  int iResult;

  memset(p, 0, sizeof(*p));

  // Initialize Winsock
  iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (iResult != 0) {
    fprintf(stderr, "WSAStartup failed: %d\n", iResult);
    return;
  }

  p->avr = avr;
  p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_COM_COUNT, irq_names);
  avr_irq_register_notify(p->irq + IRQ_UART_COM_BYTE_IN, uart_com_in_hook, p);

  p->port.port = 4000; // Default local port
  p->port.s = INVALID_SOCKET;
  p->port.listen_s = INVALID_SOCKET;

  struct sockaddr_in server_addr;
  p->port.listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (p->port.listen_s == INVALID_SOCKET) {
    fprintf(stderr, "socket failed with error: %ld\n", WSAGetLastError());
    return;
  }

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr =
      inet_addr("127.0.0.1"); // bind to localhost only
  server_addr.sin_port = htons(p->port.port);

  // Instead of failing entirely if port 4000 is used, try binding to next ports
  // if necessary
  int max_tries = 10;
  int bnd_res = -1;
  for (int i = 0; i < max_tries; i++) {
    if (bind(p->port.listen_s, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) != SOCKET_ERROR) {
      bnd_res = 0;
      break;
    }
    p->port.port++;
    server_addr.sin_port = htons(p->port.port);
  }

  if (bnd_res == -1) {
    fprintf(stderr, "bind failed with error: %ld\n", WSAGetLastError());
    closesocket(p->port.listen_s);
    p->port.listen_s = INVALID_SOCKET;
    return;
  }

  if (listen(p->port.listen_s, SOMAXCONN) == SOCKET_ERROR) {
    fprintf(stderr, "listen failed with error: %ld\n", WSAGetLastError());
    closesocket(p->port.listen_s);
    p->port.listen_s = INVALID_SOCKET;
    return;
  }

  printf("uart_com_init listening on TCP 127.0.0.1:%d\n", p->port.port);

  p->stop_thread = 0;
  p->thread = CreateThread(NULL, 0, uart_com_thread, p, 0, &p->thread_id);
}

void uart_com_stop(uart_com_t *p) {
  puts(__func__);
  p->stop_thread = 1;
  if (p->port.listen_s != INVALID_SOCKET) {
    closesocket(p->port.listen_s);
  }
  if (p->port.s != INVALID_SOCKET) {
    closesocket(p->port.s);
  }
  if (p->thread) {
    WaitForSingleObject(p->thread, INFINITE);
    CloseHandle(p->thread);
  }
  WSACleanup();
}

void uart_com_connect(uart_com_t *p, char uart) {
  uint32_t f = 0;
  avr_ioctl(p->avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
  f &= ~AVR_UART_FLAG_STDIO;
  avr_ioctl(p->avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

  avr_irq_t *src =
      avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
  avr_irq_t *dst =
      avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
  avr_irq_t *xon =
      avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
  avr_irq_t *xoff =
      avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);
  if (src && dst) {
    avr_connect_irq(src, p->irq + IRQ_UART_COM_BYTE_IN);
    avr_connect_irq(p->irq + IRQ_UART_COM_BYTE_OUT, dst);
  }
  if (xon)
    avr_irq_register_notify(xon, uart_com_xon_hook, p);
  if (xoff)
    avr_irq_register_notify(xoff, uart_com_xoff_hook, p);

  printf("uart_com_connect: UART%c is connected to TCP port %d\n", uart,
         p->port.port);
  printf("\nTo connect:\n");
  printf("  putty.exe -telnet 127.0.0.1 %d\n\n", p->port.port);
}

int uart_com_is_connected(uart_com_t *p) {
  return (p->port.s != INVALID_SOCKET);
}

#endif // _WIN32

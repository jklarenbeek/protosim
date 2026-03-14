/*
 * stk500_uploader.c
 *
 * Background thread that performs an Optiboot / STK500v1 firmware upload
 * through the simulated UART port so the bootloader hands off to the app
 * quickly (no need to wait for the watchdog timeout).
 *
 * Cross-platform:
 *   Windows – connects to TCP 127.0.0.1:<port>  (uart_com listens there)
 *   Linux   – opens the PTY at /tmp/simavr-uart0 (uart_pty creates it)
 */

#include "stk500_uploader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID  INVALID_SOCKET
#  define sock_close(s) closesocket(s)
#  define sock_sleep_ms(ms) Sleep(ms)
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <termios.h>
#  include <pthread.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
   typedef int sock_t;
#  define SOCK_INVALID  (-1)
#  define sock_close(s) close(s)
#  define sock_sleep_ms(ms) usleep((ms)*1000)
#endif

/* ────────────────────────── byte I/O helpers ──────────────────────────── */

/*
 * Send one byte.  Returns 0 on success, -1 on error.
 * On Linux, fd is an int; we cast it to sock_t which is also int.
 */
static int sock_send_byte(sock_t fd, uint8_t b)
{
#ifdef _WIN32
    return send(fd, (const char *)&b, 1, 0) == 1 ? 0 : -1;
#else
    return write((int)fd, &b, 1) == 1 ? 0 : -1;
#endif
}

/*
 * Receive one byte with a timeout_ms deadline.
 * Returns the byte on success, -1 on timeout or error.
 */
static int sock_recv_byte(sock_t fd, int timeout_ms)
{
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };
    int r = select((int)fd + 1, &rset, NULL, NULL, &tv);
    if (r <= 0) return -1;   /* timeout or error */
    uint8_t b;
#ifdef _WIN32
    if (recv(fd, (char *)&b, 1, 0) != 1) return -1;
#else
    if (read((int)fd, &b, 1) != 1) return -1;
#endif
    return (int)(unsigned int)b;
}

/* ─────────────────────── STK500 protocol helpers ──────────────────────── */

/*
 * Flush any pending input so our request/response cycles stay in sync.
 */
static void flush_input(sock_t fd)
{
    while (sock_recv_byte(fd, 5) >= 0)
        ;
}

/*
 * Send [data, len] and then CRC_EOP.
 */
static int stk_send(sock_t fd, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
        if (sock_send_byte(fd, data[i]) < 0) return -1;
    return sock_send_byte(fd, CRC_EOP);
}

/*
 * Expect STK_INSYNC (0x14), then STK_OK (0x10).
 * Returns 0 on success, -1 on mismatch/timeout.
 */
static int stk_expect_ok(sock_t fd)
{
    int b = sock_recv_byte(fd, 1000);
    if (b != STK_INSYNC) return -1;
    b = sock_recv_byte(fd, 1000);
    if (b != STK_OK) return -1;
    return 0;
}

/*
 * Synchronise with Optiboot: send STK_GET_SYNC + CRC_EOP, retry until OK.
 * The bootloader has ~1 second watchdog timeout so we try aggressively.
 */
static int stk_sync(sock_t fd)
{
    for (int attempt = 0; attempt < 64; attempt++) {
        flush_input(fd);
        uint8_t pkt[1] = { STK_GET_SYNC };
        if (stk_send(fd, pkt, 1) < 0) return -1;
        int b = sock_recv_byte(fd, 80);   /* short timeout per attempt */
        if (b == STK_INSYNC) {
            b = sock_recv_byte(fd, 80);
            if (b == STK_OK) {
                printf("[STK] Sync OK (attempt %d)\n", attempt + 1);
                return 0;
            }
        }
        sock_sleep_ms(10);
    }
    fprintf(stderr, "[STK] ERROR: failed to sync with bootloader\n");
    return -1;
}

/*
 * STK_ENTER_PROGMODE
 */
static int stk_enter_progmode(sock_t fd)
{
    uint8_t pkt[1] = { STK_ENTER_PROGMODE };
    if (stk_send(fd, pkt, 1) < 0) return -1;
    if (stk_expect_ok(fd) < 0) {
        fprintf(stderr, "[STK] ERROR: enter_progmode failed\n");
        return -1;
    }
    return 0;
}

/*
 * STK_LOAD_ADDRESS — word address (byte_addr >> 1)
 */
static int stk_load_address(sock_t fd, uint32_t byte_addr)
{
    uint16_t word_addr = (uint16_t)(byte_addr >> 1);
    uint8_t pkt[3] = {
        STK_LOAD_ADDRESS,
        (uint8_t)(word_addr & 0xFF),
        (uint8_t)(word_addr >> 8)
    };
    if (stk_send(fd, pkt, 3) < 0) return -1;
    if (stk_expect_ok(fd) < 0) {
        fprintf(stderr, "[STK] ERROR: load_address 0x%04x failed\n",
                byte_addr);
        return -1;
    }
    return 0;
}

/*
 * STK_PROG_PAGE — program one page of OPTIBOOT_PAGE_SIZE bytes.
 * Pads the page with 0xFF if binary data is shorter.
 */
static int stk_prog_page(sock_t fd,
                          const uint8_t *binary, size_t binary_size,
                          uint32_t byte_addr)
{
    /* Build header */
    uint8_t hdr[4] = {
        STK_PROG_PAGE,
        0x00,                    /* size high (= 0 for 128-byte page) */
        OPTIBOOT_PAGE_SIZE,      /* size low                          */
        'F'                      /* memory type: Flash                */
    };
    for (int i = 0; i < 4; i++)
        if (sock_send_byte(fd, hdr[i]) < 0) return -1;

    /* Send page data */
    for (int i = 0; i < OPTIBOOT_PAGE_SIZE; i++) {
        uint32_t src = byte_addr + (uint32_t)i;
        uint8_t b = (src < binary_size) ? binary[src] : 0xFF;
        if (sock_send_byte(fd, b) < 0) return -1;
    }
    /* CRC_EOP */
    if (sock_send_byte(fd, CRC_EOP) < 0) return -1;

    if (stk_expect_ok(fd) < 0) {
        fprintf(stderr, "[STK] ERROR: prog_page at 0x%04x failed\n",
                byte_addr);
        return -1;
    }
    return 0;
}

/*
 * STK_LEAVE_PROGMODE — bootloader resets / jumps to app
 */
static int stk_leave_progmode(sock_t fd)
{
    uint8_t pkt[1] = { STK_LEAVE_PROGMODE };
    if (stk_send(fd, pkt, 1) < 0) return -1;
    if (stk_expect_ok(fd) < 0) {
        fprintf(stderr, "[STK] ERROR: leave_progmode failed\n");
        return -1;
    }
    return 0;
}

/* ─────────────────────────── Connection helpers ───────────────────────── */

#ifdef _WIN32
static sock_t open_tcp(int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return SOCK_INVALID;

    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_INVALID) return SOCK_INVALID;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    /* The TCP server in uart_com may take a moment to start listening;
       retry for up to 2 seconds. */
    for (int i = 0; i < 40; i++) {
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return s;
        sock_sleep_ms(50);
    }
    closesocket(s);
    return SOCK_INVALID;
}
#else
static sock_t open_pty(void)
{
    const char *pty_path = "/tmp/simavr-uart0";
    /* The PTY may take a moment to appear */
    for (int i = 0; i < 40; i++) {
        int fd = open(pty_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            /* Set raw mode so bytes pass through unmodified */
            struct termios t;
            tcgetattr(fd, &t);
            cfmakeraw(&t);
            tcsetattr(fd, TCSANOW, &t);
            return (sock_t)fd;
        }
        sock_sleep_ms(50);
    }
    return SOCK_INVALID;
}
#endif

/* ─────────────────────────── Thread entry point ─────────────────────────── */

static void *stk500_upload_thread_fn(void *arg)
{
    stk500_args_t *a = (stk500_args_t *)arg;

    /* Small delay so the simulation loop has time to start and the
       bootloader reaches its serial-wait state. */
    sock_sleep_ms(200);

    /* Open connection */
#ifdef _WIN32
    sock_t fd = open_tcp(a->tcp_port);
#else
    sock_t fd = open_pty();
#endif
    if (fd == SOCK_INVALID) {
        fprintf(stderr, "[STK] ERROR: cannot connect to UART endpoint\n");
        return NULL;
    }
    printf("[STK] Connected to UART endpoint\n");

    /* ── STK500 upload sequence ── */
    if (stk_sync(fd) < 0)               goto done;
    if (stk_enter_progmode(fd) < 0)     goto done;

    /* Upload pages starting at byte address 0 */
    size_t n_pages = (a->size + OPTIBOOT_PAGE_SIZE - 1) / OPTIBOOT_PAGE_SIZE;
    if (n_pages == 0) n_pages = 1;  /* must upload at least one page */

    printf("[STK] Uploading %zu bytes in %zu page(s)...\n", a->size, n_pages);

    for (size_t pg = 0; pg < n_pages; pg++) {
        uint32_t byte_addr = (uint32_t)(pg * OPTIBOOT_PAGE_SIZE);
        if (stk_load_address(fd, byte_addr) < 0) goto done;
        if (stk_prog_page(fd, a->binary, a->size, byte_addr) < 0) goto done;
    }

    stk_leave_progmode(fd);   /* ignore return; BL may reset immediately */
    printf("[STK] Upload complete — bootloader is jumping to app\n");

done:
    sock_close(fd);
#ifdef _WIN32
    /* Don't call WSACleanup here; uart_com.c owns WSA lifetime */
#endif
    return NULL;
}

/* ────────────────────────── Public API ─────────────────────────────────── */

void stk500_args_init(stk500_args_t *a,
                      const uint8_t *binary, size_t size,
                      int tcp_port)
{
    a->tcp_port = tcp_port;
    a->size     = size;
    uint8_t *copy = (uint8_t *)malloc(size);
    if (!copy) { perror("stk500_args_init: malloc"); exit(1); }
    memcpy(copy, binary, size);
    a->binary = copy;
}

void stk500_args_free(stk500_args_t *a)
{
    free((void *)a->binary);
    a->binary = NULL;
    a->size   = 0;
}

void stk500_start_upload_thread(stk500_args_t *a)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0,
                            (LPTHREAD_START_ROUTINE)stk500_upload_thread_fn,
                            a, 0, NULL);
    if (!h)
        fprintf(stderr, "[STK] WARNING: failed to start upload thread\n");
    else
        CloseHandle(h);   /* detach — we don't need to join it */
#else
    pthread_t tid;
    if (pthread_create(&tid, NULL, stk500_upload_thread_fn, a) != 0)
        perror("[STK] WARNING: pthread_create");
    else
        pthread_detach(tid);
#endif
}

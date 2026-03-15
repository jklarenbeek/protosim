#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#define BAUD 115200
#define MYUBRR (F_CPU/16/BAUD-1)

volatile uint8_t rx_buffer[16];
volatile uint8_t rx_head = 0;
volatile uint8_t rx_tail = 0;

ISR(USART_RX_vect) {
    uint8_t data = UDR0;
    uint8_t next = (rx_head + 1) & 15;
    if (next != rx_tail) {
        rx_buffer[rx_head] = data;
        rx_head = next;
    }
}

void uart_init(uint16_t ubrr) {
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
    UCSR0C = (3 << UCSZ00);
}

void uart_transmit(uint8_t data) {
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = data;
}

void uart_print(const char *s) {
    while (*s) uart_transmit(*s++);
}

int main(void) {
    uart_init(MYUBRR);
    sei();

    uart_print("UART Interrupt Example Initialized\r\n");

    while (1) {
        if (rx_head != rx_tail) {
            uint8_t data = rx_buffer[rx_tail];
            rx_tail = (rx_tail + 1) & 15;
            uart_print("Echo: ");
            uart_transmit(data);
            uart_print("\r\n");
        }
        _delay_ms(5);
    }
    return 0;
}

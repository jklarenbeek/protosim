#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#include <avr/io.h>
#include <util/delay.h>

#define BAUD 9600
#define UBRR_VAL (F_CPU/16/BAUD-1)

void uart_init(void) {
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)UBRR_VAL;
    UCSR0B = (1<<RXEN0)|(1<<TXEN0);
    UCSR0C = (1<<UCSZ01)|(1<<UCSZ00); // 8N1
}

void uart_tx(unsigned char data) {
    while (!(UCSR0A & (1<<UDRE0)));
    UDR0 = data;
}

unsigned char uart_rx(void) {
    while (!(UCSR0A & (1<<RXC0)));
    return UDR0;
}

int main(void) {
    uart_init();
    // Wait for the first keystroke
    uart_rx();
    // Send "Ready\r\n"
    uart_tx('R'); uart_tx('e'); uart_tx('a'); uart_tx('d'); uart_tx('y');
    uart_tx('\r'); uart_tx('\n');

    while(1) {
        unsigned char c = uart_rx();
        uart_tx(c); // Echo back
    }
}

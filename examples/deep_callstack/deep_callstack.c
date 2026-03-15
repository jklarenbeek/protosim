#include <stdint.h>
#include <avr/io.h>

uint16_t __attribute__((noinline)) fibonacci(uint8_t n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

void __attribute__((noinline)) d(uint16_t val) {
    PORTB = (uint8_t)val;
}

void __attribute__((noinline)) c(uint16_t val) {
    d(val + 1);
}

void __attribute__((noinline)) b(uint16_t val) {
    c(val + 1);
}

void __attribute__((noinline)) a(uint16_t val) {
    b(val + 1);
}

int main(void) {
    DDRB = 0xFF;
    while (1) {
        uint16_t f = fibonacci(10);
        a(f);
    }
    return 0;
}

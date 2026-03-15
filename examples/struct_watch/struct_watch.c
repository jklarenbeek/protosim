#include <stdint.h>
#include <avr/io.h>
#include <util/delay.h>

typedef struct {
    uint16_t hp;
    uint8_t x;
    uint8_t y;
} player_t;

volatile player_t player = {100, 10, 20};

void __attribute__((noinline)) update_player(uint8_t dx, uint8_t dy) {
    player.x += dx;
    player.y += dy;
    if (player.hp > 0) player.hp--;
}

int main(void) {
    while (1) {
        update_player(1, 2);
        _delay_ms(10);
    }
    return 0;
}

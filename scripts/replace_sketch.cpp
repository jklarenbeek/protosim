#include <Arduino.h>  // Core Arduino definitions and functions

// Include your sketch code here (inline, as if concatenated)
#include "${SKETCH}"  // Replace with your .ino file name; it will be treated as C++

// Optional: If your board variant has custom init (e.g., for specific hardware)
extern void initVariant() __attribute__((weak));
void initVariant() { }

// Main entry point (mimics Arduino core's main.cpp)
int main(void) {
    init();         // Initialize Arduino core (timers, interrupts, etc.)
    initVariant();  // Board-specific variant initialization (if any)

#if defined(USBCON)
    USBDevice.attach();  // For boards with USB (e.g., Leonardo)
#endif

    setup();  // Call your sketch's setup() once

    for (;;) {  // Infinite loop to repeatedly call loop()
        loop();
        if (serialEventRun) serialEventRun();  // Handle serial events if defined
    }

    return 0;  // Never reached
}
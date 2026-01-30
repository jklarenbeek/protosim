# protosim 🚀

**The "No-Hardware" AVR Emulator for Protoduino**

protosim is a blazing fast, lightweight [AVR simulator](https://github.com/buserror/simavr) runner that exposes your simulated device as a real pseudo-terminal (PTY) on your host machine.

It allows you to develop, debug, and interact with AVR firmware (like [Protoduino](https://github.com/jklarenbeek/protoduino) projects) without needing a physical Arduino board, USB cables, or a clutter of wires.

## Why is it Cool? ❄️

*   **Virtual COM Ports**: Connect standard tools like `putty`, `screen`, `minicom`, or your own scripts to `/dev/pts/X` just like a real serial port.
*   **Instant Feedback**: No flashing time. Run your `.elf` or `.hex` file immediately.
*   **Logic Analyzer Built-in**: (Optional) Dump VCD files to visualize pin states and interrupts in GTKWave.
*   **GDB Debugging**: Attach a debugger to step through your AVR code instruction by instruction while it talks to your simulated serial terminal.
*   **Scriptable**: Automate integration tests for your embedded firmware.

## Project Structure

*   `src/`: The protosim host source code.
*   `lib/`: Dependencies (execute `npm run setup` for simavr & protoduino).
*   `examples/`: Sample AVR firmware to test the simulator.
*   `scripts/`: Helper scripts for setup and building.

## Prerequisites

*   **Linux/WSL2/macOS**
*   `gcc`, `make`, `git`,
*   `avr-gcc` (for compiling firmware examples),
*   `platform.io`, `pio`

For detailed instructions on how to setup your system read the [`ONBOARD.md`](./ONBOARD.md) document.

## Getting Started

### 1. Setup

Install dependencies and build the simulator core (`simavr`):

```bash
npm install
npm run setup
```

### 2. Build protosim

Build the `protosim` runner:

```bash
npm run build
```

### 3. Compile & Run an Example

Compile the `uart_echo` firmware and launch it in the simulator:

```bash
npm run example:uart_echo
```

You will see output like:
```
protosim running examples/uart_echo/firmware.hex on atmega328p at 16000000 Hz
UART PTY: /dev/pts/4 linked to /tmp/simavr-uart0
```

Now, open a new terminal and connect:
```bash
picocom -b 9600 /tmp/simavr-uart0
# or
putty -serial /tmp/simavr-uart0 -sercfg 9600,8,n,1,N
```

Type into `protosim`, and your virtual AVR will echo your characters back!

## Development

*   **VSCode**: Pre-configured with IntelliSense support.
*   **PlatformIO**: Includes configuration to build firmware examples easily.

## Author

- Joham (jklarenbeek@gmail.com)

## License

MIT

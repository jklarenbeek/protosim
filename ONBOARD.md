## Prerequisites and Installation

This section is designed for complete beginners who want to learn Arduino programming using the [Protoduino](https://github.com/jklarenbeek/protoduino) framework on a simulator, without needing physical hardware. We'll walk through everything step by step, assuming you have no prior experience with Linux terminals, compilers, or embedded development. By the end, you'll have a working setup to build, simulate, and interact with Arduino-like projects using protosim (which leverages simavr for AVR simulation) and Protoduino for advanced multitasking features.

### Why This Setup?

- **Arduino Basics**: Arduino projects typically use `.ino` or `.cpp` files with functions like `setup()` and `loop()`. You'll compile these into firmware (e.g., `.hex` files) using tools like avr-gcc or PlatformIO.
- **Protoduino**: This is a library that adds cooperative multitasking (protothreads), error handling, and inter-process communication (IPC) to your Arduino code, making it more like a mini operating system. It's great for learning advanced embedded concepts.
- **Simulator (simavr + protosim)**: Instead of uploading to a real Arduino board (e.g., Uno), you'll run your code in a software emulator. protosim enhances simavr by providing a virtual serial port (PTY) for interactive debugging, like echoing input/output in a terminal.

### System Requirements
- **Operating System**: Ubuntu 22.04 LTS or later (recommended for stability; as of 2026, Ubuntu 24.04 or 26.04 works fine). If you're on Windows, use WSL2 (Windows Subsystem for Linux) to run Ubuntu virtually. macOS users can follow similar steps but may need Homebrew for package installation.
- **Hardware**: Any modern computer with at least 4GB RAM and internet access for initial setup.
- **Time**: About 30-60 minutes for installation, depending on your internet speed.

If you're new to Ubuntu:
- Download and install from [ubuntu.com](https://ubuntu.com/download/desktop).
- Or, on Windows: Search for "Turn Windows features on or off," enable WSL, then run `wsl --install -d Ubuntu` in PowerShell.

Open a terminal in Ubuntu (Ctrl+Alt+T) for all commands below. Use `sudo` for admin privileges—enter your password when prompted.

### Step 1: Update Your System and Install Basic Tools
These are essential for building software from source and managing dependencies.

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install git gcc make build-essential -y
```

- **What this does**: `apt update` refreshes package lists, `upgrade` installs updates, and the install command gets Git (for downloading code), GCC (C compiler), Make (build tool), and build-essential (common development headers).

### Step 2: Install the AVR Toolchain (for Compiling Arduino Code)
AVR is the architecture for most Arduino boards (e.g., Uno uses ATmega328P). You'll need avr-gcc to compile C/C++ code into AVR binaries.

```bash
sudo apt install gcc-avr binutils-avr avr-libc gdb-avr avrdude -y
```

- **What this does**:
  - `gcc-avr`: Compiler for AVR C/C++ code.
  - `binutils-avr`: Tools like assembler and linker.
  - `avr-libc`: Standard C library for AVR.
  - `gdb-avr`: Debugger for AVR code (useful with simavr).
  - `avrdude`: Normally for uploading to hardware, but here it's for completeness (e.g., simulating uploads).
- **Verify**: Run `avr-gcc --version`. It should show version info (e.g., 7.x or later in 2026).

### Step 3: Install Node.js and npm (for Project Scripts)
This project uses npm (Node Package Manager) to run setup scripts and build commands.

```bash
sudo apt install nodejs npm -y
```

- **What this does**: Installs Node.js (JavaScript runtime) and npm (package manager).
- **Verify**: Run `node --version` and `npm --version`.
- **Note**: If versions are outdated (e.g., pre-18.x), update with `sudo npm install -g n && sudo n stable`.

Or use [nvm](https://github.com/nvm-sh/nvm) and manage nodejs versions:

```bash
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.4/install.sh | bash
```

### Step 4: Install PlatformIO CLI (for Building Arduino Projects)

PlatformIO is a command-line tool that handles Arduino frameworks, libraries (like Protoduino), and compilation without the Arduino IDE. It's cross-platform and automates a lot for beginners.

1. Install Python (if not already present; Ubuntu usually has it):
   ```bash
   sudo apt install python3 python3-pip python3-venv -y
   ```

2. Download and run the installer:
   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o get-platformio.py
   python3 get-platformio.py
   ```

- **What this does**: Downloads a Python script and installs PlatformIO Core (CLI) in `~/.platformio`.
- **Add to PATH** (so you can run `pio` from anywhere): Edit your shell config.
  ```bash
  echo 'export PATH="$PATH:$HOME/.platformio/penv/bin"' >> ~/.bashrc
  source ~/.bashrc
  ```
- **Verify**: Run `pio --version`. It should show 6.x or later (as of 2026).
- **Tip**: If you see errors, ensure Python is at version 3.8+ (`python3 --version`). PlatformIO handles AVR boards like Uno automatically.

### Step 5: Install simavr (Optional)

simavr is the core simulator for AVR chips. protosim builds on it for PTY support.

```bash
sudo apt install simavr -y
```

- **What this does**: Installs the pre-built simavr package (easier than building from source).
- **Alternative: Build from Source** (if apt version is outdated):
  ```bash
  git clone https://github.com/buserror/simavr.git
  cd simavr
  make
  sudo make install RELEASE=1
  ```
- **Verify**: Run `simavr --version`.
- **Features for Arduino**: Supports ATmega328P (Uno), peripherals like UART/Timers/SPI, GDB debugging, and VCD traces for waveform viewing (install `gtkwave` with `sudo apt install gtkwave -y` if needed).

### Step 6: Clone and Set Up the protosim Project
This pulls the project code, sets up Protoduino and simavr libraries, and builds the simulator.

1. Clone the repository (replace with actual URL if it's hosted; assuming it's local or on GitHub):
   ```bash
   git clone https://github.com/jklarenbeek/protosim.git  # Replace with real repo URL
   cd protosim
   ```

2. Install npm dependencies:
   ```bash
   npm install
   ```

3. Run setup scripts:
   ```bash
   npm run setup
   ```
   - **What this does**: Executes `scripts/setup-simavr.sh` (clones and builds simavr if not from apt) and `scripts/setup-protoduino.sh` (clones Protoduino from https://github.com/jklarenbeek/protoduino and sets it up as a library in `libraries/`).

4. Build protosim:
   ```bash
   npm run build
   ```
   - **What this does**: Compiles `src/protosim.c` and `src/uart_pty.c` using the Makefile, linking to simavr. Outputs `bin/protosim`.

- **Protoduino Setup Details**: Once cloned, Protoduino is placed in `libraries/protoduino/`. It includes `protoduino.h` for multitasking. No extra install needed—PlatformIO will detect it via `platformio.ini`.

### Step 7: Test with an Example
Now you're ready to build and simulate!

1. Build a firmware example (e.g., UART echo):
   ```bash
   npm run firmware:uart_echo  # Or for Protoduino: pio run -e pd_01
   ```
   - This uses PlatformIO to compile `.c`/`.cpp` in `firmware/` to `.hex` in `.pio/build/`.

2. Run in simulator:
   ```bash
   npm run example:uart_echo  # Launches protosim with the .hex
   ```
   - Output shows PTY path (e.g., `/tmp/simavr-uart0`). Open another terminal: `picocom -b 9600 /tmp/simavr-uart0` (install picocom with `sudo apt install picocom -y`).
   - Type and see echoes—your code is running virtually!

### Troubleshooting

- **Errors?** Check paths (e.g., in Makefile or platformio.ini). Run `pio system info` for diagnostics.
- **Learning Resources**: Read Protoduino docs in its repo. Start with simple Arduino sketches, add Protoduino features like `PT_BEGIN()`/`PT_END()` for threads.
- **Next Steps**: Create your own project in `examples/myproject/` with `platformio.ini` env, include `<protoduino.h>`, build with `pio run`, simulate with protosim.
- **Updates**: Run `sudo apt update && sudo apt upgrade` periodically. For PlatformIO: `pio upgrade`.

With this, you can experiment with Arduino code safely in a simulation.

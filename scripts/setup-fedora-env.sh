#!/bin/bash

# Exit on any error
set -e

echo "=========================================="
echo " Setting up protosim environment on Fedora"
echo "=========================================="

echo "1. Updating system package lists..."
sudo dnf update -y

echo "2. Installing essential build tools and AVR toolchain..."
sudo dnf install -y git gcc gcc-c++ make libelf-dev avr-gcc avr-binutils avr-libc avr-gdb avrdude simavr picocom python3 python3-pip curl

echo "3. Installing NVM and Node.js..."
if command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1; then
    echo "Node.js and npm are already installed. Skipping NVM installation."
else
    export NVM_DIR="$HOME/.nvm"
    if [ ! -d "$NVM_DIR" ]; then
        echo "Downloading and installing NVM..."
        curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash

        # Load nvm
        [ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
    else
        echo "NVM is already installed."
        [ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
    fi

    echo "Installing Latest LTS Node.js..."
    nvm install --lts
    nvm use --lts
fi

echo "4. Installing PlatformIO CLI..."
if [ ! -d "$HOME/.platformio" ]; then
    echo "Downloading PlatformIO installer..."
    curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
    python3 /tmp/get-platformio.py
    rm /tmp/get-platformio.py

    # Adding PlatformIO to PATH in .bashrc if not present
    if ! grep -q "\.platformio/penv/bin" ~/.bashrc; then
        echo -e '\n# PlatformIO Path' >> ~/.bashrc
        echo 'export PATH="$PATH:$HOME/.platformio/penv/bin"' >> ~/.bashrc
    fi
else
    echo "PlatformIO is already installed."
fi

echo "=========================================="
echo " Setup Complete!"
echo "=========================================="
echo "Please run the following command to reload your environment,"
echo "or simply close and reopen your terminal:"
echo ""
echo "    source ~/.bashrc"
echo ""

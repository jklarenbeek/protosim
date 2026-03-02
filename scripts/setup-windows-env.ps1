# PowerShell Script for Setting up Protosim in Windows

Write-Host "=========================================="
Write-Host " Setting up protosim environment on Windows"
Write-Host "=========================================="

Write-Host "1. Installing essential tools (Git, Node.js, Python) via winget..."

# Install Git
if (!(Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Git..."
    winget install --id Git.Git -e --source winget
} else {
    Write-Host "Git is already installed."
}

# Install Node.js
if (!(Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Node.js..."
    winget install --id OpenJS.NodeJS.LTS -e --source winget
} else {
    Write-Host "Node.js is already installed."
}

# Install Python
if (!(Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Python (used by PlatformIO installer)..."
    winget install --id Python.Python.3.12 -e --source winget
} else {
    Write-Host "Python is already installed."
}

Write-Host "2. Installing PlatformIO CLI..."
$PIO_DIR = "$env:USERPROFILE\.platformio"

if (!(Test-Path $PIO_DIR)) {
    Write-Host "Downloading PlatformIO installer..."
    $installerPath = "$env:TEMP\get-platformio.py"
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py" -OutFile $installerPath

    Write-Host "Running PlatformIO installer..."
    python $installerPath

    Remove-Item $installerPath

    # Adding PlatformIO to User PATH
    $ExistingPath = [System.Environment]::GetEnvironmentVariable("Path", "User")
    $PIO_PATH = "$PIO_DIR\penv\Scripts"
    if ($ExistingPath -notmatch [regex]::Escape($PIO_PATH)) {
        [System.Environment]::SetEnvironmentVariable("Path", $ExistingPath + ";" + $PIO_PATH, "User")
        Write-Host "Added PlatformIO to User PATH."
    }
} else {
    Write-Host "PlatformIO is already installed."
}

Write-Host "=========================================="
Write-Host " Action Required for GCC and Make"
Write-Host "=========================================="
Write-Host "For compiling C code on Windows, MSYS2 is highly recommended."
Write-Host "1. Download and install MSYS2 from https://www.msys2.org/"
Write-Host "2. Open the 'MSYS2 UCRT64' terminal and run:"
Write-Host "   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make"
Write-Host "3. Add 'C:\msys64\ucrt64\bin' to your Windows System PATH"
Write-Host "=========================================="
Write-Host ""
Write-Host "Setup Complete!"
Write-Host "Please close and reopen your terminal/IDE for environment variables to take effect."
Write-Host ""

@echo off
setlocal enabledelayedexpansion

echo ==========================================
echo  Building simavr core on Windows...
echo ==========================================

:: Assuming MSYS2 is installed at C:\Tools\msys64 and UCRT64 is used
set "MSYS2_ROOT=C:\Tools\msys64"
set "UCRT64_BIN=%MSYS2_ROOT%\ucrt64\bin"
set "PATH=%UCRT64_BIN%;%PATH%"

:: Ensure gcc is found
where gcc >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] gcc not found in %UCRT64_BIN%.
    echo Please ensure MSYS2 ucrt64 gcc is installed via: pacman -S mingw-w64-ucrt-x86_64-gcc
    exit /b 1
)

:: Paths (all relative to the protosim root directory)
set "SIMAVR_DIR=libraries\simavr"
set "SIM_DIR=%SIMAVR_DIR%\simavr\sim"
set "AVR_DIR=%SIM_DIR%"
set "CORES_DIR=%SIMAVR_DIR%\simavr\cores"
set "OBJ_DIR=%SIMAVR_DIR%\simavr\obj-x86_64-w64-mingw32"
set "WIN_COMPAT=include\win32_compat.h"

if not exist "%SIM_DIR%\sim_avr.c" (
    echo [ERROR] simavr sources not found at %SIMAVR_DIR%.
    echo Please run: git submodule update --init libraries/simavr
    exit /b 1
)

if not exist "%WIN_COMPAT%" (
    echo [ERROR] Win32 compat header not found: %WIN_COMPAT%
    echo Please ensure include\win32_compat.h exists in the protosim root.
    exit /b 1
)

:: Generate sim_core_config.h and sim_core_decl.h
echo Generating sim core headers...
python scripts\generate_core_decl.py
if %errorlevel% neq 0 (
    echo [ERROR] Failed to generate headers. Is Python installed?
    exit /b 1
)

:: Create object output directory
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

:: ── Compiler flags ─────────────────────────────────────────────────────────
:: -include injects win32_compat.h into EVERY .c file without modifying them.
:: -DHAVE_LIBELF=1 enables ELF loading (libelf available in MSYS2 ucrt64).
set "CFLAGS=-O2 -Wall -Wno-unused-parameter -Wno-unused-result -Wno-missing-field-initializers -Wno-sign-compare -Wno-implicit-function-declaration -g"
set "CFLAGS=%CFLAGS% -DHAVE_LIBELF=1"
set "CFLAGS=%CFLAGS% -include %WIN_COMPAT%"

:: Include paths: simavr headers + MSYS2 ucrt64 system headers
set "INCLUDES=-I%SIM_DIR% -I%AVR_DIR% -I%SIMAVR_DIR%\simavr\cores"
set "INCLUDES=%INCLUDES% -I%MSYS2_ROOT%\ucrt64\include"
set "INCLUDES=%INCLUDES% -I%MSYS2_ROOT%\ucrt64\include\libelf"

:: Object file list (accumulated across all compilation steps)
set "OBJS="

:: ── Compile sim_*.c files (live in SIM_DIR) ────────────────────────────────
echo.
echo [1/3] Compiling sim/*.c files...
for %%F in (
    sim_avr.c
    sim_cmds.c
    sim_core.c
    sim_cycle_timers.c
    sim_dwarf.c
    sim_elf.c
    sim_hex.c
    sim_interrupts.c
    sim_io.c
    sim_irq.c
    sim_utils.c
    sim_vcd_file.c
) do (
    echo   CC sim/%%F
    gcc %CFLAGS% %INCLUDES% -c "%SIM_DIR%\%%F" -o "%OBJ_DIR%\%%~nF.o"
    if !errorlevel! neq 0 (
        echo [ERROR] Failed to compile sim/%%F
        exit /b 1
    )
    set "OBJS=!OBJS! %OBJ_DIR%\%%~nF.o"
)

:: ── Compile sim_gdb.c separately with -DSIM_GDB_COMPILE ────────────────────
:: This tells win32_compat.h to map close() → closesocket() which is correct
:: for sim_gdb.c where close() is always called on socket descriptors.
echo   CC sim/sim_gdb.c  ^(+SIM_GDB_COMPILE^)
gcc %CFLAGS% %INCLUDES% -DSIM_GDB_COMPILE -c "%SIM_DIR%\sim_gdb.c" -o "%OBJ_DIR%\sim_gdb.o"
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile sim/sim_gdb.c
    exit /b 1
)
set "OBJS=%OBJS% %OBJ_DIR%\sim_gdb.o"

:: ── Compile avr_*.c files (all live in SIM_DIR alongside sim_*.c) ────────────
echo.
echo [2/3] Compiling avr/*.c files...
for %%F in (
    avr_adc.c
    avr_acomp.c
    avr_bitbang.c
    avr_eeprom.c
    avr_extint.c
    avr_flash.c
    avr_ioport.c
    avr_lin.c
    avr_spi.c
    avr_timer.c
    avr_twi.c
    avr_uart.c
    avr_usi.c
    avr_usb.c
    avr_watchdog.c
) do (
    if exist "%SIM_DIR%\%%F" (
        echo   CC avr/%%F
        gcc %CFLAGS% %INCLUDES% -c "%SIM_DIR%\%%F" -o "%OBJ_DIR%\%%~nF.o"
        if !errorlevel! neq 0 (
            echo [ERROR] Failed to compile avr/%%F
            exit /b 1
        )
        set "OBJS=!OBJS! %OBJ_DIR%\%%~nF.o"
    ) else (
        echo   [SKIP] avr/%%F ^(not found^)
    )
)

:: ── Compile AVR MCU core files (live in CORES_DIR) ─────────────────────────
echo.
echo [3/3] Compiling cores/sim_*.c files...
for %%F in ("%CORES_DIR%\sim_*.c") do (
    echo   CC cores/%%~nxF
    gcc %CFLAGS% %INCLUDES% -DAVR_CORE=1 -c "%%F" -o "%OBJ_DIR%\%%~nF.o"
    if !errorlevel! neq 0 (
        echo [ERROR] Failed to compile cores/%%~nxF
        exit /b 1
    )
    set "OBJS=!OBJS! %OBJ_DIR%\%%~nF.o"
)

:: ── Archive into libsimavr.a ────────────────────────────────────────────────
echo.
echo Archiving libsimavr.a...
if exist "%OBJ_DIR%\libsimavr.a" del "%OBJ_DIR%\libsimavr.a"
ar cru "%OBJ_DIR%\libsimavr.a" !OBJS!
if %errorlevel% neq 0 (
    echo [ERROR] ar failed to create libsimavr.a
    exit /b 1
)
ranlib "%OBJ_DIR%\libsimavr.a"
if %errorlevel% neq 0 (
    echo [ERROR] ranlib failed
    exit /b 1
)

echo.
echo ==========================================
echo  SUCCESS: simavr built
echo  ^> %OBJ_DIR%\libsimavr.a
echo ==========================================
exit /b 0

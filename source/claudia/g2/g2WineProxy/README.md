# Nord Modular G2 Editor — Wine / CrossOver User-Mode USB Proxy

This directory provides the 32-bit Windows user-mode proxy DLL (`setupapi.dll`) that enables the official, legacy **Nord Modular G2 Editor v1.62** (`Nord Modular G2 Editor v1.62.exe`) to communicate seamlessly with the emulated Nord Modular G2 engine running inside a DAW (REAPER, Bitwig, etc.) or standalone test harness on macOS and Linux.

## Architecture

The genuine Nord Modular G2 hardware communicates over USB via a Philips ISP1181 device controller. On Windows, the official Clavia editor connects via SetupAPI using device interface GUID `{CB3ED981-6125-4047-BC2A-292E370CC89A}`, opens the device synchronously via `CreateFileA`, registers an event handle via IOCTL `0x222000`, and transfers bulk packets via Win32 `ReadFile` and `WriteFile`.

Under Wine and CrossOver on macOS:
1. Virtual USB pass-through fails because macOS Wine builds omit USB kernel backend support in `winebus.so`.
2. Emulated Windows `.sys` kernel drivers fail because unnamed Win32 event handles cannot be safely resolved across the Wine kernel/user process boundary.

To resolve this without requiring kernel drivers, code signing, or Wine modifications, this proxy DLL intercepts SetupAPI and file I/O calls in user mode within the editor's own process. It translates hardware device requests into a standard TCP loopback connection (`127.0.0.1:7777`) to the running G2 emulator (`TransportSocketServer`).

```
+-------------------------------------------------------------+
| CrossOver / Wine (32-bit Windows Subsystem)                 |
|                                                             |
|   Nord Modular G2 Editor v1.62.exe                          |
|         │                                                   |
|         ▼ (SetupAPI / Win32 File I/O)                       |
|   setupapi.dll (User-Mode Proxy Hook)                       |
+─────────┼───────────────────────────────────────────────────+
          │
          │ TCP Loopback (127.0.0.1:7777)
          ▼
+─────────────────────────────────────────────────────────────+
| Host OS (macOS / Linux)                                     |
|                                                             |
|   DAW Host (REAPER, AU/VST3)                                |
|     └── NordModularG2ServerPlugin                           |
|           └── TransportSocketServer (Port 7777)             |
|                 └── g2Lib (ISP1181 Emulation & DSP Engine)  |
+-------------------------------------------------------------+
```

## Building the Proxy DLL

### Prerequisites
- Cross-compiler: `i686-w64-mingw32-g++` (available on macOS via `brew install mingw-w64`).

### Build Command
Run the included build script:
```bash
./build_setupapi_dll.sh
```

This compiles a standalone 32-bit `setupapi.dll` with static runtime linkage (`-static -static-libgcc -static-libstdc++`) and Win32 threads, eliminating external runtime DLL dependencies.

## Installation into CrossOver or Wine

1. Locate the directory containing the installed Windows editor:
   - Example in CrossOver: `~/Library/Application Support/CrossOver/Bottles/<bottle_name>/drive_c/Program Files/Clavia/Nord Modular G2/`
2. Copy the compiled `setupapi.dll` directly into the same directory as `Nord Modular G2 Editor v1.62.exe`.
3. In Wine / CrossOver configuration (`winecfg`), ensure the DLL override for `setupapi` is set to **native, builtin** (so Wine loads the local `setupapi.dll` from the application directory before falling back to system libraries).

## Usage with DAW

1. Launch your DAW host (e.g. REAPER) on the host machine.
2. Insert `NordModularG2ServerPlugin` (VST3 or AU) onto an instrument track.
   - The plugin initializes and automatically binds `TransportSocketServer` on `127.0.0.1:7777`.
3. Launch `NordModularG2 Editor v1.62` in CrossOver or Wine.
   - The proxy DLL connects immediately to `127.0.0.1:7777`.
   - The editor displays the synthesizer online and enables patch transmission, live editing, and parameter tweaking.

# Wine User-Mode Editor Proxy (rg2WineProxy)

`rg2WineProxy` provides a user-mode Windows DLL proxy (`setupapi.dll`) that enables the
genuine 32-bit Clavia Windows editor (`Nord Modular G2 Editor v1.62.exe`) running under
Wine or CrossOver on macOS and Linux to communicate directly with the emulated G2 engine.

This approach replaces kernel-level USB emulation (`g2usb.sys`), kernel USBIP drivers,
and physical USB loopback cables with a user-mode loopback TCP socket bridge.

---

## Architecture

```
+--------------------------------------------------------------------------+
| Wine / CrossOver Environment (Windows 32-bit)                            |
|                                                                          |
|   +---------------------------------------+                              |
|   |    Nord Modular G2 Editor v1.62.exe   |                              |
|   +-------------------+-------------------+                              |
|                       |                                                  |
|         (SetupAPI & File I/O Calls)                                      |
|                       v                                                  |
|   +---------------------------------------+                              |
|   |   setupapi.dll (User-Mode Proxy)      |                              |
|   |                                       |                              |
|   |   - SetupAPI Hook: Clavia GUID        |                              |
|   |   - IAT Hooks: CreateFile, DeviceIo,  |                              |
|   |                ReadFile, WriteFile    |                              |
|   |   - Private IOCTL Handler (0x22200x)  |                              |
|   |   - Event Completion Signaler         |                              |
|   +-------------------+-------------------+                              |
+-----------------------|--------------------------------------------------+
                        | TCP Loopback (127.0.0.1:7777)
+-----------------------|--------------------------------------------------+
| Host Environment (macOS / Linux)                                         |
|                       v                                                  |
|   +---------------------------------------+                              |
|   |   TransportSocketServer (rg2Lib)       |                              |
|   |   - Attached to rg2::TransportHub      |                              |
|   |   - Framed message packing/unpacking  |                              |
|   +-------------------+-------------------+                              |
|                       v                                                  |
|   +---------------------------------------+                              |
|   |   G2 Emulation Engine / VST3 Plugin   |                              |
|   |   (e.g., loaded in REAPER or DAW)     |                              |
|   +---------------------------------------+                              |
+--------------------------------------------------------------------------+
```

### 1. Device Enumeration Interception
The Clavia Windows editor enumerates devices using the SetupAPI interface with the
Clavia G2 device interface GUID:
```
{CB3ED981-6125-4047-BC2A-292E370CC89A}
```
`setupapi.dll` intercepts `SetupDiGetClassDevsA`, `SetupDiEnumDeviceInterfaces`, and
`SetupDiGetDeviceInterfaceDetailA`. When the requested GUID matches the Clavia GUID,
the proxy returns a virtual device path:
```
\\?\usb#vid_0ffc&pid_0002#clavia_g2_virtual#{cb3ed981-6125-4047-bc2a-292e370cc89a}
```
All non-Clavia SetupAPI calls are forwarded directly to the system `setupapi.dll` located
in `C:\windows\system32\setupapi.dll`.

### 2. Import Address Table (IAT) Hooks
Upon load via `DllMain`, the proxy scans the editor's PE Import Address Table for `kernel32.dll`
imports and hooks:
- `CreateFileA`: Intercepts opens of the virtual Clavia path, connects over TCP loopback to
  `127.0.0.1:7777`, and returns a virtual handle (`0x47325553`, ASCII `'G2US'`).
- `CloseHandle`: Detects the virtual handle, disconnects the TCP session, and cleans up state.
- `DeviceIoControl`: Intercepts the private Clavia driver IOCTL codes.
- `ReadFile`: Drains bulk IN responses from the internal incoming queue.
- `WriteFile`: Encapsulates bulk OUT data into framed packets and streams them over the socket.

### 3. Private IOCTLs Handled in User Space
The original `g2usb.sys` Windows kernel driver exposed four control codes handled by the proxy:
- `0x222000`: Registers an unnamed completion event handle. A background thread listens
  for incoming TCP packets from `TransportSocketServer` and signals this event via
  `SetEvent(hEvent)` whenever notification records or bulk responses arrive.
- `0x222004`: Unregisters the completion event handle.
- `0x222008`: Queries the event registration state.
- `0x22200C`: Dequeues a 16-byte notification record from the internal ring buffer.

### 4. Framing Protocol
Packets transmitted across the loopback socket carry a 12-byte header followed by payload:
- `uint32_t magic` (`0x47325553`, `'G2US'`)
- `uint16_t channel` (`0` = Bulk Data, `1` = Notification Record)
- `uint16_t reserved` (`0`)
- `uint32_t payloadLen` (Byte length of the payload)

---

## Cross-Compilation with MinGW

The proxy is built as a 32-bit PE DLL (`setupapi.dll`) targeting Windows x86.

### Prerequisites

- **macOS** (Homebrew):
  ```bash
  brew install mingw-w64
  ```
- **Debian / Ubuntu**:
  ```bash
  sudo apt-get install gcc-mingw-w64-i686 g++-mingw-w64-i686
  ```

### Build Script

Run the provided build script from the repository:
```bash
cd source/claudia/rg2/rg2WineProxy
./build_setupapi_dll.sh
```

### Manual Compilation Command

The script invokes the MinGW cross-compiler with static runtime linkage to eliminate
runtime dependencies on external GCC runtime DLLs (`libwinpthread-1.dll`, `libgcc_s_dw2-1.dll`):

```bash
i686-w64-mingw32-g++ -shared -O2 -std=c++17 \
    -static -static-libgcc -static-libstdc++ \
    -I. -I../rg2Lib \
    setupapi_wine.cpp rg2usb.cpp setupapi.def \
    -lws2_32 -lsetupapi \
    -o setupapi.dll
```

---

## CrossOver and Wine Installation

### 1. Place the DLL Next to the Editor Executable
Copy the compiled `setupapi.dll` directly into the directory containing `Nord Modular G2 Editor v1.62.exe`:

- **CrossOver (macOS)**:
  ```bash
  BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/<bottle_name>"
  APP_DIR="$BOTTLE_DIR/drive_c/Program Files (x86)/Clavia/Nord Modular G2"
  cp setupapi.dll "$APP_DIR/"
  ```
- **Standard Wine**:
  ```bash
  WINEPREFIX="$HOME/.wine"
  APP_DIR="$WINEPREFIX/drive_c/Program Files (x86)/Clavia/Nord Modular G2"
  cp setupapi.dll "$APP_DIR/"
  ```

### 2. DLL Override Configuration
Windows and Wine search the application directory before searching system paths. Placing
`setupapi.dll` beside the executable ensures the editor loads the proxy automatically.

If Wine is configured with global builtin overrides, verify or set the DLL override in `winecfg`:
1. Run `winecfg` (or open the bottle settings in CrossOver).
2. Navigate to the **Libraries** tab.
3. Under **New override for library**, enter `setupapi`.
4. Click **Add**, then select **Edit** and set to **Native then Builtin** (`native,builtin`).

---

## Connecting to REAPER / DAW Host

1. **Launch Host and Load Plugin**:
   - Open REAPER (or your preferred DAW) on macOS or Linux.
   - Insert the `RedGecko2` VST3 plugin on an audio track.
   - The plugin boots the G2 DSP engine and starts `TransportSocketServer` listening on `127.0.0.1:7777`.

2. **Launch the Editor**:
   - Start `Nord Modular G2 Editor v1.62.exe` inside Wine or CrossOver.
   - The editor initializes SetupAPI, detects the virtual Clavia device, connects to `127.0.0.1:7777`,
     and establishes immediate bidirectional communication.
   - Patch adjustments, parameter tweaks, and voice allocations in the Windows editor communicate
     synchronously with the G2 engine in real time.

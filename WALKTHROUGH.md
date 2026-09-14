# MEDIATOR — Technical Walkthrough

This document is a deep-dive technical reference for the MEDIATOR firmware. See `README.md` for a high-level overview and quick-start guide.

---

## Hardware Wiring

**Board:** Raspberry Pi Pico 2 W (RP2350)

The PIO-USB host port is wired using a USB-A female breakout connector:

| USB Wire | Color | Pico GPIO | Pico Physical Pin |
|---|---|---|---|
| VBUS (5V power) | Red | VBUS | **Pin 40** |
| GND | Black | GND | **Pin 38** |
| D− (Data Minus) | White | GPIO 14 | **Pin 19** |
| D+ (Data Plus) | Green | GPIO 15 | **Pin 20** |

> **Note:** D+ is on GPIO 15 and D− is automatically assigned to GPIO 14 (DP−1) by the `PIO_USB_PINOUT_DMDP` configuration.

---

## Source File Reference

| File | Purpose |
|---|---|
| `main.c` | Entry point. Initialises all modules, launches Core 1, runs the Core 0 main loop. |
| `hid_bridge.c/.h` | Thread-safe ring-buffer queue between the USB Host (Core 1) and USB Device (Core 0). Uses a hardware spin-lock for safety. |
| `capture.c/.h` | Records every HID report into an 8 KB RAM buffer. Flushes to a 60 KB circular region of flash in page-aligned 256-byte chunks. Persists the write pointer in a separate config flash sector (magic `0xDEADBEEF`). |
| `inject.c/.h` | Converts ASCII text to HID keyboard reports and replays them via `tud_hid_keyboard_report()` with configurable per-keystroke delay and random jitter. |
| `wifi_server.c/.h` | Initialises CYW43 Wi-Fi in async mode. Binds an lwIP raw TCP socket on port 80. Serves the HTML dashboard and handles `POST /clear` and `POST /type` endpoints. |
| `usb_descriptors.c/.h` | Maintains mutable VID/PID and string buffers. Clones the target keyboard's identity. Sets `g_reenumerate` flag to trigger a 250ms USB disconnect/reconnect cycle on Core 0. |
| `tusb_config.h` | TinyUSB configuration. CDC is fully removed. Only one HID interface with Boot Protocol. |
| `lwipopts.h` | lwIP tuning. `TCP_SND_BUF = 65000` to prevent `ERR_MEM` when sending the 60 KB flash dump. `LWIP_DISABLE_TCP_SANITY_CHECKS = 1` to suppress the pool-size sanity warning. |

---

## Core Architecture

| Core | Role | Key Functions |
|---|---|---|
| **Core 0** | USB Device (to PC) + Wi-Fi + Injection | `tud_task`, `bridge_task`, `capture_task`, `inject_task`, `wifi_server_task` |
| **Core 1** | USB Host (from keyboard, PIO-USB) | `tuh_task` — timing-critical, must run on Core 1 |

### Boot Sequence (Core 0)
```
board_init()
bridge_init()
capture_init()       ← reads flash write-pointer from config sector
inject_init()        ← builds ASCII→HID keycode lookup table
tud_init()           ← USB Device starts (enumerates to PC as keyboard)
multicore_launch_core1()
sleep_ms(100)        ← let Core 1 start PIO-USB
wifi_server_init()   ← cyw43 init, enable STA, connect async, bind TCP port 80
[main loop]
```

---

## Flash Memory Layout (Pico 2 W = 4 MB)

```
0x000000 ──────────────────────────── Firmware (XIP)
         ...
0x3F0000 ── Config Sector (4 KB)    ← write-pointer + magic 0xDEADBEEF
0x3F1000 ── Capture Buffer (60 KB)  ← raw keystroke ASCII, circular
0x400000 ── End of Flash
```

Flash is only written in 256-byte pages after sector-erasing in 4 KB blocks. Both operations require Core 1 to be stalled via `multicore_lockout_start_blocking()`.

---

## HTTP API

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | Returns the full HTML dashboard with all captured keystrokes inline. |
| `POST` | `/clear` | Calls `capture_clear()` to erase flash and RAM buffer. Redirects to `/`. |
| `POST` | `/type?s=<ms>&j=<0\|1>` | Starts injection. `s` = delay ms per key, `j` = enable random jitter. Request body = plain text to type. |

---

## Wi-Fi Connection State Machine

The Pico connects to Wi-Fi without blocking the main loop:

```
wifi_server_init()
  └─ cyw43_arch_wifi_connect_async()   ← fires and forgets

wifi_server_task()   [called every loop iteration]
  ├─ cyw43_arch_poll()                 ← drives the CYW43 driver
  └─ if not connected:
       ├─ check cyw43_tcpip_link_status()
       ├─ if LINK_UP → mark connected ✓
       └─ if FAIL/timeout → retry connect_async every 5 seconds
```

The TCP server is bound **before** Wi-Fi connects, so it is always ready the moment an IP is assigned.

---

## USB Identity Spoofing Sequence

```
1. Keyboard plugged in (Core 1 detects via tuh_hid_mount_cb)
2. tuh_vid_pid_get() reads real VID/PID
3. usb_descriptors_set_keyboard_id() stores VID/PID/Manufacturer/Product
4. g_reenumerate = true
5. Core 0 detects flag → calls tud_disconnect()
6. Waits 250 ms (PC considers device removed)
7. Calls tud_connect() → PC enumerates the Pico as the target keyboard
```

---

## Build & Flash Reference

```powershell
# Build only
$env:PICO_SDK_PATH = "E:\...\pico-sdk-2.1.0"
cd "E:\...\MEDIATOR\build"
ninja

# One-click build + flash
& "E:\...\MEDIATOR\build_and_flash.ps1"
```

**BOOTSEL mode:** Hold BOOTSEL button → plug USB → release → press Enter in PowerShell.

---

## Changing Wi-Fi Credentials

Edit `src/wifi_server.c`:
```c
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"
```
Rebuild and reflash.

---

## Known Limitations

- Injection only supports standard ASCII printable characters. Non-ASCII Unicode is skipped.
- Flash capture buffer is 60 KB. Once full, it wraps circularly (oldest data overwritten).
- The TCP connection close logic relies on `tcp_sndbuf == TCP_SND_BUF` to detect a fully drained send buffer — this works reliably but may cause a slight delay before the browser sees `Connection: close`.

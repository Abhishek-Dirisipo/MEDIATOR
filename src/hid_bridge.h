#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"

// Inter-core ring buffer for raw HID keyboard reports.
// Core 1 (USB host) pushes reports; Core 0 (USB device) pops and forwards.

void bridge_init(void);

// Push a raw HID keyboard report (called from Core 1 / host callback).
// Thread-safe via spin_lock. Returns false if the queue is full.
bool bridge_push_report(hid_keyboard_report_t const *report);

// Push a press+release pair to the inject queue (called from inject_task on Core 0).
// bridge_task sends inject queue items with priority over physical keyboard.
// Returns false if the inject queue is full — caller should retry next iteration.
bool bridge_inject_push_pair(hid_keyboard_report_t const *press,
                              hid_keyboard_report_t const *release);

// Called from Core 0 main loop: pops one queued report and sends it
// to the PC via tud_hid_keyboard_report().
void bridge_task(void);

// Forward LED state from PC back to physical keyboard.
void bridge_set_leds(uint8_t led_state);

// Get current pending LED state (polled by host side).
bool bridge_get_pending_leds(uint8_t *led_state);

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"

// Initialize capture subsystem. Always active — no jumper required.
void capture_init(void);

// Call in main loop — flushes RAM buffer to flash when 75% full.
void capture_task(void);

// Returns true (always capturing).
bool capture_is_active(void);

// Force immediate flush of RAM buffer to flash.
void capture_flush(void);

// Record a raw HID keyboard report, decode to text, append to buffer.
void capture_record_report(hid_keyboard_report_t const *report);

// Erase only the circular capture buffer (persistent zone untouched).
void capture_clear(void);

// Erase only the persistent zone.
void capture_clear_persist(void);

// Stream circular capture data to a TCP connection (HTTP server).
void capture_stream_to_tcp(struct tcp_pcb *tpcb);

// Stream persistent zone data to a TCP connection (HTTP server).
void capture_stream_persist_to_tcp(struct tcp_pcb *tpcb);

// Get byte counts for status display.
uint32_t capture_get_write_ptr(void);
uint32_t capture_get_total(void);

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Set by Core 1 when keyboard connects; Core 0 polls and re-enumerates
extern volatile bool g_reenumerate;

// Update VID/PID + name strings from the real keyboard.
// Called from Core 1 (tuh_hid_mount_cb). Sets g_reenumerate = true if changed.
bool usb_descriptors_set_keyboard_id(uint16_t vid, uint16_t pid,
                                     const char *manufacturer,
                                     const char *product,
                                     const char *serial);

// Get currently active VID/PID
void usb_descriptors_get_keyboard_id(uint16_t *vid, uint16_t *pid);

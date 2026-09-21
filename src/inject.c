#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "inject.h"
#include "hid_bridge.h"   // for bridge_inject_push_pair

#define INJ_BUF_SIZE 16384   // 16 KB - enough for large scripts

static char inj_buf[INJ_BUF_SIZE];
static uint32_t inj_len = 0;
static uint32_t inj_pos = 0;
static uint32_t inj_speed = 50;
static bool inj_jitter = false;
static uint32_t next_action_time = 0;

typedef struct {
    uint8_t keycode;
    bool shift;
} a2h_t;

static a2h_t a2h[128];

static const uint8_t keycode2ascii[128][2] = {HID_KEYCODE_TO_ASCII};

void inject_init(void) {
    memset(a2h, 0, sizeof(a2h));
    for (uint8_t i = 0; i < 128; i++) {
        uint8_t u = keycode2ascii[i][0];
        uint8_t s = keycode2ascii[i][1];
        if (u && u < 128) { a2h[u].keycode = i; a2h[u].shift = false; }
        if (s && s < 128) { a2h[s].keycode = i; a2h[s].shift = true; }
    }
    // Hardcode keys TinyUSB's table may leave as 0
    a2h['\n'].keycode = HID_KEY_ENTER; a2h['\n'].shift = false;
    a2h['\r'].keycode = HID_KEY_ENTER; a2h['\r'].shift = false;
    a2h['\t'].keycode = HID_KEY_TAB;   a2h['\t'].shift = false;

    // ---------------------------------------------------------------
    // TinyUSB's table has NUMPAD entries (0x59-0x63) that also map
    // to '0'-'9', '.', etc.  The loop above processes them AFTER the
    // number-row entries (0x1E-0x27) and overwrites them.
    // With Num Lock OFF, numpad keycodes produce navigation keys (End,
    // Home, etc.) instead of digits - so inject types garbage.
    // Force the correct US-layout number-row and symbol keycodes here.
    // ---------------------------------------------------------------
    a2h['1'].keycode = HID_KEY_1;      a2h['1'].shift = false;
    a2h['2'].keycode = HID_KEY_2;      a2h['2'].shift = false;
    a2h['3'].keycode = HID_KEY_3;      a2h['3'].shift = false;
    a2h['4'].keycode = HID_KEY_4;      a2h['4'].shift = false;
    a2h['5'].keycode = HID_KEY_5;      a2h['5'].shift = false;
    a2h['6'].keycode = HID_KEY_6;      a2h['6'].shift = false;
    a2h['7'].keycode = HID_KEY_7;      a2h['7'].shift = false;
    a2h['8'].keycode = HID_KEY_8;      a2h['8'].shift = false;
    a2h['9'].keycode = HID_KEY_9;      a2h['9'].shift = false;
    a2h['0'].keycode = HID_KEY_0;      a2h['0'].shift = false;

    // Symbols also contaminated by numpad or non-US keys:
    a2h['.'].keycode = HID_KEY_PERIOD;   a2h['.'].shift = false; // 0x37, not numpad (0x63)
    a2h['/'].keycode = HID_KEY_SLASH;    a2h['/'].shift = false; // 0x38, not numpad (0x54)
    a2h['-'].keycode = HID_KEY_MINUS;    a2h['-'].shift = false; // 0x2D, not numpad (0x56)
    a2h['='].keycode = HID_KEY_EQUAL;    a2h['='].shift = false; // 0x2E, not numpad (0x67)
    a2h['*'].keycode = HID_KEY_8;        a2h['*'].shift = true;  // SHIFT+8, not numpad (0x55)
    a2h['+'].keycode = HID_KEY_EQUAL;    a2h['+'].shift = true;  // SHIFT+=, not numpad (0x57)
    a2h['#'].keycode = HID_KEY_3;        a2h['#'].shift = true;  // SHIFT+3, not non-US (0x32)
    a2h['~'].keycode = HID_KEY_GRAVE;    a2h['~'].shift = true;  // SHIFT+`, not non-US (0x32/0x35)
}

void inject_start(const char *text, uint32_t len, uint32_t speed_ms, bool jitter) {
    if (len > INJ_BUF_SIZE) len = INJ_BUF_SIZE;
    memcpy(inj_buf, text, len);
    inj_len = len;
    inj_pos = 0;
    inj_speed = speed_ms;
    inj_jitter = jitter;
    next_action_time = to_ms_since_boot(get_absolute_time());
    // Flush stale physical reports and schedule a zero-report so the PC
    // sees all physical keys released before the first injected keycode.
    bridge_pre_inject();
}

bool inject_is_active(void) {
    return inj_pos < inj_len;
}

void inject_task(void) {
    if (!inject_is_active()) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now < next_action_time) return;

    // Skip carriage returns silently (browser may send \r\n)
    while (inj_pos < inj_len && (uint8_t)inj_buf[inj_pos] == '\r') inj_pos++;
    if (!inject_is_active()) return;

    char c = inj_buf[inj_pos];
    uint8_t kc = 0, mod = 0;
    if ((uint8_t)c < 128) {
        kc = a2h[(uint8_t)c].keycode;
        if (a2h[(uint8_t)c].shift) mod = KEYBOARD_MODIFIER_LEFTSHIFT;
    }

    hid_keyboard_report_t press = {0};
    press.modifier = mod;
    if (kc) press.keycode[0] = kc;
    hid_keyboard_report_t release = {0};

    // Push press+release pair to bridge inject queue.
    // bridge_task owns all tud_hid_keyboard_report calls - no direct calls here.
    // If queue is full, retry next loop (no stall watchdog needed).
    if (bridge_inject_push_pair(&press, &release)) {
        inj_pos++;
        uint32_t wait = inj_speed;
        if (inj_jitter && wait > 10) wait += (rand() % (wait / 2));
        next_action_time = now + wait;
    }
}

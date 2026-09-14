#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "hid_bridge.h"
#include "inject.h"

//--------------------------------------------------------------------+
// Physical keyboard ring buffer (Core 1 → Core 0)
//--------------------------------------------------------------------+
#define BRIDGE_QUEUE_SIZE 16

typedef struct {
    hid_keyboard_report_t reports[BRIDGE_QUEUE_SIZE];
    volatile uint8_t      head;
    volatile uint8_t      tail;
    spin_lock_t          *lock;
    volatile uint8_t      led_state;
    volatile bool         led_pending;
} bridge_t;

static bridge_t g_bridge;

//--------------------------------------------------------------------+
// Inject queue — owned entirely by Core 0, no cross-core access.
// Holds press+release pairs queued by inject_task; bridge_task sends
// them with priority over physical keyboard reports.
//--------------------------------------------------------------------+
#define INJECT_QUEUE_SIZE 8  // 4 press+release pairs max outstanding

static hid_keyboard_report_t g_inj_q[INJECT_QUEUE_SIZE];
static uint8_t g_inj_head = 0;
static uint8_t g_inj_tail = 0;

void bridge_init(void) {
    memset(&g_bridge, 0, sizeof(g_bridge));
    int lock_num = spin_lock_claim_unused(true);
    g_bridge.lock = spin_lock_init(lock_num);
    g_inj_head = 0;
    g_inj_tail = 0;
}

//--------------------------------------------------------------------+
// Core 1 (host side) → push incoming keyboard report
//--------------------------------------------------------------------+
bool bridge_push_report(hid_keyboard_report_t const *report) {
    uint32_t save = spin_lock_blocking(g_bridge.lock);
    uint8_t next = (g_bridge.tail + 1) % BRIDGE_QUEUE_SIZE;
    bool ok = false;
    if (next != g_bridge.head) {
        g_bridge.reports[g_bridge.tail] = *report;
        g_bridge.tail = next;
        ok = true;
    }
    spin_unlock(g_bridge.lock, save);
    return ok;
}

//--------------------------------------------------------------------+
// Core 0 only — push press+release pair to inject queue
//--------------------------------------------------------------------+
bool bridge_inject_push_pair(hid_keyboard_report_t const *press,
                              hid_keyboard_report_t const *release) {
    // Both slots must fit — check before writing
    uint8_t n1 = (g_inj_tail + 1) % INJECT_QUEUE_SIZE;
    uint8_t n2 = (n1      + 1) % INJECT_QUEUE_SIZE;
    if (n1 == g_inj_head || n2 == g_inj_head) return false; // full

    g_inj_q[g_inj_tail] = *press;
    g_inj_tail = n1;
    g_inj_q[g_inj_tail] = *release;
    g_inj_tail = n2;
    return true;
}

//--------------------------------------------------------------------+
// Core 0 (device side) — forward queued reports to PC
//
// Priority order:
//   1. Inject queue (press/release from inject_task)
//   2. Physical keyboard queue — ONLY when inject not active
//
// Physical keyboard is drained (discarded) while inject is active to
// prevent physical zero-reports from cancelling injected keypresses.
//--------------------------------------------------------------------+
void bridge_task(void) {
    if (!tud_hid_ready()) return;

    // --- Inject queue (highest priority) ---
    if (g_inj_head != g_inj_tail) {
        hid_keyboard_report_t r = g_inj_q[g_inj_head];
        g_inj_head = (g_inj_head + 1) % INJECT_QUEUE_SIZE;
        // Also drain physical queue to prevent interleaving
        uint32_t save = spin_lock_blocking(g_bridge.lock);
        g_bridge.head = g_bridge.tail;
        spin_unlock(g_bridge.lock, save);
        tud_hid_keyboard_report(0, r.modifier, r.keycode);
        return;
    }

    // --- While inject is generating (but inject queue momentarily empty) ---
    // Drain physical queue so no stale reports sneak through.
    if (inject_is_active()) {
        uint32_t save = spin_lock_blocking(g_bridge.lock);
        g_bridge.head = g_bridge.tail;
        spin_unlock(g_bridge.lock, save);
        return;
    }

    // --- Normal physical keyboard forwarding ---
    uint32_t save = spin_lock_blocking(g_bridge.lock);
    bool has = (g_bridge.head != g_bridge.tail);
    hid_keyboard_report_t report;
    if (has) {
        report = g_bridge.reports[g_bridge.head];
        g_bridge.head = (g_bridge.head + 1) % BRIDGE_QUEUE_SIZE;
    }
    spin_unlock(g_bridge.lock, save);

    if (has) {
        tud_hid_keyboard_report(0, report.modifier, report.keycode);
    }
}

//--------------------------------------------------------------------+
// LED state: PC → Core 0 → bridge → Core 1 → keyboard
//--------------------------------------------------------------------+
void bridge_set_leds(uint8_t led_state) {
    uint32_t save = spin_lock_blocking(g_bridge.lock);
    g_bridge.led_state   = led_state;
    g_bridge.led_pending = true;
    spin_unlock(g_bridge.lock, save);
}

bool bridge_get_pending_leds(uint8_t *led_state) {
    uint32_t save = spin_lock_blocking(g_bridge.lock);
    bool pending = g_bridge.led_pending;
    if (pending) {
        *led_state = g_bridge.led_state;
        g_bridge.led_pending = false;
    }
    spin_unlock(g_bridge.lock, save);
    return pending;
}

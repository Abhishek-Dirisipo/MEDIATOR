#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------+
// Mutable VID/PID — updated when real keyboard is detected
//--------------------------------------------------------------------+
static uint16_t g_vid = 0xCafe;
static uint16_t g_pid = 0x4001;
static char     g_manufacturer_str[64] = "Generic";
static char     g_product_str[64]      = "USB Keyboard";
static char     g_serial_str[64]       = ""; // No serial by default unless cloned

// Volatile flag: set by Core 1, polled by Core 0 to trigger re-enum
volatile bool g_reenumerate = false;

bool usb_descriptors_set_keyboard_id(uint16_t vid, uint16_t pid,
                                     const char *manufacturer,
                                     const char *product,
                                     const char *serial) {
    if (g_vid == vid && g_pid == pid) return false; // no change
    g_vid = vid;
    g_pid = pid;
    if (manufacturer) {
        strncpy(g_manufacturer_str, manufacturer, sizeof(g_manufacturer_str) - 1);
        g_manufacturer_str[sizeof(g_manufacturer_str) - 1] = '\0';
    }
    if (product) {
        strncpy(g_product_str, product, sizeof(g_product_str) - 1);
        g_product_str[sizeof(g_product_str) - 1] = '\0';
    }
    if (serial) {
        strncpy(g_serial_str, serial, sizeof(g_serial_str) - 1);
        g_serial_str[sizeof(g_serial_str) - 1] = '\0';
    } else {
        g_serial_str[0] = '\0';
    }
    g_reenumerate = true; // tell Core 0 to re-enumerate
    return true;
}

void usb_descriptors_get_keyboard_id(uint16_t *vid, uint16_t *pid) {
    *vid = g_vid;
    *pid = g_pid;
}

//--------------------------------------------------------------------+
// 1. HID Report Descriptor — Standard Boot Keyboard
//--------------------------------------------------------------------+
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

//--------------------------------------------------------------------+
// 2. Device Descriptor — pure HID (no CDC, no IAD)
//--------------------------------------------------------------------+
static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,  // class defined at interface level (pure HID)
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,   // overwritten in callback
    .idProduct          = 0x4001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

//--------------------------------------------------------------------+
// 3. Configuration Descriptor — HID only, one interface
//--------------------------------------------------------------------+
#define EPNUM_HID     0x81

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1 /*num_itf*/, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0 /*itf*/, 4 /*str_idx*/, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 5 /*bInterval ms*/)
};

//--------------------------------------------------------------------+
// 4. String Descriptors
//--------------------------------------------------------------------+

// Only the language ID is static; the rest are generated on the fly.
static uint16_t _desc_str[64];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count = 0;

    if (index == 0) {
        // Supported language is English (0x0409)
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        const char *str = NULL;
        if (index == 1) str = g_manufacturer_str;
        else if (index == 2) str = g_product_str;
        else if (index == 3 && g_serial_str[0] != '\0') str = g_serial_str;
        else if (index == 4) str = "Keyboard";

        if (str == NULL) return NULL; // No string for this index

        chr_count = strlen(str);
        if (chr_count > 63) chr_count = 63;

        // Convert ASCII to UTF-16
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}

//--------------------------------------------------------------------+
// TinyUSB Device Callbacks
//--------------------------------------------------------------------+
uint8_t const *tud_descriptor_device_cb(void) {
    desc_device.idVendor  = g_vid;
    desc_device.idProduct = g_pid;
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

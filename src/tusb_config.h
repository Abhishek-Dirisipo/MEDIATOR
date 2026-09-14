#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board-level port assignments
//--------------------------------------------------------------------+
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0   // Native USB controller -> PC (Device)
#endif
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT      1   // PIO-USB -> Keyboard (Host)
#endif
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------+
// Common
//--------------------------------------------------------------------+
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

//--------------------------------------------------------------------+
// DEVICE (native USB -> PC)
//--------------------------------------------------------------------+
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       BOARD_TUD_MAX_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

// Stealth: PC sees ONLY a HID keyboard - no CDC, no IAD, nothing extra
#define CFG_TUD_CDC             0
#define CFG_TUD_HID             1

#define CFG_TUD_HID_EP_BUFSIZE  8

//--------------------------------------------------------------------+
// HOST (PIO-USB -> physical keyboard)
//--------------------------------------------------------------------+
#define CFG_TUH_ENABLED         1
#define CFG_TUH_MAX_SPEED       BOARD_TUH_MAX_SPEED

// Use PIO-USB as the host controller on RP2040/RP2350
#if CFG_TUSB_MCU == OPT_MCU_RP2040 || CFG_TUSB_MCU == OPT_MCU_RP2350
#define CFG_TUH_RPI_PIO_USB     1
#endif

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB              0
#define CFG_TUH_DEVICE_MAX       1  // one keyboard at a time
#define CFG_TUH_HID              4  // up to 4 HID interfaces (keyboard may expose >1)
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

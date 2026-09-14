cmake_minimum_required(VERSION 3.13)

# NOTE: Do NOT set PICO_PIO_USB_PATH here.
# Setting it causes the TinyUSB family.cmake to call pico_generate_pio_header()
# which triggers an ExternalProject build of pioasm from C++ source — which fails
# without a Windows host C++ compiler. Instead, we wire PIO-USB manually below
# using the pre-generated usb_tx.pio.h / usb_rx.pio.h that ship in the repo.

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_usb_bridge C CXX ASM)

pico_sdk_init()

# ---------------------------------------------------------------------------
# PIO-USB manual integration (avoids pioasm / ExternalProject)
# The Pico-PIO-USB repo ships usb_tx.pio.h and usb_rx.pio.h pre-generated
# inside its src/ directory, so no pio compilation step is needed.
# ---------------------------------------------------------------------------
set(PIO_USB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../Pico-PIO-USB/src")

add_library(pio_usb_integrated INTERFACE)
target_sources(pio_usb_integrated INTERFACE
    ${PIO_USB_SRC}/pio_usb.c
    ${PIO_USB_SRC}/pio_usb_host.c
    ${PIO_USB_SRC}/usb_crc.c
    # hcd_pio_usb.c from TinyUSB — the host-controller driver that sits between
    # TinyUSB host stack and the Pico-PIO-USB library
    ${CMAKE_CURRENT_SOURCE_DIR}/../pico-sdk-2.1.0/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
target_include_directories(pio_usb_integrated INTERFACE ${PIO_USB_SRC})
target_compile_definitions(pio_usb_integrated INTERFACE PIO_USB_USE_TINYUSB)
target_link_libraries(pio_usb_integrated INTERFACE hardware_dma hardware_pio)

# ---------------------------------------------------------------------------
# Main firmware target
# ---------------------------------------------------------------------------
add_executable(bridge
    src/main.c
    src/usb_descriptors.c
    src/hid_bridge.c
    src/capture.c
    src/wifi_server.c
    src/inject.c
)

target_compile_definitions(bridge PRIVATE
    PIO_USB_DP_PIN_DEFAULT=14   # Default overridden in code anyway
    BOARD_TUD_RHPORT=0          # Native USB -> PC  (Device)
    BOARD_TUH_RHPORT=1          # PIO-USB  -> Keyboard (Host)
)

target_link_libraries(bridge
    pico_stdlib
    pico_multicore
    hardware_flash
    hardware_sync
    tinyusb_device
    tinyusb_host              # TinyUSB USB host stack
    tinyusb_board
    pio_usb_integrated        # our manual PIO-USB wiring
    pico_cyw43_arch_lwip_poll # Wi-Fi and TCP/IP (Polling mode)
)

pico_enable_stdio_usb(bridge 0)
pico_enable_stdio_uart(bridge 0)

target_include_directories(bridge PRIVATE src)

pico_add_extra_outputs(bridge)

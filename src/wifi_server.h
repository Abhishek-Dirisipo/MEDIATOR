#pragma once

// Initialize Wi-Fi (STA mode) and start the HTTP server on port 80.
// SSID/password are compiled in via WIFI_SSID / WIFI_PASSWORD defines.
void wifi_server_init(void);

// Poll Wi-Fi and lwIP stack — call every main loop iteration.
void wifi_server_task(void);

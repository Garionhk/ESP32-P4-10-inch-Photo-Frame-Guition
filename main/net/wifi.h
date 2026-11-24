// ===========================================================================
// wifi.h -- ESP32-C6 radio over esp_hosted, plus SNTP
// ===========================================================================
// The P4 has no radio. Wi-Fi is an ESP32-C6-MINI on SDMMC slot 1, reached
// through esp_hosted, and every call below goes over that link.
//
// Failure is never fatal, following the CYD project's rule: a frame with no
// network is still a frame. It shows photos, and sun/sunset stay correct
// because they are local math.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "esp_netif.h"

// Bring up the C6 link. Returns an error if the co-processor never answers,
// which is a hardware/firmware problem rather than a credentials one.
esp_err_t wifi_begin(void);

// Scan for networks and log them. Needs no credentials, so it is the honest
// way to prove the radio works before blaming a password.
esp_err_t wifi_scan(void);

// Connect using the credentials in settings. Non-blocking; watch wifi_ready().
esp_err_t wifi_connect(void);

bool wifi_ready(void);          // associated and holding an IP
bool wifi_connecting(void);     // trying, but not there yet -- distinct from offline
const char *wifi_ip(void);      // "0.0.0.0" until associated
int  wifi_rssi(void);

// Start SNTP once associated, and apply the configured POSIX TZ.
void wifi_start_sntp(void);
bool wifi_time_valid(void);

// Both interfaces exist from wifi_begin() onward -- see the comment there for
// why the AP one cannot be created lazily.
esp_netif_t *wifi_netif_ap(void);
esp_netif_t *wifi_netif_sta(void);

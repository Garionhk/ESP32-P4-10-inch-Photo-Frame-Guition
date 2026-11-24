// ===========================================================================
// httpd.h -- settings web UI, in both STA and AP modes
// ===========================================================================
// The server runs whenever the device is powered, not only during setup:
//
//   STA connected -> settings page on the LAN IP, e.g. http://192.168.1.42
//   no WiFi       -> SoftAP + captive portal on http://192.168.4.1
//
// which is why the overlay prints the address. Without it there is no way to
// find the page short of a serial monitor or the router's client list.
//
// Carried over from the CYD project, including the reason: re-flashing to
// change a setting is a poor repair path for something hanging on a wall, and
// a binary built from a header full of secrets carries those secrets.
#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t webui_begin(void);        // serve on whatever interface is up
void      webui_stop(void);

// Bring up the setup access point. Open on purpose: it is the recovery path
// for a device whose stored network no longer exists.
esp_err_t webui_start_setup_ap(void);
void      webui_stop_setup_ap(void);

bool        webui_is_ap(void);
const char *webui_address(void);    // what to print on screen
const char *webui_ap_ssid(void);    // "PhotoFrame-A1B2"

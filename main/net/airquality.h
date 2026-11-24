// ===========================================================================
// airquality.h -- Open-Meteo US AQI + PM2.5
// ===========================================================================
// Same last-good and backoff semantics as weather, on its own 30 min cycle
// and a different host. Runs sequentially with the weather fetch, never
// concurrently: two TLS sessions at once is the sort of thing that shows up
// as an out-of-memory reboot a week later.
#pragma once

void airquality_begin(void);
void airquality_tick(void);

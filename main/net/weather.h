// ===========================================================================
// weather.h -- Open-Meteo current conditions
// ===========================================================================
// Ported from the CYD project, keeping its two rules:
//
//   * No API key. Open-Meteo needs none, so there is nothing for the user to
//     sign up for and nothing secret in the binary.
//   * A failed fetch NEVER clears last-good data. It only stops refreshing
//     weatherUpdatedAt, so the overlay keeps showing the last reading and
//     staleness is visible instead of being displayed as a zero.
#pragma once

void weather_begin(void);   // arm the scheduler; first fetch happens asap
void weather_tick(void);    // call periodically; fetches when one is due

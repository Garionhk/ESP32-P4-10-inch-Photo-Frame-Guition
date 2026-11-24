// ===========================================================================
// app_data.h -- the single global data model
// ===========================================================================
// Ported from the CYD project, and the rule that matters comes with it:
//
//   Written only by the fetch layer, read only by the display layer. A failed
//   fetch NEVER clears existing values; it only stops refreshing *UpdatedAt,
//   so the overlay always renders last-good data and staleness is visible
//   rather than being shown as a zero.
//
// C has no default member initialisers, but the single global instance is
// zero-initialised, which leaves every *Valid flag false -- the same starting
// state the C++ original had.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    // ---- weather ----
    float    tempC, feelsLikeC;
    float    tempMaxC, tempMinC;      // today's forecast high/low
    bool     dailyValid;              // daily block seen at least once
    int      weatherCode;
    int      cloudCoverPct, humidityPct;
    float    windKph, pressureHpa, pressureTrend;
    uint32_t weatherUpdatedAt;        // epoch secs; 0 = never
    bool     weatherValid;

    // ---- air quality ----
    int      aqi, pm25;
    uint32_t aqiUpdatedAt;
    bool     aqiValid;

    // ---- uv ----
    float    uvIndex;
    uint32_t uvUpdatedAt;
    bool     uvValid;

    // ---- sun/moon: computed locally, so correct with the network down ----
    time_t   sunriseToday, sunsetToday;
    time_t   sunriseTomorrow, sunsetTomorrow;
    bool     showingNextDay;
    float    moonPhase;               // 0.0-1.0
    float    moonIlluminationPct;
    time_t   moonrise, moonset;
} AppData;

extern AppData g_data;

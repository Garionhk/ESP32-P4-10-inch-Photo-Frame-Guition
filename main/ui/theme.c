#include "theme.h"
#include "settings.h"

// Verbatim from the design doc's theme table. Each row is
// [t1, acc, dim, deep, bg].
const theme_t THEMES[THEME_COUNT] = {
    { "Ember orange", 0xFFCFA8, 0xFF8A3D, 0xB45D22, 0x6E3410, 0x0A0603 },
    { "Deep orange",  0xF0A265, 0xD65C11, 0x98430E, 0x5A2607, 0x080402 },
    { "Red ember",    0xFFC9BC, 0xFF5A36, 0xA83318, 0x5E1A0C, 0x0B0403 },
    { "Ice cyan",     0xC9E8F2, 0x5ABEDC, 0x2E7A92, 0x123C4A, 0x03080B },
    { "Mint green",   0xCDEFDC, 0x4FC98A, 0x25795A, 0x0E3A2A, 0x030906 },
    { "Violet",       0xE0D4F5, 0x9B7BE8, 0x5E43A0, 0x2E2050, 0x060410 },
    { "Paper mono",   0xF4F1EC, 0xBDB9B2, 0x8C8983, 0x4A4844, 0x0B0B0B },
};

const theme_t *theme_now(void)
{
    uint8_t i = g_settings.theme;
    if (i >= THEME_COUNT) i = 0;
    return &THEMES[i];
}

const char *theme_aqi_label(int aqi)
{
    if (aqi <= 50)  return "Good";
    if (aqi <= 100) return "Moderate";
    if (aqi <= 150) return "Sensitive";
    if (aqi <= 200) return "Unhealthy";
    if (aqi <= 300) return "Very bad";
    return "Hazardous";
}

// WMO codes as Open-Meteo returns them, collapsed to the short labels the
// design shows ("Mainly clear"). The cell allows 170 px, so these stay short.
const char *theme_weather_text(int code)
{
    switch (code) {
        case 0:  return "Clear";
        case 1:  return "Mainly clear";
        case 2:  return "Part cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Icy drizzle";
        case 61: case 63: case 65: return "Rain";
        case 66: case 67: return "Freezing rain";
        case 71: case 73: case 75: case 77: return "Snow";
        case 80: case 81: case 82: return "Showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunder, hail";
        default: return "--";
    }
}

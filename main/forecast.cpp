#include "forecast.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include "charts.h"
#include "config.h"
#include "net_time.h"
#include "quality.h"

namespace {

using forecast::Trend;

// FR-13.2: the 26 forecasts of the Zambretti forecaster.
constexpr const char *ZAMBRETTI[26] = {
    "Settled fine",                      // A
    "Fine weather",                      // B
    "Becoming fine",                     // C
    "Fine, becoming less settled",       // D
    "Fine, possible showers",            // E
    "Fairly fine, improving",            // F
    "Fairly fine, possible showers early",  // G
    "Fairly fine, showery later",        // H
    "Showery early, improving",          // I
    "Changeable, mending",               // J
    "Fairly fine, showers likely",       // K
    "Rather unsettled, clearing later",  // L
    "Unsettled, probably improving",     // M
    "Showery, bright intervals",         // N
    "Showery, becoming less settled",    // O
    "Changeable, some rain",             // P
    "Unsettled, short fine intervals",   // Q
    "Unsettled, rain later",             // R
    "Unsettled, some rain",              // S
    "Mostly very unsettled",             // T
    "Occasional rain, worsening",        // U
    "Rain at times, very unsettled",     // V
    "Rain at frequent intervals",        // W
    "Rain, very unsettled",              // X
    "Stormy, may improve",               // Y
    "Stormy, much rain",                 // Z
};

// The forecast number Z of each direction, from the sea-level pressure P in
// hPa: falling Z = 127 - 0.12 P (1 .. 9), steady Z = 144 - 0.13 P (10 .. 19),
// rising Z = 185 - 0.16 P (20 .. 32); each Z stands for one letter.
constexpr const char FALLING_CODES[] = "ABDHORUXZ";
constexpr const char STEADY_CODES[]  = "ABEKNPSWXZ";
constexpr const char RISING_CODES[]  = "ABCFGIJLMQTYZ";

char zambretti(float p, int direction)
{
    p = std::fmin(std::fmax(p, config::FORECAST_MIN_HPA), config::FORECAST_MAX_HPA);
    const char *codes;
    int first;
    float z;
    if (direction < 0) {
        codes = FALLING_CODES, first = 1, z = 127.0f - 0.12f * p;
    } else if (direction > 0) {
        codes = RISING_CODES, first = 20, z = 185.0f - 0.16f * p;
    } else {
        codes = STEADY_CODES, first = 10, z = 144.0f - 0.13f * p;
    }
    const int last = first + static_cast<int>(strlen(codes)) - 1;
    int n = static_cast<int>(lroundf(z));
    n = n < first ? first : n > last ? last : n;
    return codes[n - first];
}

Trend classify(float change, int &direction)
{
    const float a = std::fabs(change);
    if (a < config::TREND_STEADY_HPA) {
        direction = 0;
        return Trend::STEADY;
    }
    direction = change > 0 ? 1 : -1;
    const int level = a < config::TREND_SLOW_HPA ? 0 : a < config::TREND_QUICK_HPA ? 1
                    : a <= config::TREND_VERY_RAPID_HPA ? 2 : 3;
    constexpr Trend RISING[] = {Trend::RISING_SLOWLY, Trend::RISING, Trend::RISING_QUICKLY,
                                Trend::RISING_VERY_RAPIDLY};
    constexpr Trend FALLING[] = {Trend::FALLING_SLOWLY, Trend::FALLING, Trend::FALLING_QUICKLY,
                                 Trend::FALLING_VERY_RAPIDLY};
    return direction > 0 ? RISING[level] : FALLING[level];
}

}  // namespace

namespace forecast {

bool evaluate(float pressure_msl_hpa, Outlook &out)
{
    struct tm local;
    float delta = 0;
    if (!net_time::local_time(local) ||
        !charts::pressure_change(config::TREND_SPAN_S, time(nullptr), delta)) {
        return false;
    }
    // The buckets hold the station pressure; the sea-level factor is constant.
    out.change_hpa = delta * quality::msl_factor();
    out.trend = classify(out.change_hpa, out.direction);
    out.code = zambretti(pressure_msl_hpa, out.direction);
    out.text = ZAMBRETTI[out.code - 'A'];
    return true;
}

const char *trend_name(Trend t)
{
    switch (t) {
    case Trend::STEADY:               return "steady";
    case Trend::RISING_SLOWLY:        return "rising_slowly";
    case Trend::RISING:               return "rising";
    case Trend::RISING_QUICKLY:       return "rising_quickly";
    case Trend::RISING_VERY_RAPIDLY:  return "rising_very_rapidly";
    case Trend::FALLING_SLOWLY:       return "falling_slowly";
    case Trend::FALLING:              return "falling";
    case Trend::FALLING_QUICKLY:      return "falling_quickly";
    case Trend::FALLING_VERY_RAPIDLY: return "falling_very_rapidly";
    }
    return "steady";
}

}  // namespace forecast

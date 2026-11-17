// Pressure tendency and a simple weather forecast (FR-13.1, FR-13.2).
// The tendency is the change of the sea-level pressure over the last 3 hours,
// taken from the chart buckets (charts.cpp); the forecast is the Zambretti
// forecaster, from the sea-level pressure and the direction of the tendency.
// Both are derived where they are shown or sent, like the dew point.
#pragma once

namespace forecast {

// FR-13.1: class of the tendency, after the UK Met Office.
enum class Trend {
    STEADY,
    RISING_SLOWLY, RISING, RISING_QUICKLY, RISING_VERY_RAPIDLY,
    FALLING_SLOWLY, FALLING, FALLING_QUICKLY, FALLING_VERY_RAPIDLY,
};

struct Outlook {
    float change_hpa;  // sea-level pressure change over TREND_SPAN_S
    Trend trend;
    int direction;     // -1 falling, 0 steady, +1 rising
    char code;         // FR-13.2: Zambretti letter, 'A' .. 'Z'
    const char *text;  // FR-13.2: forecast text, e.g. "Fine weather"
};

// Evaluates the tendency and the forecast for the current sea-level pressure.
// False while there is no tendency: clock not synchronized, less than 3 hours
// of measurements, or no recent measurement.
bool evaluate(float pressure_msl_hpa, Outlook &out);

// Name of the class for JSON, e.g. "rising_slowly".
const char *trend_name(Trend t);

}  // namespace forecast

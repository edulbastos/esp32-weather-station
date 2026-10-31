// Chart data for the web page (FR-8.4), kept ready in RAM.
// For each chart period (1 h, 24 h, 7 d) a table of WEB_CHART_POINTS time
// buckets holds the running averages of the measurements. Every measurement
// updates one bucket per table, so answering the page costs no flash access,
// whatever the measurement or log interval. The tables are rebuilt from the
// flash history at start-up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include "bme280.h"
#include "esp_err.h"

namespace charts {

struct Point {
    time_t time;  // average time of the measurements in the bucket
    float temperature_c;
    float humidity_rh;
    float pressure_hpa;
};

// Creates the lock. Call before the first add().
esp_err_t init();

// Adds a measurement taken at Unix time `t` (non-zero) to every table.
void add(time_t t, const Bme280Data &values);

// Fills the tables from the flash history (records with a time only).
void rebuild_from_history();

// Number of points of the table for this period, 0 if there is none.
uint32_t points_for(uint32_t hours);

// True if a table exists for this period and number of points.
bool serves(uint32_t hours, uint32_t points);

// Copies the non-empty buckets of the table for `hours`, oldest first, into
// `out` (room for WEB_CHART_MAX_POINTS) and returns their number. `now` is the
// current Unix time, or 0 if unknown (then the newest bucket ends the period).
size_t snapshot(uint32_t hours, time_t now, Point *out, uint32_t &bucket_s);

}  // namespace charts

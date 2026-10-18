#include "charts.h"

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "history.h"

static const char *TAG = "charts";

namespace {

constexpr size_t POINTS = config::WEB_CHART_POINTS;
constexpr size_t TIERS = sizeof(config::WEB_CHART_PERIODS_H) / sizeof(config::WEB_CHART_PERIODS_H[0]);

// Running averages of the measurements in one time bucket. The bucket with
// number `index` covers [index * bucket_s, (index + 1) * bucket_s).
struct Bucket {
    int32_t index;
    uint32_t n;      // 0 = empty
    float offset_s;  // average time within the bucket
    float temp, hum, press;
};

// A ring of POINTS buckets: bucket `index` lives in slot index % POINTS, so
// a new bucket replaces the one that just left the period.
struct Tier {
    uint32_t hours;
    uint32_t bucket_s;
    Bucket buckets[POINTS];
};

Tier s_tiers[TIERS];
SemaphoreHandle_t s_lock = nullptr;

void average_in(float &mean, float x, uint32_t n)
{
    mean += (x - mean) / n;  // incremental mean, stable in float
}

const Tier *find(uint32_t hours)
{
    for (const Tier &tier : s_tiers) {
        if (tier.hours == hours) {
            return &tier;
        }
    }
    return nullptr;
}

}  // namespace

namespace charts {

esp_err_t init()
{
    for (size_t i = 0; i < TIERS; i++) {
        s_tiers[i].hours = config::WEB_CHART_PERIODS_H[i];
        s_tiers[i].bucket_s = config::WEB_CHART_PERIODS_H[i] * 3600 / POINTS;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

void add(time_t t, const Bme280Data &values)
{
    if (t <= 0 || s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (Tier &tier : s_tiers) {
        const int32_t index = static_cast<int32_t>(t / tier.bucket_s);
        Bucket &b = tier.buckets[index % POINTS];
        if (b.n > 0 && b.index > index) {
            continue;  // older than what the slot holds now (rebuild after live data)
        }
        if (b.n == 0 || b.index != index) {
            b = Bucket{index, 0, 0, 0, 0, 0};
        }
        b.n++;
        average_in(b.offset_s, static_cast<float>(t - static_cast<time_t>(index) * tier.bucket_s), b.n);
        average_in(b.temp, values.temperature_c, b.n);
        average_in(b.hum, values.humidity_rh, b.n);
        average_in(b.press, values.pressure_hpa, b.n);
    }
    xSemaphoreGive(s_lock);
}

void rebuild_from_history()
{
    uint32_t longest_h = 0;
    for (const uint32_t h : config::WEB_CHART_PERIODS_H) {
        longest_h = h > longest_h ? h : longest_h;
    }
    const uint32_t last_n = static_cast<uint32_t>(static_cast<uint64_t>(longest_h) * 3600 * 1000 /
                                                  config::LOG_PERIOD_MS + 1);
    uint32_t used = 0;
    const esp_err_t err = history::visit(last_n, [&used](const history::Entry &e) {
        if (e.time != 0) {
            add(e.time, Bme280Data{e.temperature_c, e.humidity_rh, e.pressure_hpa});
            used++;
        }
        return true;
    });
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Chart tables rebuilt from %lu history records", static_cast<unsigned long>(used));
    } else {
        ESP_LOGW(TAG, "Chart tables not rebuilt: %s", esp_err_to_name(err));
    }
}

bool serves(uint32_t hours, uint32_t points)
{
    return points == POINTS && find(hours) != nullptr;
}

size_t snapshot(uint32_t hours, time_t now, Point *out, uint32_t &bucket_s)
{
    const Tier *tier = find(hours);
    if (tier == nullptr || s_lock == nullptr) {
        return 0;
    }
    bucket_s = tier->bucket_s;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int32_t newest = 0;
    if (now > 0) {
        newest = static_cast<int32_t>(now / tier->bucket_s);
    } else {
        for (const Bucket &b : tier->buckets) {
            if (b.n > 0 && b.index > newest) {
                newest = b.index;
            }
        }
    }
    size_t count = 0;
    for (int32_t index = newest - static_cast<int32_t>(POINTS) + 1; index <= newest; index++) {
        if (index < 0) {
            continue;
        }
        const Bucket &b = tier->buckets[index % POINTS];
        if (b.n > 0 && b.index == index) {
            out[count++] = Point{static_cast<time_t>(index) * tier->bucket_s + static_cast<time_t>(b.offset_s),
                                 b.temp, b.hum, b.press};
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

}  // namespace charts

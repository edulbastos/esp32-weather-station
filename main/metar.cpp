#include "metar.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include "charts.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "identity.h"
#include "net_time.h"
#include "quality.h"
#include "telemetry.h"

static const char *TAG = "metar";

namespace {

bool s_enabled = false;
SemaphoreHandle_t s_lock = nullptr;

// Shared with `sys`, /api/current and MQTT; written by taskMetar only.
struct State {
    bool has_sample = false;
    time_t obs_time = 0;     // of the last sample
    float ref_hpa = 0;       // reference QNH (with the rounding bias removed)
    float qnh_hpa = 0;       // station QNH at obs_time
    float diff_hpa = 0;      // qnh_hpa - ref_hpa
    size_t samples = 0;      // in the window
    bool has_median = false;
    float median_hpa = 0;
    float offset_hpa = 0;    // suggested total pressure offset (cal set pressure ...)
    int altitude_m = 0;      // suggested altitude
    char result[48] = "no request yet";  // outcome of the last request, for `sys`
};
State s_state;

// FR-11.4: window of the last METAR_WINDOW differences (taskMetar only).
float s_diffs[config::METAR_WINDOW];
size_t s_count = 0, s_next = 0;
int16_t s_window_offset = 0;  // pressure offset (0.1 hPa) the window was taken with
float s_station_hpa = 0;      // station pressure of the last sample
time_t s_last_obs = 0;        // observation used last
bool s_warned = false;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

void set_result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void set_result(const char *fmt, ...)
{
    Lock lock;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_state.result, sizeof(s_state.result), fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// HTTPS request
// ---------------------------------------------------------------------------

struct Answer {
    char *buf;
    size_t len;
    bool overflow;
};

esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    auto *a = static_cast<Answer *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && a != nullptr) {
        if (a->len + evt->data_len >= config::METAR_MAX_ANSWER) {
            a->overflow = true;
        } else {
            memcpy(a->buf + a->len, evt->data, evt->data_len);
            a->len += evt->data_len;
            a->buf[a->len] = '\0';
        }
    }
    return ESP_OK;
}

// Fetches the METAR answer into `buf` (METAR_MAX_ANSWER bytes). Returns false
// (with the reason in the state) on failure; `status` is the HTTP status, or
// 0 when no answer arrived.
bool fetch(char *buf, int &status)
{
    char url[128];
    snprintf(url, sizeof(url), config::METAR_URL_FORMAT, config::METAR_STATION);
    char user_agent[48];
    snprintf(user_agent, sizeof(user_agent), "%s/%s", config::METAR_USER_AGENT, identity::firmware_version());
    Answer answer = {buf, 0, false};
    buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = config::METAR_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // server certificate is checked
    cfg.user_agent = user_agent;
    cfg.event_handler = on_http_event;
    cfg.user_data = &answer;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        set_result("HTTP client not created");
        ESP_LOGW(TAG, "HTTP client not created");
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        set_result("request failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "METAR request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status == 204) {
        // The API has no recent observation of this airport.
        set_result("no recent observation (HTTP 204)");
        ESP_LOGI(TAG, "No recent METAR of %s", config::METAR_STATION);
        return false;
    }
    if (status != 200) {
        set_result("HTTP status %d", status);
        ESP_LOGW(TAG, "METAR request answered with HTTP status %d", status);
        return false;
    }
    if (answer.overflow) {
        set_result("answer longer than %u bytes", static_cast<unsigned>(config::METAR_MAX_ANSWER));
        ESP_LOGW(TAG, "METAR answer longer than %u bytes, ignored", static_cast<unsigned>(config::METAR_MAX_ANSWER));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal JSON reading: the answer is one small array of flat objects; the
// first object is the newest observation.
// ---------------------------------------------------------------------------

const char *value_of(const char *json, const char *key)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (p == nullptr) {
        return nullptr;
    }
    p += strlen(pattern);
    while (*p == ' ') {
        p++;
    }
    return p;
}

bool number_of(const char *json, const char *key, double &out)
{
    const char *p = value_of(json, key);
    if (p == nullptr) {
        return false;
    }
    char *end = nullptr;
    out = strtod(p, &end);
    return end != p && std::isfinite(out);
}

bool string_of(const char *json, const char *key, char *out, size_t len)
{
    const char *p = value_of(json, key);
    if (p == nullptr || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"' && n + 1 < len) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return *p == '"';
}

// FR-11.4: a QNH reported in hPa ("Q1025") is rounded down to the whole hPa.
bool has_q_group(const char *raw)
{
    for (const char *p = raw; (p = strstr(p, " Q")) != nullptr; p += 2) {
        if (isdigit(static_cast<unsigned char>(p[2])) && isdigit(static_cast<unsigned char>(p[3])) &&
            isdigit(static_cast<unsigned char>(p[4])) && isdigit(static_cast<unsigned char>(p[5])) &&
            (p[6] == '\0' || p[6] == ' ' || p[6] == '=')) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

// Mean station pressure (calibrated, not reduced) of the 1-minute chart
// buckets within METAR_MATCH_S of `t`.
bool station_pressure_at(time_t t, float &out)
{
    std::unique_ptr<charts::Point[]> pts(new (std::nothrow) charts::Point[config::WEB_CHART_MAX_POINTS]);
    if (!pts) {
        return false;
    }
    uint32_t bucket_s = 0;
    const size_t n = charts::snapshot(config::WEB_CHART_PERIODS_H[0], time(nullptr), pts.get(), bucket_s);
    double sum = 0;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (std::llabs(static_cast<long long>(pts[i].time - t)) <= config::METAR_MATCH_S) {
            sum += pts[i].pressure_hpa;
            used++;
        }
    }
    if (used == 0) {
        return false;
    }
    out = static_cast<float>(sum / used);
    return true;
}

float median_of_window()
{
    float v[config::METAR_WINDOW];
    std::copy(s_diffs, s_diffs + s_count, v);
    std::sort(v, v + s_count);
    return s_count % 2 ? v[s_count / 2] : (v[s_count / 2 - 1] + v[s_count / 2]) / 2;
}

void add_sample(time_t obs_time, float ref_hpa, float station_hpa)
{
    // A changed pressure offset makes the older samples meaningless.
    const int16_t offset = quality::offsets(0).press_deci;
    if (offset != s_window_offset && s_count > 0) {
        ESP_LOGI(TAG, "Pressure calibration changed, comparison window restarted");
        s_count = s_next = 0;
    }
    s_window_offset = offset;

    const float factor = quality::msl_factor();
    const float qnh = station_hpa * factor;
    const float diff = qnh - ref_hpa;
    s_diffs[s_next] = diff;
    s_next = (s_next + 1) % config::METAR_WINDOW;
    s_count = std::min(s_count + 1, config::METAR_WINDOW);
    s_station_hpa = station_hpa;

    const bool has_median = s_count >= config::METAR_MIN_SAMPLES;
    float median = 0, offset_hpa = 0;
    int altitude = 0;
    if (has_median) {
        median = median_of_window();
        // Either a pressure offset that cancels the median (in the units of
        // `cal set pressure`, added to the current one) ...
        offset_hpa = std::round((offset / 10.0f - median / factor) * 10.0f) / 10.0f;
        // ... or the altitude at which the reduction gives the reference.
        const float f = factor - median / station_hpa;
        altitude = static_cast<int>(std::lround((1.0f - std::pow(f, -1.0f / 5.25588f)) / 2.25577e-5f));
    }

    {
        Lock lock;
        s_state.has_sample = true;
        s_state.obs_time = obs_time;
        s_state.ref_hpa = ref_hpa;
        s_state.qnh_hpa = qnh;
        s_state.diff_hpa = diff;
        s_state.samples = s_count;
        s_state.has_median = has_median;
        s_state.median_hpa = median;
        s_state.offset_hpa = offset_hpa;
        s_state.altitude_m = altitude;
        snprintf(s_state.result, sizeof(s_state.result), "ok");
    }

    if (has_median) {
        ESP_LOGI(TAG, "%s: reference QNH %.1f hPa, station %.1f hPa, difference %+.1f hPa, median %+.1f hPa of %u",
                 config::METAR_STATION, ref_hpa, qnh, diff, median, static_cast<unsigned>(s_count));
    } else {
        ESP_LOGI(TAG, "%s: reference QNH %.1f hPa, station %.1f hPa, difference %+.1f hPa (%u of %u samples)",
                 config::METAR_STATION, ref_hpa, qnh, diff, static_cast<unsigned>(s_count),
                 static_cast<unsigned>(config::METAR_MIN_SAMPLES));
    }

    // Warned once; again only after the median came back within the limit.
    const bool off = has_median && std::fabs(median) > config::METAR_WARN_HPA;
    if (off && !s_warned) {
        ESP_LOGW(TAG, "Sea-level pressure differs from %s by %+.1f hPa (median of %u): set "
                      "`cal set pressure %.1f` or the altitude to %d m (now %d m)",
                 config::METAR_STATION, median, static_cast<unsigned>(s_count), offset_hpa, altitude,
                 quality::altitude_m());
    }
    s_warned = off;

    char json[metar::JSON_BYTES];
    const int n = metar::json(json, sizeof(json));
    if (n > 0 && static_cast<size_t>(n) < sizeof(json)) {
        telemetry::publish_metar(json);  // fails harmlessly while not connected
    }
}

enum class Outcome { USED, NOT_NEW, FAILED };

Outcome attempt()
{
    // FR-11.4: the TLS handshake needs about 40 KB in one piece.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < config::METAR_MIN_HEAP_BLOCK) {
        set_result("skipped, largest heap block %u KB", static_cast<unsigned>(largest / 1024));
        ESP_LOGW(TAG, "METAR request skipped: largest free heap block %u KB, needs %u KB",
                 static_cast<unsigned>(largest / 1024), static_cast<unsigned>(config::METAR_MIN_HEAP_BLOCK / 1024));
        return Outcome::FAILED;
    }
    std::unique_ptr<char[]> buf(new (std::nothrow) char[config::METAR_MAX_ANSWER]);
    if (!buf) {
        set_result("out of memory");
        return Outcome::FAILED;
    }
    int status = 0;
    if (!fetch(buf.get(), status)) {
        // 204: no recent observation yet; worth the second try of the hour.
        return status == 204 ? Outcome::NOT_NEW : Outcome::FAILED;
    }

    double obs = 0, altim = 0;
    char station[8] = "", raw[160] = "";
    if (!number_of(buf.get(), "obsTime", obs) || !number_of(buf.get(), "altim", altim) ||
        !string_of(buf.get(), "icaoId", station, sizeof(station))) {
        set_result("no usable observation");
        ESP_LOGW(TAG, "METAR of %s: no usable observation in the answer", config::METAR_STATION);
        return Outcome::FAILED;
    }
    string_of(buf.get(), "rawOb", raw, sizeof(raw));  // may be cut; the Q group comes early
    const time_t obs_time = static_cast<time_t>(obs);
    if (obs_time <= s_last_obs) {
        set_result("observation not new");
        return Outcome::NOT_NEW;
    }
    s_last_obs = obs_time;

    if (altim < config::METAR_QNH_MIN_HPA || altim > config::METAR_QNH_MAX_HPA) {
        set_result("implausible QNH %.1f hPa", altim);
        ESP_LOGW(TAG, "METAR of %s: implausible QNH %.1f hPa, ignored", station, altim);
        return Outcome::FAILED;
    }
    float station_hpa = 0;
    if (!station_pressure_at(obs_time, station_hpa)) {
        set_result("no station data at the observation time");
        ESP_LOGW(TAG, "METAR of %s: no measurement within %lu s of the observation, ignored", station,
                 static_cast<unsigned long>(config::METAR_MATCH_S));
        return Outcome::FAILED;
    }
    const float ref = static_cast<float>(altim) + (has_q_group(raw) ? 0.5f : 0.0f);
    add_sample(obs_time, ref, station_hpa);
    return Outcome::USED;
}

// Next Unix time at `minute` of an hour, after `now`.
time_t next_at(time_t now, uint32_t minute)
{
    time_t t = now - now % 3600 + static_cast<time_t>(minute) * 60;
    return t > now ? t : t + 3600;
}

// Sleeps until Unix time `t`, in steps of at most a minute (the clock may be
// corrected meanwhile).
void sleep_until(time_t t)
{
    while (true) {
        const time_t now = time(nullptr);
        if (now >= t) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(std::min<time_t>(t - now, 60) * 1000));
    }
}

void taskMetar(void *)
{
    while (true) {
        // FR-11.4: only with a synchronized clock.
        struct tm now_tm;
        if (!net_time::local_time(now_tm)) {
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }
        sleep_until(next_at(time(nullptr), config::METAR_FETCH_MINUTE));
        if (attempt() == Outcome::NOT_NEW) {
            sleep_until(next_at(time(nullptr), config::METAR_RETRY_MINUTE));
            attempt();
        }
    }
}

bool valid_code(const char *code)
{
    const size_t len = strlen(code);
    if (len < 3 || len > 4) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum(static_cast<unsigned char>(code[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace metar {

esp_err_t start()
{
    if (config::METAR_STATION[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_code(config::METAR_STATION)) {
        ESP_LOGW(TAG, "METAR airport \"%s\" is not an ICAO code, pressure check disabled", config::METAR_STATION);
        return ESP_ERR_INVALID_ARG;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr ||
        xTaskCreate(taskMetar, "taskMetar", config::METAR_TASK_STACK, nullptr, config::METAR_TASK_PRIO, nullptr) !=
            pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_enabled = true;
    ESP_LOGI(TAG, "Pressure check against the METAR of %s every hour at minute %lu", config::METAR_STATION,
             static_cast<unsigned long>(config::METAR_FETCH_MINUTE));
    return ESP_OK;
}

bool enabled() { return s_enabled; }

int json(char *buf, size_t len)
{
    if (!s_enabled) {
        return snprintf(buf, len, "null");
    }
    State st;
    {
        Lock lock;
        st = s_state;
    }
    if (!st.has_sample) {
        return snprintf(buf, len, "null");
    }
    char median[16] = "null", offset[16] = "null", altitude[16] = "null";
    if (st.has_median) {
        snprintf(median, sizeof(median), "%.1f", st.median_hpa);
        snprintf(offset, sizeof(offset), "%.1f", st.offset_hpa);
        snprintf(altitude, sizeof(altitude), "%d", st.altitude_m);
    }
    return snprintf(buf, len,
                    "{\"station\":\"%s\",\"obs_time\":%lld,\"qnh_ref_hpa\":%.1f,\"qnh_hpa\":%.1f,\"diff_hpa\":%.1f,"
                    "\"median_diff_hpa\":%s,\"samples\":%u,\"suggested_offset_hpa\":%s,\"suggested_altitude_m\":%s}",
                    config::METAR_STATION, static_cast<long long>(st.obs_time), st.ref_hpa, st.qnh_hpa, st.diff_hpa,
                    median, static_cast<unsigned>(st.samples), offset, altitude);
}

void print_status()
{
    if (!s_enabled) {
        printf("METAR check: off\n");
        return;
    }
    State st;
    {
        Lock lock;
        st = s_state;
    }
    printf("METAR check: %s, last request: %s\n", config::METAR_STATION, st.result);
    if (!st.has_sample) {
        return;
    }
    char when[24];
    struct tm local;
    localtime_r(&st.obs_time, &local);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
    printf("  observation %s: reference QNH %.1f hPa, station %.1f hPa, difference %+.1f hPa\n", when, st.ref_hpa,
           st.qnh_hpa, st.diff_hpa);
    if (st.has_median) {
        printf("  median %+.1f hPa of %u samples; to cancel it: `cal set pressure %.1f`, or altitude %d m (now %d m)\n",
               st.median_hpa, static_cast<unsigned>(st.samples), st.offset_hpa, st.altitude_m,
               quality::altitude_m());
    } else {
        printf("  %u of %u samples before the median is reported\n", static_cast<unsigned>(st.samples),
               static_cast<unsigned>(config::METAR_MIN_SAMPLES));
    }
}

}  // namespace metar

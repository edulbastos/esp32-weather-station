#include "web_server.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "charts.h"
#include "history.h"
#include "net_time.h"

static const char *TAG = "web";

// web_page.html, embedded by main/CMakeLists.txt as is (EMBED_FILES) and
// gzip-compressed (built from it at compile time)
extern const char web_page_start[] asm("_binary_web_page_html_start");
extern const char web_page_end[] asm("_binary_web_page_html_end");
extern const char web_page_gz_start[] asm("_binary_web_page_html_gz_start");
extern const char web_page_gz_end[] asm("_binary_web_page_html_gz_end");

namespace {

web_server::CurrentFn s_current = nullptr;
httpd_handle_t s_server = nullptr;
std::atomic<bool> s_starting{false};
std::atomic<uint32_t> s_requests{0};

// Collects the response in a small buffer and sends it in chunks, so large
// answers (the whole history) never have to fit in RAM.
class ChunkWriter {
public:
    explicit ChunkWriter(httpd_req_t *req) : req_(req) {}

    bool write(const char *data, size_t len)
    {
        while (ok_ && len > 0) {
            const size_t n = std::min(len, sizeof(buf_) - used_);
            memcpy(buf_ + used_, data, n);
            used_ += n;
            data += n;
            len -= n;
            if (used_ == sizeof(buf_)) {
                flush();
            }
        }
        return ok_;
    }

    // Formats one piece of the answer. A piece that does not fit would make
    // the JSON invalid, so it fails the answer (and is logged) instead of
    // being cut off.
    [[gnu::format(printf, 2, 3)]] bool print(const char *fmt, ...)
    {
        char line[192];
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(line)) {
            ESP_LOGE(TAG, "Answer piece of %d bytes does not fit the %u-byte buffer", n,
                     static_cast<unsigned>(sizeof(line)));
            ok_ = false;
            return false;
        }
        return write(line, n);
    }

    // Sends what is left and ends the chunked response.
    esp_err_t finish()
    {
        flush();
        if (ok_) {
            ok_ = httpd_resp_send_chunk(req_, nullptr, 0) == ESP_OK;
        }
        return ok_ ? ESP_OK : ESP_FAIL;
    }

private:
    void flush()
    {
        if (ok_ && used_ > 0) {
            // A failure means the client went away; stop producing data.
            ok_ = httpd_resp_send_chunk(req_, buf_, used_) == ESP_OK;
        }
        used_ = 0;
    }

    httpd_req_t *req_;
    char buf_[1024];
    size_t used_ = 0;
    bool ok_ = true;
};

bool accepts_gzip(httpd_req_t *req)
{
    char value[96] = "";
    // A truncated value (ESP_ERR_HTTPD_RESULT_TRUNC) still holds its start.
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value));
    return (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) && strstr(value, "gzip") != nullptr;
}

// FR-8.2: the page, gzip-compressed when the browser accepts it, so fewer
// packets have to cross the Wi-Fi link.
esp_err_t handle_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    if (accepts_gzip(req)) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, web_page_gz_start, web_page_gz_end - web_page_gz_start);
    }
    return httpd_resp_send(req, web_page_start, web_page_end - web_page_start);
}

// FR-8.2: browsers that ask for /favicon.ico anyway, and Safari on iOS asking
// for its home-screen icons, get an empty answer instead of a 404 (which
// ESP-IDF logs as a warning).
esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, nullptr, 0);
}

// FR-8.3: current values as JSON.
esp_err_t handle_current(httpd_req_t *req)
{
    web_server::Current cur = {};
    if (!s_current(cur)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Data busy, try again");
    }

    char json[320];
    char time_field[24] = "null";
    if (cur.time != 0) {
        snprintf(time_field, sizeof(time_field), "%lld", static_cast<long long>(cur.time));
    }
    if (cur.valid) {
        snprintf(json, sizeof(json),
                 "{\"valid\":true,\"time\":%s,\"age_s\":%.1f,\"temperature_c\":%.2f,"
                 "\"humidity_rh\":%.2f,\"pressure_hpa\":%.2f,\"wifi_level\":%d,"
                 "\"measure_period_s\":%lu,\"log_period_s\":%lu}",
                 time_field, cur.age_ms / 1000.0, cur.values.temperature_c, cur.values.humidity_rh,
                 cur.values.pressure_hpa, net_time::wifi_level(),
                 static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    } else {
        snprintf(json, sizeof(json),
                 "{\"valid\":false,\"wifi_level\":%d,\"measure_period_s\":%lu,\"log_period_s\":%lu}",
                 net_time::wifi_level(), static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// FR-8.4: the last `hours` hours as JSON, averaged into at most `points`
// time buckets, so the answer stays small whatever the window. With buckets
// shorter than the log interval every record is its own point.
esp_err_t handle_history(httpd_req_t *req)
{
    uint32_t hours = config::WEB_HISTORY_HOURS;
    uint32_t points = config::WEB_CHART_POINTS;
    char query[48], value[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "hours", value, sizeof(value)) == ESP_OK) {
            hours = std::clamp<uint32_t>(strtoul(value, nullptr, 10), 1, config::WEB_HISTORY_MAX_HOURS);
        }
        if (httpd_query_key_value(query, "points", value, sizeof(value)) == ESP_OK) {
            points = std::clamp<uint32_t>(strtoul(value, nullptr, 10), 10, config::WEB_HISTORY_MAX_POINTS);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    struct tm now_tm;
    const bool synced = net_time::local_time(now_tm);

    // The periods of the page come ready from RAM (charts.cpp): no flash
    // access, constant time whatever the measurement or log interval.
    if (charts::serves(hours, points)) {
        std::unique_ptr<charts::Point[]> pts(new (std::nothrow) charts::Point[config::WEB_CHART_POINTS]);
        if (!pts) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        uint32_t bucket_s = 0;
        const size_t n = charts::snapshot(hours, synced ? time(nullptr) : 0, pts.get(), bucket_s);
        ChunkWriter out(req);
        out.print("{\"hours\":%lu,\"points\":%lu,\"bucket_s\":%lu,\"log_period_s\":%lu,\"measure_period_s\":%lu,"
                  "\"source\":\"measurements\",\"records\":[",
                  static_cast<unsigned long>(hours), static_cast<unsigned long>(points),
                  static_cast<unsigned long>(bucket_s), static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000),
                  static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000));
        for (size_t i = 0; i < n; i++) {
            out.print("%s[%lld,%.2f,%.2f,%.1f]", i ? "," : "", static_cast<long long>(pts[i].time),
                      pts[i].temperature_c, pts[i].humidity_rh, pts[i].pressure_hpa);
        }
        out.print("],\"available\":true}");
        return out.finish();
    }

    // Other periods: averaged from the flash history on request.
    const uint32_t window_s = hours * 3600;
    const uint32_t bucket_s = std::max<uint32_t>(1, window_s / points);

    // Records that can fall in the window at the current log interval.
    const uint64_t window_records = static_cast<uint64_t>(window_s) * 1000 / config::LOG_PERIOD_MS + 1;
    const uint32_t last_n = static_cast<uint32_t>(std::min<uint64_t>(window_records, UINT32_MAX));

    // Buckets start at the beginning of the window; before NTP sync the
    // window is unknown and starts at the first record that has a time.
    // Records without a time cannot be placed on the chart.
    time_t base = synced ? time(nullptr) - static_cast<time_t>(window_s) : 0;

    ChunkWriter out(req);
    out.print("{\"hours\":%lu,\"points\":%lu,\"bucket_s\":%lu,\"log_period_s\":%lu,\"measure_period_s\":%lu,"
              "\"source\":\"history\",\"records\":[",
              static_cast<unsigned long>(hours), static_cast<unsigned long>(points),
              static_cast<unsigned long>(bucket_s), static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000),
              static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000));

    struct {
        int64_t index = -1;
        uint32_t n = 0;
        double time = 0, temp = 0, hum = 0, press = 0;
    } acc;
    bool first = true;
    // Sends the average of the current bucket.
    auto emit = [&]() {
        if (acc.n == 0) {
            return true;
        }
        const bool ok = out.print("%s[%lld,%.2f,%.2f,%.1f]", first ? "" : ",", llround(acc.time / acc.n),
                                  acc.temp / acc.n, acc.hum / acc.n, acc.press / acc.n);
        first = false;
        acc = {};
        return ok;
    };

    const esp_err_t err = history::visit(last_n, [&](const history::Entry &e) {
        if (e.time == 0) {
            return true;
        }
        if (base == 0) {
            base = e.time;
        }
        if (e.time < base) {
            return true;
        }
        const int64_t index = (e.time - base) / bucket_s;
        if (index != acc.index) {
            if (!emit()) {
                return false;
            }
            acc.index = index;
        }
        acc.n++;
        acc.time += e.time;
        acc.temp += e.temperature_c;
        acc.hum += e.humidity_rh;
        acc.press += e.pressure_hpa;
        return true;
    });
    emit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "History read failed: %s", esp_err_to_name(err));
    }
    out.print("],\"available\":%s}", err == ESP_ERR_INVALID_STATE ? "false" : "true");
    return out.finish();
}

// FR-8.5: the whole history as a CSV download.
esp_err_t handle_csv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"weather-history.csv\"");
    ChunkWriter out(req);
    out.write(history::CSV_HEADER, strlen(history::CSV_HEADER));
    const esp_err_t err = history::visit(0, [&out](const history::Entry &e) {
        char line[80];
        const int n = history::format_csv(e, line, sizeof(line));
        return n > 0 && out.write(line, std::min<size_t>(n, sizeof(line) - 1));
    });
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History read failed: %s", esp_err_to_name(err));
    }
    return out.finish();
}

// FR-8.6: one request per connection. Browsers keep idle connections open and
// later reset them, which ESP-IDF logs as "error in recv : 104"; closing each
// connection after its answer avoids that. The page makes one request per
// measurement or log interval, so the extra TCP handshakes are negligible.
using Handler = esp_err_t (*)(httpd_req_t *);

esp_err_t handle_and_close(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    s_requests++;
    const esp_err_t err = reinterpret_cast<Handler>(req->user_ctx)(req);
    // On failure the server already closes the session itself.
    if (err == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    return err;
}

// The server writes each response header and each chunk in several small
// pieces. With Nagle's algorithm on, every piece after the first waits for
// the browser's ACK, which browsers delay by up to 200 ms: about 0.2 to 0.3 s
// per request, over a second for a new page. Small answers do not need it.
esp_err_t open_no_delay(httpd_handle_t, int sockfd)
{
    const int on = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        ESP_LOGW(TAG, "TCP_NODELAY not set: errno %d", errno);  // slower, still works
    }
    return ESP_OK;
}

esp_err_t start_server()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = config::WEB_SERVER_PORT;
    cfg.stack_size = config::WEB_TASK_STACK;
    cfg.task_priority = config::WEB_TASK_PRIO;
    cfg.max_open_sockets = config::WEB_MAX_CLIENTS;
    cfg.max_uri_handlers = config::WEB_MAX_ROUTES;
    // On a lossy link one segment can wait several seconds for its
    // retransmission; the default 5 s cut pages off halfway.
    cfg.send_wait_timeout = config::WEB_SEND_TIMEOUT_S;
    cfg.lru_purge_enable = true;  // a new client closes the least recently used connection
    cfg.open_fn = open_no_delay;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const struct {
        const char *uri;
        Handler handler;
    } routes[] = {
        {"/", handle_page},
        {"/api/current", handle_current},
        {"/api/history", handle_history},
        {"/history.csv", handle_csv},
        {"/favicon.ico", handle_favicon},
        {"/apple-touch-icon.png", handle_favicon},
        {"/apple-touch-icon-precomposed.png", handle_favicon},
    };
    for (const auto &r : routes) {
        httpd_uri_t uri = {};
        uri.uri = r.uri;
        uri.method = HTTP_GET;
        uri.handler = handle_and_close;
        uri.user_ctx = reinterpret_cast<void *>(r.handler);
        if ((err = httpd_register_uri_handler(s_server, &uri)) != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

// FR-8.1: the server starts with the first IP address; the URL is logged on
// every (re)connection, since the address can change.
void on_got_ip(void *, esp_event_base_t, int32_t, void *data)
{
    if (s_server == nullptr && !s_starting.exchange(true)) {
        const esp_err_t err = start_server();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Web server start failed: %s", esp_err_to_name(err));
            if (s_server != nullptr) {
                httpd_stop(s_server);
                s_server = nullptr;
            }
            s_starting = false;
            return;
        }
    }
    const auto *info = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "Web page at http://" IPSTR "/", IP2STR(&info->ip_info.ip));
}

}  // namespace

namespace web_server {

esp_err_t start(CurrentFn current)
{
    s_current = current;
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
}

uint32_t requests_served() { return s_requests; }

}  // namespace web_server

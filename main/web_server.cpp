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
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "charts.h"
#include "gzip_stream.h"
#include "health.h"
#include "history.h"
#include "identity.h"
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

std::atomic<size_t> s_answer_bytes{0};  // of the request being handled (one at a time)
constexpr const char *NO_STORE = "Cache-Control: no-store\r\n";

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Sends `len` bytes; the socket may take them in several parts.
bool send_all(httpd_req_t *req, const char *data, size_t len)
{
    while (len > 0) {
        const int n = httpd_send(req, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

// FR-8.6: collects the answer in a buffer on the heap and sends it with one
// write per buffer: status line, headers and body together, so lwIP fills
// whole segments. ESP-IDF's own response functions send every header field,
// separator and line break with a write of its own; with TCP_NODELAY each
// became a packet, about 11 for a small JSON answer, and on a lossy link
// every packet is a chance to wait for a retransmission. An answer that fits
// the buffer goes out with Content-Length in a single write; a longer one is
// sent chunked, one chunk of `size` bytes per write, so large answers (the
// whole history) never have to fit in RAM. `headers` holds extra header
// lines, each ending with CRLF.
class ChunkWriter {
public:
    ChunkWriter(httpd_req_t *req, const char *type, const char *headers = "",
                size_t size = config::WEB_CHUNK_BYTES)
        : req_(req), type_(type), headers_(headers),
          buf_(new (std::nothrow) char[HEAD_ROOM + size + TAIL_ROOM]), size_(size), ok_(buf_ != nullptr)
    {
        if (!ok_) {
            ESP_LOGE(TAG, "No memory for a %u-byte answer buffer", static_cast<unsigned>(size));
        }
    }

    bool write(const char *data, size_t len)
    {
        while (ok_ && len > 0) {
            const size_t n = std::min(len, size_ - used_);
            memcpy(body() + used_, data, n);
            used_ += n;
            data += n;
            len -= n;
            if (used_ == size_) {
                send(false);
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

    // Sends what is left and ends the answer.
    esp_err_t finish()
    {
        if (ok_) {
            send(true);
        }
        return ok_ ? ESP_OK : ESP_FAIL;
    }

private:
    // Room for the status line and headers, and for a chunk size line,
    // before the body; for the chunk end and the last chunk after it.
    static constexpr size_t HEAD_ROOM = 384;
    static constexpr size_t TAIL_ROOM = 8;  // "\r\n" + "0\r\n\r\n"

    char *body() { return buf_.get() + HEAD_ROOM; }

    // Puts the headers (first write only) and the chunk framing around the
    // buffered body and sends it all with one write.
    void send(bool last)
    {
        char *start = body();
        char *end = body() + used_;
        const bool chunked = started_ || !last;
        if (chunked) {
            if (used_ > 0) {
                char size_line[12];
                const int n = snprintf(size_line, sizeof(size_line), "%x\r\n", static_cast<unsigned>(used_));
                start -= n;
                memcpy(start, size_line, n);
                memcpy(end, "\r\n", 2);
                end += 2;
            }
            if (last) {
                memcpy(end, "0\r\n\r\n", 5);
                end += 5;
            }
        }
        if (!started_) {
            char head[HEAD_ROOM - 16];
            char length[40];
            if (chunked) {
                snprintf(length, sizeof(length), "Transfer-Encoding: chunked\r\n");
            } else {
                snprintf(length, sizeof(length), "Content-Length: %u\r\n", static_cast<unsigned>(used_));
            }
            const int n = snprintf(head, sizeof(head),
                                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sConnection: close\r\n%s\r\n", type_,
                                   length, headers_);
            if (n < 0 || static_cast<size_t>(n) >= sizeof(head)) {
                ESP_LOGE(TAG, "Answer headers do not fit %u bytes", static_cast<unsigned>(sizeof(head)));
                ok_ = false;
                return;
            }
            start -= n;
            memcpy(start, head, n);
            started_ = true;
        }
        // A failure means the client went away; stop producing data.
        ok_ = send_all(req_, start, end - start);
        s_answer_bytes += end - start;
        used_ = 0;
    }

    httpd_req_t *req_;
    const char *type_;
    const char *headers_;
    std::unique_ptr<char[]> buf_;
    size_t size_;
    size_t used_ = 0;
    bool started_ = false;  // headers sent
    bool ok_;
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
    const bool gzip = accepts_gzip(req);
    ChunkWriter out(req, "text/html; charset=utf-8",
                    gzip ? "Vary: Accept-Encoding\r\nContent-Encoding: gzip\r\n" : "Vary: Accept-Encoding\r\n");
    if (gzip) {
        out.write(web_page_gz_start, web_page_gz_end - web_page_gz_start);
    } else {
        out.write(web_page_start, web_page_end - web_page_start);
    }
    return out.finish();
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

// FR-8.3, FR-10.5: current values and identifiers as JSON.
esp_err_t handle_current(httpd_req_t *req)
{
    web_server::Current cur = {};
    if (!s_current(cur)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Data busy, try again");
    }

    char json[896];
    char time_field[24] = "null";
    if (cur.time != 0) {
        snprintf(time_field, sizeof(time_field), "%lld", static_cast<long long>(cur.time));
    }
    // FR-5.8: the current RSSI, null while Wi-Fi is not connected.
    char rssi_field[12] = "null";
    const int rssi = net_time::rssi_dbm();
    if (rssi != net_time::NO_RSSI) {
        snprintf(rssi_field, sizeof(rssi_field), "%d", rssi);
    }
    char sensor_id[24];
    identity::sensor_id(0, sensor_id, sizeof(sensor_id));
    // FR-8.9: firmware version and heap, for troubleshooting from the page.
    const health::Heap heap = health::heap_now();
    const int n = snprintf(json, sizeof(json),
                           "{\"device_id\":\"%s\",\"name\":\"%s\",\"sensor_id\":\"%s\",\"firmware\":\"%s\","
                           "\"heap_free_kb\":%u,\"heap_min_kb\":%u,\"heap_largest_kb\":%u,",
                           identity::device_id(), identity::device_name_json(), sensor_id,
                           identity::firmware_version(), heap.free_kb, heap.min_kb, heap.largest_kb);
    char *rest = json + n;
    const size_t rest_len = sizeof(json) - n;
    if (cur.valid) {
        snprintf(rest, rest_len,
                 "\"valid\":true,\"time\":%s,\"age_s\":%.1f,\"temperature_c\":%.2f,"
                 "\"humidity_rh\":%.2f,\"pressure_hpa\":%.2f,\"wifi_level\":%d,\"rssi_dbm\":%s,"
                 "\"measure_period_s\":%lu,\"log_period_s\":%lu}",
                 time_field, cur.age_ms / 1000.0, cur.values.temperature_c, cur.values.humidity_rh,
                 cur.values.pressure_hpa, net_time::wifi_level(), rssi_field,
                 static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    } else {
        snprintf(rest, rest_len,
                 "\"valid\":false,\"wifi_level\":%d,\"rssi_dbm\":%s,\"measure_period_s\":%lu,"
                 "\"log_period_s\":%lu}",
                 net_time::wifi_level(), rssi_field, static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    }
    ChunkWriter out(req, "application/json", NO_STORE);
    out.write(json, strlen(json));
    return out.finish();
}

// FR-10.5: the info message of FR-10.4 (device, firmware, intervals, sensors).
esp_err_t handle_info(httpd_req_t *req)
{
    char json[768];
    const int n = identity::info_json(json, sizeof(json));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Info does not fit");
    }
    ChunkWriter out(req, "application/json", NO_STORE);
    out.write(json, n);
    return out.finish();
}

// FR-8.4: one chart period from the tables in RAM (charts.cpp), in the JSON
// format of /api/history. `pts` holds WEB_CHART_POINTS points.
bool print_chart(ChunkWriter &out, uint32_t hours, bool synced, charts::Point *pts)
{
    uint32_t bucket_s = 0;
    const size_t n = charts::snapshot(hours, synced ? time(nullptr) : 0, pts, bucket_s);
    out.print("{\"hours\":%lu,\"points\":%lu,\"bucket_s\":%lu,\"log_period_s\":%lu,\"measure_period_s\":%lu,"
              "\"source\":\"measurements\",\"records\":[",
              static_cast<unsigned long>(hours), static_cast<unsigned long>(config::WEB_CHART_POINTS),
              static_cast<unsigned long>(bucket_s), static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000),
              static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000));
    for (size_t i = 0; i < n; i++) {
        out.print("%s[%lld,%.2f,%.2f,%.1f]", i ? "," : "", static_cast<long long>(pts[i].time),
                  pts[i].temperature_c, pts[i].humidity_rh, pts[i].pressure_hpa);
    }
    return out.print("],\"available\":true}");
}

// FR-8.4: all chart periods of the page in one answer, so a new page needs
// one request (and one TCP connection) for its charts instead of three.
esp_err_t handle_charts(httpd_req_t *req)
{
    std::unique_ptr<charts::Point[]> pts(new (std::nothrow) charts::Point[config::WEB_CHART_POINTS]);
    if (!pts) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    struct tm now_tm;
    const bool synced = net_time::local_time(now_tm);
    ChunkWriter out(req, "application/json", NO_STORE);
    out.print("{\"periods\":[");
    bool first = true;
    for (const uint32_t hours : config::WEB_CHART_PERIODS_H) {
        if (!first) {
            out.print(",");
        }
        first = false;
        print_chart(out, hours, synced, pts.get());
    }
    out.print("]}");
    return out.finish();
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
    struct tm now_tm;
    const bool synced = net_time::local_time(now_tm);

    // The periods of the page come ready from RAM (charts.cpp): no flash
    // access, constant time whatever the measurement or log interval.
    if (charts::serves(hours, points)) {
        std::unique_ptr<charts::Point[]> pts(new (std::nothrow) charts::Point[config::WEB_CHART_POINTS]);
        if (!pts) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        ChunkWriter out(req, "application/json", NO_STORE);
        print_chart(out, hours, synced, pts.get());
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

    ChunkWriter out(req, "application/json", NO_STORE);
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
        if (e.time == 0 || e.sensor_index != 0) {  // FR-10.5: sensor at position 0
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

// FR-8.5, FR-10.5: the whole history as a CSV download named after the
// device; FR-8.8: gzip-compressed for browsers that accept it.
esp_err_t handle_csv(httpd_req_t *req)
{
    char headers[160] = "";  // filled before the first write
    ChunkWriter out(req, "text/csv", headers);
    // Without memory for the compressor the CSV goes out uncompressed.
    GzipStream gz;
    const bool gzip = accepts_gzip(req) &&
                      gz.begin([&out](const uint8_t *data, size_t len) {
                          return out.write(reinterpret_cast<const char *>(data), len);
                      });
    auto write = [&](const char *data, size_t len) { return gzip ? gz.write(data, len) : out.write(data, len); };

    snprintf(headers, sizeof(headers),
             "Content-Disposition: attachment; filename=\"history-%s.csv\"\r\nVary: Accept-Encoding\r\n%s",
             identity::device_id(), gzip ? "Content-Encoding: gzip\r\n" : "");
    // FR-8.6: each chunk is one write, but its last segment is rarely full.
    // Nagle's algorithm merges it with the next chunk; while data keep
    // flowing it adds no delay.
    const int off = 0;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &off, sizeof(off));
    write(history::CSV_HEADER, strlen(history::CSV_HEADER));
    const esp_err_t err = history::visit(0, [&write](const history::Entry &e) {
        char line[128];
        const int n = history::format_csv(e, line, sizeof(line));
        return n > 0 && write(line, std::min<size_t>(n, sizeof(line) - 1));
    });
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History read failed: %s", esp_err_to_name(err));
    }
    if (gzip) {
        gz.finish();
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
    s_answer_bytes = 0;
    const esp_err_t err = reinterpret_cast<Handler>(req->user_ctx)(req);

    // FR-8.6: a slow answer is logged with the time since the connection
    // was opened (set by open_no_delay), so a slow link can be measured.
    // The time ends when the last byte is handed to TCP; a lost request or
    // handshake segment shows up, a slow delivery only for answers larger
    // than the TCP send buffer.
    const uint32_t ms = now_ms() - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(req->sess_ctx));
    if (ms >= config::WEB_SLOW_ANSWER_MS) {
        ESP_LOGW(TAG, "Slow answer: %s, %u bytes in %.1f s%s", req->uri, static_cast<unsigned>(s_answer_bytes.load()),
                 ms / 1000.0, err == ESP_OK ? "" : " (failed)");
    }
    // On failure the server already closes the session itself.
    if (err == ESP_OK) {
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    } else {
        // The client stopped taking data. A normal close would leave the
        // unsent data (up to the TCP send buffer) to lwIP, which retransmits
        // them for minutes; a few such connections used up the heap. A zero
        // linger time makes the close reset the connection and free them.
        // Not used for every close: lingering blocks the server until the
        // client acknowledges, which iOS delays.
        const struct linger reset = {1, 0};
        setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    }
    return err;
}

// The server writes each response header and each chunk in several small
// pieces. With Nagle's algorithm on, every piece after the first waits for
// the browser's ACK, which browsers delay by up to 200 ms: about 0.2 to 0.3 s
// per request, over a second for a new page. Small answers do not need it.
esp_err_t open_no_delay(httpd_handle_t hd, int sockfd)
{
    // The opening time travels with the session as its context (no memory
    // to free: the value is stored in the pointer itself).
    httpd_sess_set_ctx(hd, sockfd, reinterpret_cast<void *>(static_cast<uintptr_t>(now_ms())), [](void *) {});
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
        {"/api/info", handle_info},
        {"/api/history", handle_history},
        {"/api/charts", handle_charts},
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

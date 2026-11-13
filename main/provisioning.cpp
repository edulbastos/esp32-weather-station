#include "provisioning.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "config.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "identity.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "settings.h"

static const char *TAG = "setup";

extern const char setup_page_start[] asm("_binary_setup_page_html_start");
extern const char setup_page_end[] asm("_binary_setup_page_html_end");

namespace {

// FR-12.2: survives esp_restart() (not a power-on, where it holds garbage
// and so almost never the magic value).
constexpr uint32_t BOOT_SETUP = 0x5e7a5e7a;
RTC_NOINIT_ATTR uint32_t s_boot_request;

std::atomic<bool> s_active{false};
char s_ap_ssid[16] = "";  // "ws-setup-" + 4 hex digits
char s_ap_password[config::SETUP_AP_PASS_LEN + 1] = "";
char s_ap_ip[16] = "192.168.4.1";
uint32_t s_ap_ip_addr = 0;  // network byte order, for the DNS answers
std::atomic<int> s_clients{0};
std::atomic<int64_t> s_idle_since_us{0};  // last moment without any client
TaskHandle_t s_task = nullptr;

// FR-12.5: state of a test, shown on the page, the display and `sys`.
enum State : int { IDLE, TESTING_WIFI, TESTING_BROKER, SAVED, FAILED };
const char *const STATE_NAMES[] = {"idle", "testing_wifi", "testing_broker", "saved", "failed"};
SemaphoreHandle_t s_lock = nullptr;  // s_state .. s_networks
State s_state = IDLE;
char s_message[160] = "";
char s_sta_ip[16] = "";
char s_broker[96] = "";
settings::Settings s_pending = {};

// Wi-Fi and MQTT events seen by a test.
EventGroupHandle_t s_events = nullptr;
constexpr EventBits_t BIT_GOT_IP = 1 << 0;
constexpr EventBits_t BIT_DISCONNECTED = 1 << 1;
constexpr EventBits_t BIT_ASSOCIATED = 1 << 2;
constexpr EventBits_t BIT_MQTT_OK = 1 << 3;
constexpr EventBits_t BIT_MQTT_FAIL = 1 << 4;
std::atomic<uint8_t> s_last_reason{0};
std::atomic<uint32_t> s_got_ip{0};
std::atomic<int> s_mqtt_refused{0};  // CONNACK return code, 0 = not refused

// FR-12.4: networks of the last scan, strongest first.
struct Network {
    char ssid[33];
    int8_t rssi;
    bool secure;
};
Network s_networks[config::SETUP_SCAN_MAX];
size_t s_network_count = 0;
SemaphoreHandle_t s_scan_lock = nullptr;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

void set_state(State state, const char *fmt, ...)
{
    Lock lock;
    s_state = state;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_message, sizeof(s_message), fmt, args);
    va_end(args);
}

State state()
{
    Lock lock;
    return s_state;
}

[[noreturn]] void restart_into(bool setup, const char *cause)
{
    ESP_LOGI(TAG, "%s setup mode (%s), restarting", setup ? "Entering" : "Leaving", cause);
    s_boot_request = setup ? BOOT_SETUP : 0;
    vTaskDelay(pdMS_TO_TICKS(200));  // let the log line out
    esp_restart();
}

// --- Text helpers ------------------------------------------------------------

// Appends formatted text to a fixed buffer; stops at its end.
struct Text {
    char *buf;
    size_t cap;
    size_t len = 0;

    void add(const char *fmt, ...)
    {
        if (len >= cap) {
            return;
        }
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(buf + len, cap - len, fmt, args);
        va_end(args);
        len = n < 0 ? cap : std::min(cap, len + static_cast<size_t>(n));
    }

    // A JSON string with quotes; control characters as \u00XX.
    void str(const char *s)
    {
        add("\"");
        for (; *s; s++) {
            const auto c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') {
                add("\\%c", c);
            } else if (c < 0x20) {
                add("\\u%04x", c);
            } else {
                add("%c", c);
            }
        }
        add("\"");
    }

    bool full() const { return len >= cap - 1; }
};

int hex_value(char c)
{
    return isdigit(static_cast<unsigned char>(c)) ? c - '0' : (tolower(static_cast<unsigned char>(c)) - 'a' + 10);
}

// Value of `key` in an application/x-www-form-urlencoded body, decoded.
// Returns 1 when found, 0 when missing, -1 when longer than `len` - 1.
int form_value(const char *body, const char *key, char *out, size_t len)
{
    const size_t key_len = strlen(key);
    for (const char *p = body; p != nullptr && *p != '\0';) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t o = 0;
            while (*p != '\0' && *p != '&') {
                char c = *p++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && isxdigit(static_cast<unsigned char>(p[0])) &&
                           isxdigit(static_cast<unsigned char>(p[1]))) {
                    c = static_cast<char>(hex_value(p[0]) << 4 | hex_value(p[1]));
                    p += 2;
                }
                if (o + 1 >= len) {
                    return -1;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p != nullptr) {
            p++;
        }
    }
    return 0;
}

// --- Wi-Fi -------------------------------------------------------------------

bool password_rejected(uint8_t reason)
{
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE;
}

bool network_missing(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const int n = ++s_clients;
        ESP_LOGI(TAG, "Device joined the access point (%d connected)", n);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        int n = s_clients.load();
        while (n > 0 && !s_clients.compare_exchange_weak(n, n - 1)) {
        }
        if (n <= 1) {
            s_idle_since_us = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "Device left the access point");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_events, BIT_ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_last_reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_got_ip = static_cast<ip_event_got_ip_t *>(data)->ip_info.ip.addr;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

// FR-12.4: scans and keeps the networks, one entry per name, strongest first.
void scan()
{
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return;
    }
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    uint16_t count = 0;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
    }
    count = std::min<uint16_t>(count, 2 * config::SETUP_SCAN_MAX);
    std::unique_ptr<wifi_ap_record_t[]> records(count ? new (std::nothrow) wifi_ap_record_t[count] : nullptr);
    if (records == nullptr || esp_wifi_scan_get_ap_records(&count, records.get()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        count = 0;
    }
    std::sort(records.get(), records.get() + count,
              [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });

    Network found[config::SETUP_SCAN_MAX];
    size_t n = 0;
    for (size_t i = 0; i < count && n < config::SETUP_SCAN_MAX; i++) {
        const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
        if (ssid[0] == '\0' ||
            std::any_of(found, found + n, [ssid](const Network &f) { return strcmp(f.ssid, ssid) == 0; })) {
            continue;  // hidden, or another node of a mesh
        }
        snprintf(found[n].ssid, sizeof(found[n].ssid), "%s", ssid);
        found[n].rssi = records[i].rssi;
        found[n].secure = records[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    {
        Lock lock;
        std::copy(found, found + n, s_networks);
        s_network_count = n;
    }
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "Scan: %u networks", static_cast<unsigned>(n));
}

// --- Test and save (FR-12.5) -------------------------------------------------

void on_test_mqtt(void *, esp_event_base_t, int32_t id, void *data)
{
    const auto *event = static_cast<esp_mqtt_event_handle_t>(data);
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_events, BIT_MQTT_OK);
    } else if (id == MQTT_EVENT_ERROR || id == MQTT_EVENT_DISCONNECTED) {
        if (id == MQTT_EVENT_ERROR && event->error_handle != nullptr &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            s_mqtt_refused = event->error_handle->connect_return_code;
        }
        xEventGroupSetBits(s_events, BIT_MQTT_FAIL);
    }
}

const char *refusal_text(int code)
{
    switch (code) {
    case MQTT_CONNECTION_REFUSE_PROTOCOL:           return "protocol version not accepted";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:        return "client ID rejected";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: return "server unavailable";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:       return "wrong user name or password";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:     return "not authorized";
    default:                                        return "refused";
    }
}

// Tries the broker of `s` for SETUP_BROKER_TEST_MS; writes the result.
void test_broker(const settings::Settings &s, char *result, size_t len)
{
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s.mqtt_uri;
    if (s.mqtt_username[0] != '\0') {
        cfg.credentials.username = s.mqtt_username;
        cfg.credentials.authentication.password = s.mqtt_password;
    }
    cfg.credentials.client_id = identity::device_id();
    cfg.network.disable_auto_reconnect = true;
    cfg.network.timeout_ms = config::SETUP_BROKER_TEST_MS;

    s_mqtt_refused = 0;
    xEventGroupClearBits(s_events, BIT_MQTT_OK | BIT_MQTT_FAIL);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client == nullptr) {
        snprintf(result, len, "not tested (invalid address or no memory)");
        return;
    }
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_test_mqtt, nullptr);
    EventBits_t bits = 0;
    if (esp_mqtt_client_start(client) == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, BIT_MQTT_OK | BIT_MQTT_FAIL, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(config::SETUP_BROKER_TEST_MS));
    }
    esp_mqtt_client_destroy(client);

    if (bits & BIT_MQTT_OK) {
        snprintf(result, len, "connected");
    } else if (s_mqtt_refused != 0) {
        snprintf(result, len, "refused: %s", refusal_text(s_mqtt_refused));
    } else {
        snprintf(result, len, "not reached");
    }
}

void run_test()
{
    settings::Settings s;
    {
        Lock lock;
        s = s_pending;
        s_sta_ip[0] = '\0';
        s_broker[0] = '\0';
    }
    ESP_LOGI(TAG, "Testing Wi-Fi \"%s\"", s.wifi_ssid);

    wifi_config_t sta = {};
    strlcpy(reinterpret_cast<char *>(sta.sta.ssid), s.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char *>(sta.sta.password), s.wifi_password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED | BIT_ASSOCIATED);
    s_last_reason = 0;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }

    const int64_t deadline = esp_timer_get_time() + config::SETUP_WIFI_TEST_MS * 1000LL;
    int rejected = 0;
    bool got_ip = false;
    while (err == ESP_OK) {
        const int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) {
            break;
        }
        const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED, pdTRUE, pdFALSE,
                                                     pdMS_TO_TICKS(left_ms));
        if (bits & BIT_GOT_IP) {
            got_ip = true;
            break;
        }
        if (bits & BIT_DISCONNECTED) {
            if (password_rejected(s_last_reason) && ++rejected >= config::SETUP_WIFI_AUTH_FAILS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    }

    if (!got_ip) {
        esp_wifi_disconnect();
        const uint8_t reason = s_last_reason;
        const bool associated = xEventGroupGetBits(s_events) & BIT_ASSOCIATED;
        if (err != ESP_OK) {
            set_state(FAILED, "Wi-Fi test not started: %s", esp_err_to_name(err));
        } else if (rejected >= config::SETUP_WIFI_AUTH_FAILS || password_rejected(reason)) {
            set_state(FAILED, "Wrong Wi-Fi password for \"%s\".", s.wifi_ssid);
        } else if (network_missing(reason)) {
            set_state(FAILED, "Wi-Fi network \"%s\" not found (or its security does not match).", s.wifi_ssid);
        } else if (associated) {
            set_state(FAILED, "Joined \"%s\" but got no IP address (DHCP).", s.wifi_ssid);
        } else {
            set_state(FAILED, "Could not connect to \"%s\" (reason %u).", s.wifi_ssid, reason);
        }
        ESP_LOGW(TAG, "Wi-Fi test failed, nothing stored (reason %u)", reason);
        return;
    }

    esp_ip4_addr_t ip = {s_got_ip};
    char ip_text[16];
    esp_ip4addr_ntoa(&ip, ip_text, sizeof(ip_text));
    {
        Lock lock;
        snprintf(s_sta_ip, sizeof(s_sta_ip), "%s", ip_text);
    }
    ESP_LOGI(TAG, "Wi-Fi test passed, IP %s", ip_text);

    char broker[96] = "off";
    if (s.mqtt_uri[0] != '\0') {
        set_state(TESTING_BROKER, "Wi-Fi works (IP %s). Trying the MQTT broker...", ip_text);
        test_broker(s, broker, sizeof(broker));
        ESP_LOGI(TAG, "MQTT broker test: %s", broker);
    }
    {
        Lock lock;
        snprintf(s_broker, sizeof(s_broker), "%s", broker);
    }

    // FR-12.5: stored whatever the broker result; the broker may be down now.
    err = settings::save(s);
    if (err != ESP_OK) {
        set_state(FAILED, "Wi-Fi works, but the settings could not be stored: %s", esp_err_to_name(err));
        return;
    }
    set_state(SAVED, "Saved. The station restarts in %lu s; its web page will be at http://%s/",
              static_cast<unsigned long>(config::SETUP_RESTART_DELAY_MS / 1000), ip_text);
    vTaskDelay(pdMS_TO_TICKS(config::SETUP_RESTART_DELAY_MS));
    restart_into(false, "settings saved");
}

// Runs the tests on request, the first scan, and the idle timeout (FR-12.2).
void taskSetup(void *)
{
    scan();
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0) {
            run_test();
        }
        const bool configured = settings::get().wifi_ssid[0] != '\0';
        const State st = state();
        if (configured && s_clients == 0 && st != TESTING_WIFI && st != TESTING_BROKER && st != SAVED &&
            esp_timer_get_time() - s_idle_since_us >= config::SETUP_IDLE_TIMEOUT_MS * 1000LL) {
            restart_into(false, "no device joined the access point for 15 minutes");
        }
    }
}

// --- Captive portal DNS (FR-12.3) ---------------------------------------------

// Answers every A query with the address of the access point.
void taskDns(void *)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "Captive DNS unavailable");
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(nullptr);
        return;
    }
    uint8_t buf[512];
    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        if (n < 12 || (buf[2] & 0x80) != 0 || (buf[4] == 0 && buf[5] == 0)) {
            continue;  // too short, an answer, or no question
        }
        // End of the first question: labels, then type and class.
        size_t pos = 12;
        while (pos < static_cast<size_t>(n) && buf[pos] != 0 && (buf[pos] & 0xC0) == 0) {
            pos += buf[pos] + 1;
        }
        if (pos + 5 > static_cast<size_t>(n) || buf[pos] != 0) {
            continue;
        }
        pos++;
        const bool type_a = buf[pos] == 0 && buf[pos + 1] == 1 && buf[pos + 2] == 0 && buf[pos + 3] == 1;
        pos += 4;
        // Header: answer, recursion desired copied, recursion available, no error;
        // one question, the answer below, no other records.
        buf[2] = 0x80 | (buf[2] & 0x01);
        buf[3] = 0x80;
        buf[4] = 0;
        buf[5] = 1;
        buf[6] = 0;
        buf[7] = type_a ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (type_a && pos + 16 <= sizeof(buf)) {
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + pos, answer, sizeof(answer));
            memcpy(buf + pos + sizeof(answer), &s_ap_ip_addr, 4);
            pos += sizeof(answer) + 4;
        } else {
            buf[7] = 0;
        }
        sendto(sock, buf, pos, 0, reinterpret_cast<sockaddr *>(&from), from_len);
    }
}

// --- Setup page (FR-12.4) ----------------------------------------------------

esp_err_t send_json(httpd_req_t *req, const char *status, const char *body, size_t len)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    char buf[256];
    Text t{buf, sizeof(buf)};
    t.add("{\"error\":");
    t.str(message);
    t.add("}");
    return send_json(req, status, buf, t.len);
}

esp_err_t handle_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_page_start, setup_page_end - setup_page_start - 1);  // EMBED_FILES adds a NUL
}

esp_err_t handle_settings(httpd_req_t *req)
{
    const settings::Settings &s = settings::get();
    constexpr size_t SIZE = 2560;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[SIZE]);
    if (buf == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    Text t{buf.get(), SIZE};
    t.add("{\"device_id\":\"%s\",\"firmware\":", identity::device_id());
    t.str(identity::firmware_version());
    t.add(",\"source\":\"%s\",\"wifi_ssid\":", settings::source_name());
    t.str(s.wifi_ssid);
    t.add(",\"wifi_password_set\":%s,\"mqtt_uri\":", s.wifi_password[0] ? "true" : "false");
    t.str(s.mqtt_uri);
    t.add(",\"mqtt_username\":");
    t.str(s.mqtt_username);
    t.add(",\"mqtt_password_set\":%s,\"name\":", s.mqtt_password[0] ? "true" : "false");
    t.str(s.name);
    t.add(",\"altitude_m\":%ld,\"metar\":", static_cast<long>(s.altitude_m));
    t.str(s.metar);
    t.add(",\"limits\":{\"ssid\":%u,\"wifi_password_min\":%u,\"wifi_password_max\":%u,\"mqtt_uri\":%u,"
          "\"mqtt_username\":%u,\"mqtt_password\":%u,\"name\":%u,\"altitude_min\":%d,\"altitude_max\":%d}}",
          static_cast<unsigned>(config::SETTINGS_SSID_MAX), static_cast<unsigned>(config::SETTINGS_WIFI_PASS_MIN),
          static_cast<unsigned>(config::SETTINGS_WIFI_PASS_MAX), static_cast<unsigned>(config::SETTINGS_MQTT_URI_MAX),
          static_cast<unsigned>(config::SETTINGS_MQTT_USER_MAX), static_cast<unsigned>(config::SETTINGS_MQTT_PASS_MAX),
          static_cast<unsigned>(config::DEVICE_NAME_MAX), config::SETTINGS_ALTITUDE_MIN_M,
          config::SETTINGS_ALTITUDE_MAX_M);
    if (t.full()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "answer too long");
    }
    return send_json(req, "200 OK", buf.get(), t.len);
}

esp_err_t handle_scan(httpd_req_t *req)
{
    char query[32];
    const State st = state();
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "refresh") != nullptr &&
        st != TESTING_WIFI && st != TESTING_BROKER) {
        scan();
    }
    constexpr size_t SIZE = 6144;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[SIZE]);
    if (buf == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    Text t{buf.get(), SIZE};
    t.add("{\"networks\":[");
    {
        Lock lock;
        for (size_t i = 0; i < s_network_count; i++) {
            t.add("%s{\"ssid\":", i ? "," : "");
            t.str(s_networks[i].ssid);
            t.add(",\"rssi\":%d,\"secure\":%s}", s_networks[i].rssi, s_networks[i].secure ? "true" : "false");
        }
    }
    t.add("]}");
    return send_json(req, "200 OK", buf.get(), t.len);
}

esp_err_t handle_status(httpd_req_t *req)
{
    char buf[512];
    Text t{buf, sizeof(buf)};
    {
        Lock lock;
        t.add("{\"state\":\"%s\",\"message\":", STATE_NAMES[s_state]);
        t.str(s_message);
        t.add(",\"ip\":");
        t.str(s_sta_ip);
        t.add(",\"broker\":");
        t.str(s_broker);
        t.add("}");
    }
    return send_json(req, "200 OK", buf, t.len);
}

esp_err_t handle_save(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > config::SETUP_FORM_MAX_BYTES) {
        return send_error(req, "413 Payload Too Large", "Form too large");
    }
    std::unique_ptr<char[]> body(new (std::nothrow) char[req->content_len + 1]);
    if (body == nullptr) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body.get() + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    // FR-12.4: the active set, with the passwords kept unless a new one is
    // given or the box to clear it is ticked.
    settings::Settings s = settings::get();
    struct Field {
        const char *key;
        char *out;
        size_t len;
        const char *label;
    };
    char wifi_password[sizeof(s.wifi_password)] = "";
    char mqtt_password[sizeof(s.mqtt_password)] = "";
    char altitude[12] = "";
    char flag[4] = "";
    const Field fields[] = {
        {"wifi_ssid", s.wifi_ssid, sizeof(s.wifi_ssid), "Wi-Fi network"},
        {"wifi_password", wifi_password, sizeof(wifi_password), "Wi-Fi password"},
        {"mqtt_uri", s.mqtt_uri, sizeof(s.mqtt_uri), "MQTT broker"},
        {"mqtt_username", s.mqtt_username, sizeof(s.mqtt_username), "MQTT user name"},
        {"mqtt_password", mqtt_password, sizeof(mqtt_password), "MQTT password"},
        {"name", s.name, sizeof(s.name), "Name"},
        {"altitude_m", altitude, sizeof(altitude), "Altitude"},
        {"metar", s.metar, sizeof(s.metar), "METAR airport"},
    };
    for (const Field &f : fields) {
        if (form_value(body.get(), f.key, f.out, f.len) < 0) {
            char why[64];
            snprintf(why, sizeof(why), "%s: too long", f.label);
            return send_error(req, "400 Bad Request", why);
        }
    }
    if (wifi_password[0] != '\0') {
        snprintf(s.wifi_password, sizeof(s.wifi_password), "%s", wifi_password);
    } else if (form_value(body.get(), "wifi_password_clear", flag, sizeof(flag)) == 1 && flag[0] == '1') {
        s.wifi_password[0] = '\0';
    }
    if (mqtt_password[0] != '\0') {
        snprintf(s.mqtt_password, sizeof(s.mqtt_password), "%s", mqtt_password);
    } else if (form_value(body.get(), "mqtt_password_clear", flag, sizeof(flag)) == 1 && flag[0] == '1') {
        s.mqtt_password[0] = '\0';
    }
    char *end = nullptr;
    const long alt = strtol(altitude, &end, 10);
    if (altitude[0] == '\0' || *end != '\0') {
        return send_error(req, "400 Bad Request", "Altitude: a whole number of metres");
    }
    s.altitude_m = static_cast<int32_t>(std::clamp<long>(alt, INT32_MIN / 2, INT32_MAX / 2));

    char why[96];
    if (!settings::validate(s, why, sizeof(why))) {
        return send_error(req, "400 Bad Request", why);
    }
    bool busy;
    {
        Lock lock;
        busy = s_state == TESTING_WIFI || s_state == TESTING_BROKER || s_state == SAVED;
        if (!busy) {
            s_pending = s;
            s_state = TESTING_WIFI;
            snprintf(s_message, sizeof(s_message), "Connecting to \"%s\"...", s.wifi_ssid);
        }
    }
    if (busy) {
        return send_error(req, "409 Conflict", "A test is already running");
    }
    xTaskNotifyGive(s_task);
    const char ok[] = "{\"ok\":true}";
    return send_json(req, "202 Accepted", ok, sizeof(ok) - 1);
}

// FR-12.3: any other address leads to the setup page (captive portal).
esp_err_t redirect(httpd_req_t *req, httpd_err_code_t)
{
    char location[32];
    snprintf(location, sizeof(location), "http://%s/", s_ap_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t start_server()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = config::WEB_SERVER_PORT;
    cfg.stack_size = config::SETUP_WEB_STACK;
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, handle_page, nullptr},
        {"/api/settings", HTTP_GET, handle_settings, nullptr},
        {"/api/scan", HTTP_GET, handle_scan, nullptr},
        {"/api/status", HTTP_GET, handle_status, nullptr},
        {"/api/save", HTTP_POST, handle_save, nullptr},
    };
    for (const httpd_uri_t &route : routes) {
        if ((err = httpd_register_uri_handler(server, &route)) != ESP_OK) {
            return err;
        }
    }
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
}

// --- Console (FR-12.6) -------------------------------------------------------

int cmd_setup(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "";
    if (sub[0] == '\0') {
        if (provisioning::active()) {
            printf("Already in setup mode\n");
            provisioning::print_status();
        } else {
            provisioning::enter("console");
        }
    } else if (strcmp(sub, "cancel") == 0) {
        if (!provisioning::active()) {
            printf("Not in setup mode\n");
        } else {
            provisioning::leave("console");
        }
    } else if (strcmp(sub, "show") == 0) {
        settings::print();
    } else if (strcmp(sub, "clear") == 0) {
        const esp_err_t err = settings::clear();
        if (err != ESP_OK) {
            printf("Settings not erased: %s\n", esp_err_to_name(err));
            return 1;
        }
        restart_into(false, "stored settings erased");
    } else {
        printf("Usage: setup | setup cancel | setup show | setup clear\n");
        return 1;
    }
    return 0;
}

}  // namespace

namespace provisioning {

bool wanted_at_start(const char **cause)
{
    const bool requested = s_boot_request == BOOT_SETUP;
    s_boot_request = 0;
    if (requested) {
        *cause = "requested before the restart";
        return true;
    }
    if (settings::get().wifi_ssid[0] == '\0') {
        *cause = "no Wi-Fi network configured";
        return true;
    }
    return false;
}

esp_err_t start(const char *cause)
{
    s_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == nullptr || s_scan_lock == nullptr || s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // FR-12.3: name from the MAC, a new random password at every entry.
    const uint8_t *mac = identity::device_mac();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02x%02x", config::SETUP_AP_PREFIX, mac[4], mac[5]);
    const size_t chars = strlen(config::SETUP_AP_PASS_CHARS);
    for (size_t i = 0; i < config::SETUP_AP_PASS_LEN; i++) {
        s_ap_password[i] = config::SETUP_AP_PASS_CHARS[esp_random() % chars];
    }
    s_ap_password[config::SETUP_AP_PASS_LEN] = '\0';

    esp_err_t err;
    if ((err = esp_netif_init()) != ESP_OK || (err = esp_event_loop_create_default()) != ESP_OK) {
        return err;
    }
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init_cfg)) != ESP_OK ||
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||  // nothing of a test persists
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr)) != ESP_OK) {
        return err;
    }

    wifi_config_t ap_cfg = {};
    strlcpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy(reinterpret_cast<char *>(ap_cfg.ap.password), s_ap_password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = config::SETUP_AP_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 1;
    if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)) != ESP_OK ||
        (err = esp_wifi_set_protocol(WIFI_IF_STA, config::WIFI_PROTOCOLS)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }

    // FR-12.3: the DHCP server hands out the station itself as DNS server.
    esp_netif_ip_info_t ip_info = {};
    esp_netif_get_ip_info(ap, &ip_info);
    s_ap_ip_addr = ip_info.ip.addr;
    esp_ip4addr_ntoa(&ip_info.ip, s_ap_ip, sizeof(s_ap_ip));
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    uint8_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(ap);

    s_idle_since_us = esp_timer_get_time();
    s_active = true;
    if ((err = start_server()) != ESP_OK) {
        return err;
    }
    if (xTaskCreate(taskDns, "setupDns", config::SETUP_DNS_STACK, nullptr, config::SETUP_TASK_PRIO, nullptr) !=
            pdPASS ||
        xTaskCreate(taskSetup, "setup", config::SETUP_TASK_STACK, nullptr, config::SETUP_TASK_PRIO, &s_task) !=
            pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Setup mode (%s): join Wi-Fi \"%s\" with password \"%s\", then open http://%s/", cause,
             s_ap_ssid, s_ap_password, s_ap_ip);
    return ESP_OK;
}

bool active() { return s_active; }

void enter(const char *cause)
{
    restart_into(true, cause);
}

void leave(const char *cause)
{
    if (settings::get().wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No Wi-Fi network configured, setup mode stays");
        return;
    }
    restart_into(false, cause);
}

void on_long_press()
{
    if (active()) {
        leave("button held");
    } else {
        enter("button held");
    }
}

void screen(char lines[SCREEN_LINES][SCREEN_CHARS])
{
    snprintf(lines[0], SCREEN_CHARS, "Wi-Fi %s", s_ap_ssid);
    snprintf(lines[1], SCREEN_CHARS, "Pass  %s", s_ap_password);
    snprintf(lines[2], SCREEN_CHARS, "http://%.14s", s_ap_ip);
    const char *status;
    switch (state()) {
    case TESTING_WIFI:   status = "Testing Wi-Fi..."; break;
    case TESTING_BROKER: status = "Testing broker..."; break;
    case SAVED:          status = "Saved, restarting"; break;
    case FAILED:         status = "Test failed, retry"; break;
    default:             status = s_clients > 0 ? "Phone connected" : "Join with a phone"; break;
    }
    snprintf(lines[3], SCREEN_CHARS, "%s", status);
}

void print_status()
{
    if (!active()) {
        return;
    }
    printf("Setup mode: access point \"%s\", password %s, http://%s/, %d device(s) connected\n", s_ap_ssid,
           s_ap_password, s_ap_ip, s_clients.load());
    Lock lock;
    if (s_state != IDLE) {
        printf("  last test: %s\n", s_message);
    }
}

esp_err_t register_console_command()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "setup";
    cmd.help = "Settings (Wi-Fi, MQTT broker, name, altitude, METAR): 'setup' enters setup mode (access point "
               "and setup page), 'setup cancel' leaves it, 'setup show' lists the settings, 'setup clear' "
               "erases the stored ones (menuconfig values apply). Entering and leaving restart the station";
    cmd.hint = "[cancel | show | clear]";
    cmd.func = cmd_setup;
    return esp_console_cmd_register(&cmd);
}

}  // namespace provisioning

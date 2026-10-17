#include "health.h"

#include <atomic>
#include <cstdio>
#include "config.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "net_time.h"
#include "ping/ping_sock.h"
#include "telemetry.h"
#include "web_server.h"

static const char *TAG = "health";

namespace {

bool s_wifi_enabled = false;
TaskHandle_t s_task = nullptr;

// Network watchdog state, also shown by `sys`.
std::atomic<bool> s_gateway_seen{false};   // armed once the gateway answered
std::atomic<int64_t> s_last_reply_us{0};    // esp_timer time of the last reply
std::atomic<int> s_failed_checks{0};
std::atomic<int> s_forced_reconnects{0};

struct Heap {
    unsigned free_kb, min_kb, largest_kb;
};

Heap heap_now()
{
    return Heap{
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024),
    };
}

esp_netif_t *sta_netif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

void on_ping_end(esp_ping_handle_t, void *args)
{
    xTaskNotifyGive(static_cast<TaskHandle_t>(args));
}

// Pings the default gateway and returns the number of replies (0 = none,
// -1 = could not ping, e.g. no IP address).
int ping_gateway()
{
    esp_netif_ip_info_t info;
    esp_netif_t *netif = sta_netif();
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.gw.addr == 0) {
        return -1;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_set_ip4_u32(&cfg.target_addr, info.gw.addr);
    cfg.count = config::NET_PING_COUNT;
    cfg.interval_ms = 500;
    cfg.timeout_ms = config::NET_PING_TIMEOUT_MS;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = xTaskGetCurrentTaskHandle();
    cbs.on_ping_end = on_ping_end;

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        return -1;
    }
    ulTaskNotifyTake(pdTRUE, 0);  // drop a stale notification
    int replies = -1;
    if (esp_ping_start(ping) == ESP_OK) {
        const TickType_t max_wait =
            pdMS_TO_TICKS(config::NET_PING_COUNT * (500 + config::NET_PING_TIMEOUT_MS) + 1000);
        if (ulTaskNotifyTake(pdTRUE, max_wait) > 0) {
            uint32_t received = 0;
            esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &received, sizeof(received));
            replies = static_cast<int>(received);
        } else {
            esp_ping_stop(ping);
        }
    }
    esp_ping_delete_session(ping);  // not allowed inside the ping callbacks
    return replies;
}

// NFR-2.3: one watchdog step. Counts a failure when Wi-Fi is associated but
// has no IP address, or has one and the gateway does not answer. Not
// associated (access point away) is left to the usual reconnection.
void check_network()
{
    if (!net_time::associated()) {
        s_failed_checks = 0;
        return;
    }

    bool ok = false;
    if (net_time::has_ip()) {
        const int replies = ping_gateway();
        if (replies > 0) {
            ok = true;
            s_last_reply_us = esp_timer_get_time();
            if (!s_gateway_seen.exchange(true)) {
                ESP_LOGI(TAG, "Gateway answers ping, network watchdog armed");
            }
        }
    }
    if (ok) {
        if (s_failed_checks > 0 || s_forced_reconnects > 0) {
            ESP_LOGI(TAG, "Network back");
        }
        s_failed_checks = 0;
        s_forced_reconnects = 0;
        return;
    }
    // Some routers never answer ping; the watchdog only acts after the
    // gateway has answered at least once.
    if (!s_gateway_seen) {
        return;
    }

    const int fails = ++s_failed_checks;
    const Heap h = heap_now();
    ESP_LOGW(TAG, "Wi-Fi associated but %s (%d/%d), heap free %u KB, min %u KB",
             net_time::has_ip() ? "gateway not answering" : "no IP address", fails,
             config::NET_CHECK_FAILS, h.free_kb, h.min_kb);
    if (fails < config::NET_CHECK_FAILS) {
        return;
    }
    s_failed_checks = 0;

    if (s_forced_reconnects >= config::NET_MAX_RECONNECTS) {
        // NFR-2.4: Wi-Fi reconnects did not help; the network stack itself is
        // stuck. The history is in flash and survives the restart.
        ESP_LOGE(TAG, "Network still down after %d Wi-Fi reconnects, restarting", config::NET_MAX_RECONNECTS);
        vTaskDelay(pdMS_TO_TICKS(200));  // let the log line out
        esp_restart();
    }
    const int n = ++s_forced_reconnects;
    ESP_LOGW(TAG, "Forcing Wi-Fi reconnect (%d/%d)", n, config::NET_MAX_RECONNECTS);
    net_time::force_reconnect();
}

void log_health()
{
    const Heap h = heap_now();
    wifi_ap_record_t ap;
    char wifi[16] = "off";
    if (s_wifi_enabled) {
        if (net_time::has_ip() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(wifi, sizeof(wifi), "%d dBm", ap.rssi);
        } else {
            snprintf(wifi, sizeof(wifi), "down");
        }
    }
    ESP_LOGI(TAG, "Heap free %u KB (min %u KB, largest block %u KB), Wi-Fi %s, MQTT %s, web requests %lu",
             h.free_kb, h.min_kb, h.largest_kb, wifi,
             !telemetry::enabled() ? "off" : telemetry::connected() ? "connected" : "disconnected",
             static_cast<unsigned long>(web_server::requests_served()));
}

void taskHealth(void *)
{
    const uint32_t log_every = config::HEALTH_LOG_PERIOD_MS / config::NET_CHECK_PERIOD_MS;
    uint32_t cycle = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::NET_CHECK_PERIOD_MS));
        if (s_wifi_enabled) {
            check_network();
        }
        if (++cycle >= log_every) {
            cycle = 0;
            log_health();
        }
    }
}

const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "exception/panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (supply voltage too low)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

// NFR-2.3: `sys` prints the state needed to diagnose a stuck network.
int cmd_sys(int, char **)
{
    const int64_t up_s = esp_timer_get_time() / 1000000;
    printf("Uptime: %lld d %02lld:%02lld:%02lld, last start: %s\n", up_s / 86400, up_s / 3600 % 24,
           up_s / 60 % 60, up_s % 60, reset_reason_text(esp_reset_reason()));

    const Heap h = heap_now();
    printf("Heap: free %u KB, minimum since start %u KB, largest block %u KB\n", h.free_kb, h.min_kb,
           h.largest_kb);

    if (!s_wifi_enabled) {
        printf("Wi-Fi: not configured\n");
    } else {
        wifi_ap_record_t ap;
        esp_netif_ip_info_t info = {};
        if (esp_netif_t *netif = sta_netif()) {
            esp_netif_get_ip_info(netif, &info);
        }
        printf("Wi-Fi: %s", !net_time::associated() ? "not associated" : net_time::has_ip() ? "connected" : "associated, no IP");
        if (net_time::associated() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            printf(", RSSI %d dBm, channel %u", ap.rssi, ap.primary);
        }
        printf("\nIP " IPSTR ", gateway " IPSTR "\n", IP2STR(&info.ip), IP2STR(&info.gw));

        const int64_t last = s_last_reply_us;
        if (last == 0) {
            printf("Gateway ping: no reply yet (watchdog %s)\n", s_gateway_seen ? "armed" : "not armed");
        } else {
            printf("Gateway ping: last reply %lld s ago, failed checks %d, forced reconnects %d\n",
                   (esp_timer_get_time() - last) / 1000000, s_failed_checks.load(), s_forced_reconnects.load());
        }
    }
    printf("MQTT: %s\n", !telemetry::enabled() ? "off" : telemetry::connected() ? "connected" : "disconnected");
    printf("Web: %lu requests served\n", static_cast<unsigned long>(web_server::requests_served()));
    return 0;
}

}  // namespace

namespace health {

esp_err_t start(bool wifi_enabled)
{
    s_wifi_enabled = wifi_enabled;
    return xTaskCreate(taskHealth, "taskHealth", config::HEALTH_TASK_STACK, nullptr, config::HEALTH_TASK_PRIO,
                       &s_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t register_console_command()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "sys";
    cmd.help = "System health: uptime, last start reason, heap, Wi-Fi, gateway ping, MQTT, web";
    cmd.func = cmd_sys;
    return esp_console_cmd_register(&cmd);
}

void log_reset_reason()
{
    ESP_LOGI(TAG, "Started by: %s", reset_reason_text(esp_reset_reason()));
}

}  // namespace health

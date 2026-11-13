#include "health.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "identity.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "metar.h"
#include "net_time.h"
#include "ping/ping_sock.h"
#include "provisioning.h"
#include "quality.h"
#include "settings.h"
#include "telemetry.h"
#include "web_server.h"

static const char *TAG = "health";

namespace health {

Heap heap_now()
{
    return Heap{
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024),
    };
}

}  // namespace health

namespace {

using health::Heap;
using health::heap_now;

bool s_wifi_enabled = false;
TaskHandle_t s_task = nullptr;

// Network watchdog state, also shown by `sys`.
std::atomic<bool> s_gateway_seen{false};   // armed once the gateway answered
std::atomic<int64_t> s_last_ok_us{0};       // esp_timer time of the last sign of traffic
std::atomic<const char *> s_last_ok_source{"none"};
uint32_t s_last_web_requests = 0;
std::atomic<int> s_failed_checks{0};
std::atomic<int> s_forced_reconnects{0};

// Text of net_time::format_drops() with every reason in use.
constexpr size_t DROPS_TEXT_BYTES = 40 + 24 * config::WIFI_DROP_REASONS;

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

// NFR-2.3: one watchdog step. The network counts as working when there is a
// sign of traffic: the MQTT client is connected (its keepalive proves round
// trips to the broker), the web server answered a request since the last
// check, or the gateway answers ping. Some routers drop ping now and then, so
// the ping is only the last resort. A check fails when Wi-Fi is associated and
// has no IP address or shows no sign of traffic. Not associated (access point
// away) is left to the usual reconnection.
void check_network()
{
    if (!net_time::associated()) {
        s_failed_checks = 0;
        return;
    }

    const char *source = nullptr;
    if (net_time::has_ip()) {
        const uint32_t requests = web_server::requests_served();
        // Until armed, ping on every check so that a router that answers ping
        // is detected even while MQTT keeps the checks passing.
        if (!s_gateway_seen && ping_gateway() > 0) {
            s_gateway_seen = true;
            ESP_LOGI(TAG, "Gateway answers ping, network watchdog armed");
            source = "gateway ping";
        } else if (telemetry::connected()) {
            source = "MQTT";
        } else if (requests != s_last_web_requests) {
            source = "web";
        } else if (s_gateway_seen && ping_gateway() > 0) {
            source = "gateway ping";
        }
        s_last_web_requests = requests;
    }
    const bool ok = source != nullptr;
    if (ok) {
        s_last_ok_us = esp_timer_get_time();
        s_last_ok_source = source;
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
             net_time::has_ip() ? "no traffic (MQTT down, no web request, no ping reply)" : "no IP address", fails,
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
    char drops[DROPS_TEXT_BYTES];
    net_time::format_drops(drops, sizeof(drops));
    ESP_LOGI(TAG, "Heap free %u KB (min %u KB, largest block %u KB), Wi-Fi %s, MQTT %s, web requests %lu, Wi-Fi drops %s",
             h.free_kb, h.min_kb, h.largest_kb, wifi,
             !telemetry::enabled() ? "off" : telemetry::connected() ? "connected" : "disconnected",
             static_cast<unsigned long>(web_server::requests_served()), drops);
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

// --- NFR-2.5: `ping` and `arp` console commands -----------------------------

void on_diag_ping_success(esp_ping_handle_t ping, void *)
{
    uint16_t seq = 0;
    uint8_t ttl = 0;
    uint32_t elapsed = 0, size = 0;
    ip_addr_t from;
    esp_ping_get_profile(ping, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(ping, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(ping, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(ping, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(ping, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    printf("%lu bytes from %s: seq=%u ttl=%u time=%lu ms\n", static_cast<unsigned long>(size), ipaddr_ntoa(&from),
           seq, ttl, static_cast<unsigned long>(elapsed));
}

void on_diag_ping_timeout(esp_ping_handle_t ping, void *)
{
    uint16_t seq = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("seq=%u: no reply\n", seq);
}

// `ping <ip> [count [size]]`: pings any address from the station, to see
// whether the path from the station to a device works (NFR-2.5).
int cmd_ping(int argc, char **argv)
{
    ip_addr_t target = {};
    if (argc < 2 || !ipaddr_aton(argv[1], &target) || !IP_IS_V4(&target)) {
        printf("Usage: ping <IPv4 address> [count (1..%lu)] [size (0..%lu bytes)]\n",
               static_cast<unsigned long>(config::DIAG_PING_MAX_COUNT),
               static_cast<unsigned long>(config::DIAG_PING_MAX_SIZE));
        return 1;
    }
    const long count = argc >= 3 ? strtol(argv[2], nullptr, 10) : config::DIAG_PING_COUNT;
    const long size = argc >= 4 ? strtol(argv[3], nullptr, 10) : config::DIAG_PING_SIZE;
    if (count < 1 || count > static_cast<long>(config::DIAG_PING_MAX_COUNT) || size < 0 ||
        size > static_cast<long>(config::DIAG_PING_MAX_SIZE)) {
        printf("Count 1..%lu, size 0..%lu bytes\n", static_cast<unsigned long>(config::DIAG_PING_MAX_COUNT),
               static_cast<unsigned long>(config::DIAG_PING_MAX_SIZE));
        return 1;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = count;
    cfg.data_size = size;
    cfg.interval_ms = config::DIAG_PING_INTERVAL_MS;
    cfg.timeout_ms = config::DIAG_PING_TIMEOUT_MS;
    cfg.task_stack_size = config::DIAG_PING_TASK_STACK;
    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = xTaskGetCurrentTaskHandle();
    cbs.on_ping_success = on_diag_ping_success;
    cbs.on_ping_timeout = on_diag_ping_timeout;
    cbs.on_ping_end = on_ping_end;

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        printf("Ping not possible (no network?)\n");
        return 1;
    }
    printf("PING %s, %ld bytes, %ld times\n", argv[1], size, count);
    ulTaskNotifyTake(pdTRUE, 0);  // drop a stale notification
    if (esp_ping_start(ping) == ESP_OK) {
        const TickType_t max_wait =
            pdMS_TO_TICKS(count * (config::DIAG_PING_INTERVAL_MS + config::DIAG_PING_TIMEOUT_MS) + 1000);
        if (ulTaskNotifyTake(pdTRUE, max_wait) == 0) {
            esp_ping_stop(ping);
        }
        uint32_t sent = 0, received = 0;
        esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
        esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &received, sizeof(received));
        printf("%lu sent, %lu received, %lu %% lost\n", static_cast<unsigned long>(sent),
               static_cast<unsigned long>(received),
               static_cast<unsigned long>(sent ? (sent - received) * 100 / sent : 0));
    }
    esp_ping_delete_session(ping);
    return 0;
}

// The ARP table is read and cleared in the lwIP thread (no core locking).
struct ArpDump {
    struct Entry {
        ip4_addr_t ip;
        uint8_t mac[6];
        char netif[4];
    } entries[ARP_TABLE_SIZE];
    size_t count;
    bool clear;
    SemaphoreHandle_t done;
};

void arp_in_tcpip_thread(void *arg)
{
    auto *dump = static_cast<ArpDump *>(arg);
    if (dump->clear) {
        struct netif *nif;
        NETIF_FOREACH(nif) {
            etharp_cleanup_netif(nif);
        }
    }
    dump->count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = nullptr;
        struct netif *nif = nullptr;
        struct eth_addr *eth = nullptr;
        if (etharp_get_entry(i, &ip, &nif, &eth) == 1) {
            ArpDump::Entry &e = dump->entries[dump->count++];
            e.ip = *ip;
            memcpy(e.mac, eth->addr, sizeof(e.mac));
            snprintf(e.netif, sizeof(e.netif), "%c%c", nif->name[0], nif->name[1]);
        }
    }
    xSemaphoreGive(dump->done);
}

// `arp` lists the station's ARP table; `arp clear` empties it first. A device
// appears there when its ARP request for the station arrived (or after the
// station talked to it), so this shows whether a device's packets get here.
int cmd_arp(int argc, char **argv)
{
    const bool clear = argc >= 2 && strcmp(argv[1], "clear") == 0;
    if (argc >= 2 && !clear) {
        printf("Usage: arp | arp clear\n");
        return 1;
    }
    auto *dump = static_cast<ArpDump *>(calloc(1, sizeof(ArpDump)));
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (dump == nullptr || done == nullptr) {
        free(dump);
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
        printf("Out of memory\n");
        return 1;
    }
    dump->clear = clear;
    dump->done = done;
    if (tcpip_callback(arp_in_tcpip_thread, dump) != ERR_OK) {
        printf("Network stack not running\n");
        vSemaphoreDelete(done);
        free(dump);
        return 1;
    }
    xSemaphoreTake(done, portMAX_DELAY);  // the lwIP thread always answers
    if (clear) {
        printf("ARP table cleared\n");
    }
    printf("%-16s %-18s %s\n", "IP address", "MAC address", "interface");
    for (size_t i = 0; i < dump->count; i++) {
        const ArpDump::Entry &e = dump->entries[i];
        printf("%-16s %02x:%02x:%02x:%02x:%02x:%02x  %s\n", ip4addr_ntoa(&e.ip), e.mac[0], e.mac[1], e.mac[2],
               e.mac[3], e.mac[4], e.mac[5], e.netif);
    }
    if (dump->count == 0) {
        printf("(empty)\n");
    }
    vSemaphoreDelete(done);
    free(dump);
    return 0;
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

    if (provisioning::active()) {
        provisioning::print_status();  // FR-12.3
    } else if (!s_wifi_enabled) {
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
        wifi_ps_type_t ps = WIFI_PS_NONE;
        esp_wifi_get_ps(&ps);
        printf(", power save %s", ps == WIFI_PS_NONE ? "off" : "on");
        printf("\nIP " IPSTR ", gateway " IPSTR "\n", IP2STR(&info.ip), IP2STR(&info.gw));

        char dns[net_time::DNS_TEXT_BYTES];
        net_time::format_dns(dns, sizeof(dns));
        printf("DNS: %s\n", dns);

        char drops[DROPS_TEXT_BYTES];
        net_time::format_drops(drops, sizeof(drops));
        printf("Wi-Fi drops since start: %s\n", drops);

        const int64_t last = s_last_ok_us;
        printf("Network watchdog: %s", s_gateway_seen ? "armed" : "not armed (gateway has not answered ping yet)");
        if (last != 0) {
            printf(", last sign of traffic %lld s ago (%s)", (esp_timer_get_time() - last) / 1000000,
                   s_last_ok_source.load());
        }
        printf(", failed checks %d, forced reconnects %d\n", s_failed_checks.load(), s_forced_reconnects.load());
    }
    // FR-10.1 .. FR-10.3, FR-12.1
    printf("Settings: %s (`setup show`)\n", settings::source_name());
    printf("Device: %s%s%s%s, firmware %s\n", identity::device_id(), identity::device_name()[0] ? " (\"" : "",
           identity::device_name(), identity::device_name()[0] ? "\")" : "", identity::firmware_version());
    for (size_t i = 0; i < identity::SENSOR_COUNT; i++) {
        identity::Sensor s;
        if (identity::sensor(i, s)) {
            char id[24];
            identity::format_sensor_id(s.type, s.fingerprint, id, sizeof(id));
            char cal[96];
            quality::format_offsets(quality::offsets(i), cal, sizeof(cal));
            printf("Sensor %u: %s at i2c@0x%02x, %s, rejected samples %lu\n", static_cast<unsigned>(i), id,
                   s.address, cal, static_cast<unsigned long>(quality::rejected(i)));  // FR-1.6
        }
    }
    // FR-11.2, FR-11.4
    printf("Altitude: %d m, sea-level pressure factor %.4f\n", quality::altitude_m(), quality::msl_factor());
    metar::print_status();
    if (telemetry::enabled()) {
        printf("MQTT: %s, client ID \"%s\"\n", telemetry::connected() ? "connected" : "disconnected",
               telemetry::client_id());
    } else {
        printf("MQTT: off\n");
    }
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
    cmd.help = "System health: uptime, last start reason, heap, Wi-Fi, gateway ping, device, sensor and "
               "calibration, altitude, METAR check, MQTT, web, settings source, setup mode";
    cmd.func = cmd_sys;
    esp_err_t err = esp_console_cmd_register(&cmd);

    // NFR-2.5
    if (err == ESP_OK) {
        esp_console_cmd_t ping = {};
        ping.command = "ping";
        ping.help = "Pings an address from the station: 'ping <ip> [count] [size]', e.g. 'ping 192.168.0.106 10 1400'";
        ping.hint = "<ip> [count] [size]";
        ping.func = cmd_ping;
        err = esp_console_cmd_register(&ping);
    }
    if (err == ESP_OK) {
        esp_console_cmd_t arp = {};
        arp.command = "arp";
        arp.help = "ARP table of the station: 'arp' lists it, 'arp clear' empties it (a device that then reaches "
                   "the station appears again)";
        arp.hint = "[clear]";
        arp.func = cmd_arp;
        err = esp_console_cmd_register(&arp);
    }
    return err;
}

void log_reset_reason()
{
    ESP_LOGI(TAG, "Started by: %s", reset_reason_text(esp_reset_reason()));
}

}  // namespace health

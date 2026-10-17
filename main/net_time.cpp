#include "net_time.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

static const char *TAG = "net_time";

namespace {

std::atomic<bool> s_synced{false};
std::atomic<bool> s_started{false};
std::atomic<bool> s_connected{false};   // has an IP address
std::atomic<bool> s_associated{false};  // associated with the access point
TimerHandle_t s_retry_timer = nullptr;

void retry_connect(TimerHandle_t)
{
    esp_wifi_connect();
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_associated = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // FR-5.5: keep trying, but not in a tight loop.
        s_connected = false;
        s_associated = false;
        const auto *info = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying in %lu ms", info->reason,
                 static_cast<unsigned long>(config::WIFI_RETRY_DELAY_MS));
        xTimerStart(s_retry_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *info = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&info->ip_info.ip));
        s_connected = true;
    }
}

void on_time_sync(struct timeval *)
{
    if (!s_synced.exchange(true)) {
        ESP_LOGI(TAG, "Time synchronized via NTP");
    }
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

}  // namespace

namespace net_time {

esp_err_t start()
{
    // FR-5.2: local time zone for localtime().
    setenv("TZ", config::TIMEZONE, 1);
    tzset();

    if (config::WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured (idf.py menuconfig), time of day disabled");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        return err;
    }
    if ((err = esp_netif_init()) != ESP_OK ||
        (err = esp_event_loop_create_default()) != ESP_OK) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    s_retry_timer = xTimerCreate("wifiRetry", pdMS_TO_TICKS(config::WIFI_RETRY_DELAY_MS),
                                 pdFALSE, nullptr, retry_connect);
    if (s_retry_timer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init_cfg)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr)) != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_cfg = {};
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid), config::WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.password), config::WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = config::WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    s_started = true;

    // NFR-2.3: power save can make the station deaf with some routers.
    if ((err = esp_wifi_set_ps(config::WIFI_POWER_SAVE ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE)) != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi power save %s", config::WIFI_POWER_SAVE ? "on (modem sleep)" : "off");

    // FR-5.1: SNTP retries and re-syncs periodically on its own.
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(config::NTP_SERVER);
    sntp_cfg.sync_cb = on_time_sync;
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\", NTP server %s", config::WIFI_SSID, config::NTP_SERVER);
    }
    return err;
}

bool local_time(struct tm &out)
{
    if (!s_synced) {
        return false;
    }
    const time_t now = time(nullptr);
    localtime_r(&now, &out);
    return true;
}

int wifi_level()
{
    if (!s_started) {
        return -1;
    }
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    if (ap.rssi >= config::WIFI_RSSI_GOOD_DBM) {
        return 3;
    }
    return ap.rssi >= config::WIFI_RSSI_FAIR_DBM ? 2 : 1;
}

bool associated() { return s_associated; }

bool has_ip() { return s_connected; }

esp_err_t force_reconnect()
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_disconnect();  // -> WIFI_EVENT_STA_DISCONNECTED -> retry
}

}  // namespace net_time

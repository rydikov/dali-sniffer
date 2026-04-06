/* Wi-Fi WebSocket Chat Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <cstdio>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "web_server.h"

namespace {

constexpr const char *kTag = "example";

void init_nvs()
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
}

void init_network_stack()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
}

void wifi_event_handler(void *arg,
                        esp_event_base_t event_base,
                        int32_t event_id,
                        void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(kTag, "Wi-Fi started, connecting to AP \"%s\"", CONFIG_WIFI_SSID);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(kTag, "Connected to AP \"%s\"", CONFIG_WIFI_SSID);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            auto *disconnected = static_cast<wifi_event_sta_disconnected_t *>(event_data);

            ESP_LOGW(kTag,
                     "Disconnected from AP \"%s\", reason=%d. Retrying...",
                     CONFIG_WIFI_SSID,
                     disconnected->reason);
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        char ip_address[16];

        esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
        ESP_LOGI(kTag, "Got IP address: %s", ip_address);
    }
}

void register_wifi_handlers()
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
}

wifi_config_t build_wifi_config()
{
    wifi_config_t wifi_config = {};

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    std::snprintf(reinterpret_cast<char *>(wifi_config.sta.ssid),
                  sizeof(wifi_config.sta.ssid),
                  "%s",
                  CONFIG_WIFI_SSID);
    std::snprintf(reinterpret_cast<char *>(wifi_config.sta.password),
                  sizeof(wifi_config.sta.password),
                  "%s",
                  CONFIG_WIFI_PASSWORD);

    if (CONFIG_WIFI_PASSWORD[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    return wifi_config;
}

void start_wifi_station(wifi_config_t &wifi_config)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "Initializing Wi-Fi station");

    if (CONFIG_WIFI_SSID[0] == '\0') {
        ESP_LOGE(kTag, "Wi-Fi SSID is empty. Configure CONFIG_WIFI_SSID in menuconfig.");
        return;
    }

    init_nvs();
    init_network_stack();
    register_wifi_handlers();
    wifi_config_t wifi_config = build_wifi_config();
    start_wifi_station(wifi_config);
    ESP_ERROR_CHECK(web_server_start());
}

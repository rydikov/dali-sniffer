#include "dali_status_heartbeat.h"

#include <cstddef>
#include <cstdint>

#include "dali_sniffer.h"
#include "dali_state_sync.h"
#include "devices_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_DALI_STATUS_HEARTBEAT_SECONDS
#define CONFIG_DALI_STATUS_HEARTBEAT_SECONDS 0
#endif

namespace {

constexpr const char *kTag = "dali_status_heartbeat";
constexpr size_t kMaxDeviceMatches = 64;
constexpr uint8_t kDaliQueryStatus = 0x90;
constexpr TickType_t kDaliReplyWaitAfterQueryTicks = pdMS_TO_TICKS(25);
constexpr uint32_t kHeartbeatTaskStackSize = 4096;
constexpr UBaseType_t kHeartbeatTaskPriority = 5;

void heartbeat_task(void *arg)
{
    (void)arg;

    const TickType_t heartbeat_ticks = pdMS_TO_TICKS(CONFIG_DALI_STATUS_HEARTBEAT_SECONDS * 1000U);

    while (true) {
        devices_api_device_match_t matches[kMaxDeviceMatches] = {};
        size_t match_count = 0;
        const esp_err_t find_err =
            devices_api_find_devices("broadcast", false, 0, false, 0, matches, kMaxDeviceMatches, &match_count);
        if (find_err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to load saved devices for heartbeat: %s", esp_err_to_name(find_err));
        }

        for (size_t i = 0; i < match_count; ++i) {
            const uint8_t address_byte = static_cast<uint8_t>((matches[i].address << 1) | 0x01);

            dali_state_sync_expect_status_reply(matches[i].address);
            const esp_err_t send_err = dali_sniffer_send_frame(address_byte, kDaliQueryStatus);
            if (send_err != ESP_OK) {
                ESP_LOGW(kTag,
                         "Failed to send heartbeat QUERY_STATUS for device %u: %s",
                         static_cast<unsigned>(matches[i].address),
                         esp_err_to_name(send_err));
                dali_state_sync_clear_expected_reply();
                continue;
            }

            vTaskDelay(kDaliReplyWaitAfterQueryTicks);
        }

        vTaskDelay(heartbeat_ticks);
    }
}

}  // namespace

esp_err_t dali_status_heartbeat_start(void)
{
    if (CONFIG_DALI_STATUS_HEARTBEAT_SECONDS <= 0) {
        ESP_LOGI(kTag, "DALI status heartbeat is disabled");
        return ESP_OK;
    }

    ESP_LOGI(kTag, "Starting DALI status heartbeat every %d seconds", CONFIG_DALI_STATUS_HEARTBEAT_SECONDS);
    if (xTaskCreate(heartbeat_task,
                    "dali_hb",
                    kHeartbeatTaskStackSize,
                    nullptr,
                    kHeartbeatTaskPriority,
                    nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

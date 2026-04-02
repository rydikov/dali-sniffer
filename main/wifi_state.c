#include "wifi_state.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_status_mutex;
static wifi_status_t s_wifi_status;

esp_err_t wifi_state_init(const char *ssid)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_status.connected = false;
    snprintf(s_wifi_status.ssid, sizeof(s_wifi_status.ssid), "%s", ssid != NULL ? ssid : "");
    s_wifi_status.ip[0] = '\0';

    return ESP_OK;
}

void wifi_state_copy(wifi_status_t *status)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *status = s_wifi_status;
    xSemaphoreGive(s_status_mutex);
}

void wifi_state_update(bool connected, const char *ip)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_wifi_status.connected = connected;
    snprintf(s_wifi_status.ip, sizeof(s_wifi_status.ip), "%s", ip != NULL ? ip : "");
    xSemaphoreGive(s_status_mutex);
}

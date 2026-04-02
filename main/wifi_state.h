#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"

typedef struct {
    bool connected;
    char ssid[sizeof(((wifi_config_t *)0)->sta.ssid)];
    char ip[16];
} wifi_status_t;

esp_err_t wifi_state_init(const char *ssid);
void wifi_state_copy(wifi_status_t *status);
void wifi_state_update(bool connected, const char *ip);

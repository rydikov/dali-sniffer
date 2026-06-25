#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

struct devices_api_device_match_t {
    uint8_t address;
};

enum devices_api_device_status_t : uint8_t {
    DEVICES_API_DEVICE_STATUS_OFF = 0,
    DEVICES_API_DEVICE_STATUS_ON = 1,
    DEVICES_API_DEVICE_STATUS_FAILURE = 2,
};

esp_err_t devices_api_get_handler(httpd_req_t *req);
esp_err_t devices_api_post_handler(httpd_req_t *req);
esp_err_t devices_api_put_handler(httpd_req_t *req);
esp_err_t devices_api_delete_handler(httpd_req_t *req);
esp_err_t devices_api_update_device_status(uint8_t address, devices_api_device_status_t status);
esp_err_t devices_api_find_devices(const char *address_kind,
                                   bool has_address_value,
                                   int address_value,
                                   bool require_scene,
                                   uint8_t scene,
                                   devices_api_device_match_t *matches,
                                   size_t max_matches,
                                   size_t *match_count);
esp_err_t devices_api_find_brightness_devices(const char *address_kind,
                                              bool has_address_value,
                                              int address_value,
                                              bool require_scene,
                                              uint8_t scene,
                                              devices_api_device_match_t *matches,
                                              size_t max_matches,
                                              size_t *match_count);
esp_err_t devices_api_find_color_temperature_devices(const char *address_kind,
                                                     bool has_address_value,
                                                     int address_value,
                                                     bool require_scene,
                                                     uint8_t scene,
                                                     devices_api_device_match_t *matches,
                                                     size_t max_matches,
                                                     size_t *match_count);
esp_err_t devices_api_find_rgbw_devices(const char *address_kind,
                                        bool has_address_value,
                                        int address_value,
                                        bool require_scene,
                                        uint8_t scene,
                                        devices_api_device_match_t *matches,
                                        size_t max_matches,
                                        size_t *match_count);

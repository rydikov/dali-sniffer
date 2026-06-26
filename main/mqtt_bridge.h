#pragma once

#include <stdint.h>

#include "dali_protocol.h"
#include "dali_sniffer.h"
#include "esp_err.h"

esp_err_t mqtt_bridge_start(void);
bool mqtt_bridge_is_enabled(void);
void mqtt_bridge_publish_status(void);
void mqtt_bridge_publish_command_request(const char *origin, const char *command_text, bool accepted);
void mqtt_bridge_publish_command_result(const char *origin,
                                        const char *command_text,
                                        const dali_command_exec_result_t &result);
void mqtt_bridge_publish_device_brightness(uint8_t address, uint8_t level);
void mqtt_bridge_publish_device_color_temperature(uint8_t address, uint32_t kelvin);
void mqtt_bridge_publish_device_rgb(uint8_t address, uint8_t red, uint8_t green, uint8_t blue);
void mqtt_bridge_publish_device_white(uint8_t address, uint8_t level);

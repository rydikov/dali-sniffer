#pragma once

#include "dali_protocol.h"
#include "dali_sniffer.h"
#include "esp_err.h"

esp_err_t mqtt_bridge_start(void);
bool mqtt_bridge_is_enabled(void);
void mqtt_bridge_publish_status(void);
void mqtt_bridge_publish_sniffer_event(const dali_frame_event_t &frame);
void mqtt_bridge_publish_command_request(const char *origin, const char *command_text, bool accepted);
void mqtt_bridge_publish_command_result(const char *origin,
                                        const char *command_text,
                                        const dali_command_exec_result_t &result);


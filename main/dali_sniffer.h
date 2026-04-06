#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct dali_frame_event_t {
    uint32_t data;
    uint8_t length;
    bool is_backward_frame;
};

esp_err_t dali_sniffer_start(void);
QueueHandle_t dali_sniffer_get_event_queue(void);
esp_err_t dali_sniffer_send_frame(uint8_t address_byte, uint8_t data_byte);

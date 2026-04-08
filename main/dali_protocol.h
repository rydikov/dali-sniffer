#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dali_sniffer.h"
#include "esp_err.h"

enum class DaliTargetKind : uint8_t {
    Lamp,
    Group,
    Broadcast,
};

struct dali_target_t {
    DaliTargetKind kind;
    uint8_t index;
};

constexpr size_t kMaxDaliTxFrames = 16;

struct dali_tx_plan_t {
    size_t frame_count;
    dali_tx_frame_t frames[kMaxDaliTxFrames];
};

struct dali_command_exec_result_t {
    bool accepted;
    bool sent;
    size_t frame_count;
    char feedback[160];
};

struct dali_frame_description_t {
    bool is_backward_frame;
    uint8_t bit_length;
    uint32_t raw_value;
    char raw_hex[9];
    char text[128];
    char address_kind[16];
    bool has_address_value;
    int address_value;
    char address_label[24];
    bool has_command_name;
    char command_name[48];
    bool has_command_index;
    uint8_t command_index;
    bool has_level;
    uint8_t level;
    bool has_arg;
    uint8_t arg;
    bool has_opcode;
    uint8_t opcode;
};

bool dali_build_tx_plan(const char *command_text, dali_tx_plan_t *plan, char *error_text, size_t error_text_size);
esp_err_t dali_execute_command_text(const char *command_text, dali_command_exec_result_t *result);
void dali_describe_frame(const dali_frame_event_t &frame, dali_frame_description_t *description);


#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dali_protocol.h"

enum class mqtt_device_set_status_t : uint8_t {
    // Topic не относится к device/group *_set управлению.
    NotMatched,
    // Retained *_set сообщение распознано, но специально не исполняется.
    IgnoredRetained,
    // Команда валидна, command содержит готовое действие для очереди MQTT bridge.
    Ready,
    // Topic или payload похожи на *_set команду, но не прошли валидацию.
    Invalid,
};

enum class mqtt_device_set_command_kind_t : uint8_t {
    // Команду нужно передать в общий текстовый DALI parser.
    Text,
    // Команда уже собрана как набор DALI forward frames.
    Frames,
};

struct mqtt_device_set_command_t {
    mqtt_device_set_command_kind_t kind;
    // Человекочитаемое имя команды для command_request/command_result.
    char command_text[128];
    size_t frame_count;
    dali_tx_frame_t frames[kMaxDaliTxFrames];
};

struct mqtt_device_set_result_t {
    mqtt_device_set_status_t status;
    mqtt_device_set_command_t command;
    char error[128];
};

// Разбирает MQTT topic вида:
//   <root>/device/<address>/<state>_set
//   <root>/group/<address>/<state>_set
//
// Модуль не отправляет кадры сам: он только валидирует входные данные и
// возвращает готовую команду, чтобы mqtt_bridge сохранил единую очередь
// последовательного исполнения DALI-команд.
void mqtt_device_set_build_command(const char *root_topic,
                                   const char *topic,
                                   int topic_len,
                                   const char *payload,
                                   int payload_len,
                                   bool retain,
                                   mqtt_device_set_result_t *result);

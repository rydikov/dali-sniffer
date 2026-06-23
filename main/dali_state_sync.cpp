#include "dali_state_sync.h"

#include <cstddef>
#include <cstring>

#include "devices_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_bridge.h"

namespace {

constexpr const char *kTag = "dali_state_sync";
constexpr size_t kMaxBrightnessMatches = 64;
constexpr uint8_t kDaliQueryActualLevel = 0xA0;
constexpr uint32_t kKelvinMiredNumerator = 1000000UL;
constexpr TickType_t kDaliReplyWaitAfterQueryTicks = pdMS_TO_TICKS(25);

struct dt8_color_temperature_context_t {
    uint8_t dtr0;
    uint8_t dtr1;
    bool has_dtr0;
    bool has_dtr1;
};

// Backward reply в DALI не содержит адреса устройства. Поэтому после
// QUERY_ACTUAL_LEVEL запоминаем, к каким нашим устройствам относился запрос,
// и следующий reply трактуем как уровень яркости именно для них.
devices_api_device_match_t s_pending_brightness_matches[kMaxBrightnessMatches] = {};
size_t s_pending_brightness_match_count = 0;
bool s_has_pending_brightness_reply = false;
dt8_color_temperature_context_t s_dt8_color_temperature = {};

bool description_command_is(const dali_frame_description_t &description, const char *command_name)
{
    return description.has_command_name && std::strcmp(description.command_name, command_name) == 0;
}

void clear_pending_brightness_reply()
{
    s_has_pending_brightness_reply = false;
    s_pending_brightness_match_count = 0;
}

void record_pending_brightness_reply(const devices_api_device_match_t *matches, size_t match_count)
{
    if (match_count == 0) {
        clear_pending_brightness_reply();
        return;
    }

    std::memcpy(s_pending_brightness_matches, matches, sizeof(matches[0]) * match_count);
    s_pending_brightness_match_count = match_count;
    s_has_pending_brightness_reply = true;
}

esp_err_t find_brightness_devices_for_description(const dali_frame_description_t &description,
                                                  bool require_scene,
                                                  uint8_t scene,
                                                  devices_api_device_match_t *matches,
                                                  size_t max_matches,
                                                  size_t *match_count)
{
    // dali_protocol уже разобрал short/group/broadcast и положил результат в
    // description. Здесь только прокидываем эти метаданные в storage lookup.
    return devices_api_find_brightness_devices(description.address_kind,
                                               description.has_address_value,
                                               description.address_value,
                                               require_scene,
                                               scene,
                                               matches,
                                               max_matches,
                                               match_count);
}

esp_err_t find_color_temperature_devices_for_description(const dali_frame_description_t &description,
                                                         devices_api_device_match_t *matches,
                                                         size_t max_matches,
                                                         size_t *match_count)
{
    // Для CT scene-фильтр не нужен: DT8_SET_COLOUR_TEMP_TC адресуется напрямую
    // short/group/broadcast target-у, который уже лежит в description.
    return devices_api_find_color_temperature_devices(description.address_kind,
                                                      description.has_address_value,
                                                      description.address_value,
                                                      false,
                                                      0,
                                                      matches,
                                                      max_matches,
                                                      match_count);
}

void publish_brightness_matches(const devices_api_device_match_t *matches, size_t match_count, uint8_t level)
{
    // Payload brightness намеренно простой: raw DALI level как строковое число.
    // Формирование topic спрятано внутри mqtt_bridge.
    for (size_t i = 0; i < match_count; ++i) {
        mqtt_bridge_publish_device_brightness(matches[i].address, level);
    }
}

void publish_color_temperature_matches(const devices_api_device_match_t *matches, size_t match_count, uint32_t kelvin)
{
    // Цветовую температуру публикуем так же просто, как brightness: plain number,
    // но уже в привычных Kelvin, а не в DALI mired.
    for (size_t i = 0; i < match_count; ++i) {
        mqtt_bridge_publish_device_color_temperature(matches[i].address, kelvin);
    }
}

void remember_query_actual_level_target(const dali_frame_description_t &description)
{
    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxBrightnessMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  false,
                                                                  0,
                                                                  matches,
                                                                  kMaxBrightnessMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve QUERY_ACTUAL_LEVEL target: %s", esp_err_to_name(err));
        clear_pending_brightness_reply();
        return;
    }

    if (match_count == 0) {
        // Если запрос не относится к сохранённым brightness-устройствам,
        // следующий reply нам не нужен.
        clear_pending_brightness_reply();
        return;
    }

    record_pending_brightness_reply(matches, match_count);
}

void publish_pending_brightness_reply(const dali_frame_event_t &frame)
{
    if (!s_has_pending_brightness_reply) {
        return;
    }

    // Backward frame несёт только один байт данных; для QUERY_ACTUAL_LEVEL это
    // фактическая яркость устройства.
    publish_brightness_matches(s_pending_brightness_matches,
                               s_pending_brightness_match_count,
                               static_cast<uint8_t>(frame.data & 0xFF));
    clear_pending_brightness_reply();
}

void publish_target_brightness(const dali_frame_description_t &description, uint8_t level)
{
    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxBrightnessMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  false,
                                                                  0,
                                                                  matches,
                                                                  kMaxBrightnessMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve brightness target: %s", esp_err_to_name(err));
        return;
    }

    // Один DALI target может соответствовать нескольким нашим устройствам:
    // group и broadcast разворачиваются в список сохранённых адресов.
    publish_brightness_matches(matches, match_count, level);
}

void query_scene_brightness(const dali_frame_description_t &description)
{
    if (!description.has_command_index || description.command_index > 15) {
        return;
    }

    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxBrightnessMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  true,
                                                                  description.command_index,
                                                                  matches,
                                                                  kMaxBrightnessMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve GO_TO_SCENE target: %s", esp_err_to_name(err));
        return;
    }

    // GO_TO_SCENE сам не содержит уровня яркости. Для каждого подходящего
    // устройства отправляем short QUERY_ACTUAL_LEVEL, чтобы последующий reply
    // дал реальное состояние brightness.
    for (size_t i = 0; i < match_count; ++i) {
        const uint8_t address_byte = static_cast<uint8_t>((matches[i].address << 1) | 0x01);

        // Backward reply не содержит адреса, поэтому перед каждым follow-up
        // QUERY_ACTUAL_LEVEL запоминаем ровно то устройство, которое сейчас
        // опрашиваем после GO_TO_SCENE.
        record_pending_brightness_reply(&matches[i], 1);

        const esp_err_t send_err = dali_sniffer_send_frame(address_byte, kDaliQueryActualLevel);
        if (send_err != ESP_OK) {
            ESP_LOGW(kTag,
                     "Failed to query brightness for device %u after scene %u: %s",
                     static_cast<unsigned>(matches[i].address),
                     static_cast<unsigned>(description.command_index),
                     esp_err_to_name(send_err));
            clear_pending_brightness_reply();
            continue;
        }

        // Небольшая пауза снижает шанс отправить следующий query до того, как
        // устройство успеет ответить на предыдущий.
        vTaskDelay(kDaliReplyWaitAfterQueryTicks);
    }
}

void remember_dt8_color_temperature_dtr(const dali_frame_description_t &description)
{
    if (!description.has_arg) {
        return;
    }

    if (description_command_is(description, "DATA_TRANSFER_REGISTER0")) {
        s_dt8_color_temperature.dtr0 = description.arg;
        s_dt8_color_temperature.has_dtr0 = true;
        return;
    }

    if (description_command_is(description, "DATA_TRANSFER_REGISTER1")) {
        s_dt8_color_temperature.dtr1 = description.arg;
        s_dt8_color_temperature.has_dtr1 = true;
    }
}

uint32_t kelvin_from_mired(uint16_t mired)
{
    // Округляем до ближайшего Kelvin: mired=194 даст 5155K, а не 5154K.
    return (kKelvinMiredNumerator + (mired / 2U)) / mired;
}

void publish_target_color_temperature(const dali_frame_description_t &description)
{
    if (!s_dt8_color_temperature.has_dtr0 || !s_dt8_color_temperature.has_dtr1) {
        ESP_LOGW(kTag, "Skipping DT8 colour temperature without complete DTR0/DTR1 context");
        return;
    }

    const uint16_t mired =
        static_cast<uint16_t>((static_cast<uint16_t>(s_dt8_color_temperature.dtr1) << 8) |
                              s_dt8_color_temperature.dtr0);
    if (mired == 0) {
        ESP_LOGW(kTag, "Skipping DT8 colour temperature with zero mired value");
        return;
    }

    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxBrightnessMatches] = {};
    const esp_err_t err = find_color_temperature_devices_for_description(description,
                                                                         matches,
                                                                         kMaxBrightnessMatches,
                                                                         &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve colour temperature target: %s", esp_err_to_name(err));
        return;
    }

    publish_color_temperature_matches(matches, match_count, kelvin_from_mired(mired));
}

}  // namespace

void dali_state_sync_handle_frame(const dali_frame_event_t &frame, const dali_frame_description_t &description)
{
    // Единая точка синхронизации DALI-трафика с MQTT state topics. WebSocket UI
    // остаётся в web_server, а device-state логика живёт здесь.
    if (description.is_backward_frame) {
        publish_pending_brightness_reply(frame);
        return;
    }

    if (description_command_is(description, "QUERY_ACTUAL_LEVEL")) {
        remember_query_actual_level_target(description);
        return;
    }

    clear_pending_brightness_reply();

    // DTR0/DTR1 сами по себе не адресованы устройству. Они подготавливают
    // 16-битное DT8 Tc значение для следующего DT8_SET_COLOUR_TEMP_TC.
    if (description_command_is(description, "DATA_TRANSFER_REGISTER0") ||
        description_command_is(description, "DATA_TRANSFER_REGISTER1")) {
        remember_dt8_color_temperature_dtr(description);
        return;
    }

    // DAPC явно несёт уровень, а OFF в DALI является командой яркости 0.
    if (description_command_is(description, "DAPC") && description.has_level) {
        publish_target_brightness(description, description.level);
        return;
    }

    if (description_command_is(description, "OFF")) {
        publish_target_brightness(description, 0);
        return;
    }

    if (description_command_is(description, "GO_TO_SCENE")) {
        query_scene_brightness(description);
        return;
    }

    if (description_command_is(description, "DT8_SET_COLOUR_TEMP_TC")) {
        publish_target_color_temperature(description);
    }
}

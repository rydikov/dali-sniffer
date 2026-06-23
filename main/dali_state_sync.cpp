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
constexpr size_t kMaxDeviceMatches = 64;
constexpr uint8_t kDaliQueryActualLevel = 0xA0;
constexpr uint32_t kKelvinMiredNumerator = 1000000UL;
constexpr TickType_t kDaliReplyWaitAfterQueryTicks = pdMS_TO_TICKS(25);

enum class pending_reply_kind_t : uint8_t {
    None,
    Brightness,
    ColorTemperatureHigh,
    ColorTemperatureLow,
    RgbwChannel,
};

enum class rgbw_channel_t : uint8_t {
    Red,
    Green,
    Blue,
    White,
};

struct dt8_color_temperature_context_t {
    uint8_t dtr0;
    uint8_t dtr1;
    uint8_t dtr2;
    bool has_dtr0;
    bool has_dtr1;
    bool has_dtr2;
};

struct pending_reply_context_t {
    pending_reply_kind_t kind;
    devices_api_device_match_t matches[kMaxDeviceMatches];
    size_t match_count;
    uint8_t color_temperature_high;
    rgbw_channel_t rgbw_channel;
};

struct dt8_temporary_rgb_context_t {
    devices_api_device_match_t matches[kMaxDeviceMatches];
    size_t match_count;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    bool has_rgb;
};

// Backward reply в DALI не содержит адреса устройства. Поэтому после
// query запоминаем, к каким нашим устройствам относился запрос, и следующий
// reply трактуем в контексте именно этой команды.
pending_reply_context_t s_pending_reply = {};
dt8_color_temperature_context_t s_dt8_color_temperature = {};
dt8_temporary_rgb_context_t s_dt8_temporary_rgb = {};
uint8_t s_dt8_query_colour_value_index = 0;
bool s_has_dt8_query_colour_value_index = false;

bool description_command_is(const dali_frame_description_t &description, const char *command_name)
{
    return description.has_command_name && std::strcmp(description.command_name, command_name) == 0;
}

uint32_t kelvin_from_mired(uint16_t mired);

bool description_is_dt8_query_colour_value(const dali_frame_description_t &description)
{
    if (description_command_is(description, "DT8_QUERY_COLOUR_VALUE")) {
        return true;
    }

    // Некоторые внешние DALI-декодеры показывают QueryColourValue на opcode
    // 0xFA. Оставляем эту ветку, чтобы корректно привязать реальные ответы
    // из шины даже если локальная таблица имён считает 0xFA другой командой.
    return description.bit_length == 16 && (description.raw_value & 0xFFU) == 0xFAU;
}

bool description_has_addressed_target(const dali_frame_description_t &description)
{
    return std::strcmp(description.address_kind, "short") == 0 ||
           std::strcmp(description.address_kind, "group") == 0 ||
           std::strcmp(description.address_kind, "broadcast") == 0;
}

bool description_is_arc_power_level(const dali_frame_description_t &description)
{
    if (!description.has_level || !description_has_addressed_target(description)) {
        return false;
    }

    return description_command_is(description, "DAPC") ||
           description_command_is(description, "ARC_POWER") ||
           description_command_is(description, "ArcPower");
}

bool description_is_relative_brightness_command(const dali_frame_description_t &description)
{
    return description_command_is(description, "UP") ||
           description_command_is(description, "DOWN") ||
           description_command_is(description, "STEP_UP") ||
           description_command_is(description, "STEP_DOWN") ||
           description_command_is(description, "RECALL_MAX_LEVEL") ||
           description_command_is(description, "RECALL_MIN_LEVEL") ||
           description_command_is(description, "ON_AND_STEP_UP") ||
           description_command_is(description, "STEP_DOWN_AND_OFF") ||
           description_command_is(description, "GO_TO_LAST_ACTIVE_LEVEL");
}

void clear_pending_reply()
{
    s_pending_reply.kind = pending_reply_kind_t::None;
    s_pending_reply.match_count = 0;
    s_pending_reply.color_temperature_high = 0;
    s_pending_reply.rgbw_channel = rgbw_channel_t::Red;
}

void clear_dt8_temporary_rgb()
{
    s_dt8_temporary_rgb.match_count = 0;
    s_dt8_temporary_rgb.red = 0;
    s_dt8_temporary_rgb.green = 0;
    s_dt8_temporary_rgb.blue = 0;
    s_dt8_temporary_rgb.has_rgb = false;
}

void record_pending_reply(pending_reply_kind_t kind,
                          const devices_api_device_match_t *matches,
                          size_t match_count,
                          uint8_t color_temperature_high = 0,
                          rgbw_channel_t rgbw_channel = rgbw_channel_t::Red)
{
    if (kind == pending_reply_kind_t::None || match_count == 0) {
        clear_pending_reply();
        return;
    }

    std::memcpy(s_pending_reply.matches, matches, sizeof(matches[0]) * match_count);
    s_pending_reply.match_count = match_count;
    s_pending_reply.color_temperature_high = color_temperature_high;
    s_pending_reply.rgbw_channel = rgbw_channel;
    s_pending_reply.kind = kind;
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

esp_err_t find_rgbw_devices_for_description(const dali_frame_description_t &description,
                                            devices_api_device_match_t *matches,
                                            size_t max_matches,
                                            size_t *match_count)
{
    return devices_api_find_rgbw_devices(description.address_kind,
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

void publish_rgbw_matches(const devices_api_device_match_t *matches,
                          size_t match_count,
                          rgbw_channel_t channel,
                          uint8_t level)
{
    for (size_t i = 0; i < match_count; ++i) {
        switch (channel) {
        case rgbw_channel_t::Red:
            mqtt_bridge_publish_device_red(matches[i].address, level);
            break;
        case rgbw_channel_t::Green:
            mqtt_bridge_publish_device_green(matches[i].address, level);
            break;
        case rgbw_channel_t::Blue:
            mqtt_bridge_publish_device_blue(matches[i].address, level);
            break;
        case rgbw_channel_t::White:
            mqtt_bridge_publish_device_white(matches[i].address, level);
            break;
        }
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
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  false,
                                                                  0,
                                                                  matches,
                                                                  kMaxDeviceMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve QUERY_ACTUAL_LEVEL target: %s", esp_err_to_name(err));
        clear_pending_reply();
        return;
    }

    if (match_count == 0) {
        // Если запрос не относится к сохранённым brightness-устройствам,
        // следующий reply нам не нужен.
        clear_pending_reply();
        return;
    }

    record_pending_reply(pending_reply_kind_t::Brightness, matches, match_count);
}

void remember_dt8_query_colour_value_target(const dali_frame_description_t &description)
{
    if (!s_has_dt8_query_colour_value_index) {
        clear_pending_reply();
        return;
    }

    if (s_dt8_query_colour_value_index == 2) {
        size_t match_count = 0;
        devices_api_device_match_t matches[kMaxDeviceMatches] = {};
        const esp_err_t err =
            find_color_temperature_devices_for_description(description, matches, kMaxDeviceMatches, &match_count);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to resolve DT8 colour temperature query target: %s", esp_err_to_name(err));
            clear_pending_reply();
            return;
        }

        record_pending_reply(pending_reply_kind_t::ColorTemperatureHigh, matches, match_count);
        return;
    }

    rgbw_channel_t channel = rgbw_channel_t::Red;
    bool has_channel = true;
    switch (s_dt8_query_colour_value_index) {
    case 9:
        channel = rgbw_channel_t::Red;
        break;
    case 10:
        channel = rgbw_channel_t::Green;
        break;
    case 11:
        channel = rgbw_channel_t::Blue;
        break;
    case 12:
        channel = rgbw_channel_t::White;
        break;
    default:
        has_channel = false;
        break;
    }

    if (!has_channel) {
        clear_pending_reply();
        return;
    }

    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_rgbw_devices_for_description(description, matches, kMaxDeviceMatches, &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve DT8 RGBW query target: %s", esp_err_to_name(err));
        clear_pending_reply();
        return;
    }

    record_pending_reply(pending_reply_kind_t::RgbwChannel, matches, match_count, 0, channel);
}

void remember_query_content_dtr_target()
{
    if (s_pending_reply.kind != pending_reply_kind_t::ColorTemperatureHigh ||
        s_pending_reply.match_count == 0) {
        clear_pending_reply();
        return;
    }

    s_pending_reply.kind = pending_reply_kind_t::ColorTemperatureLow;
}

void publish_pending_reply(const dali_frame_event_t &frame)
{
    if (s_pending_reply.kind == pending_reply_kind_t::None) {
        return;
    }

    const uint8_t value = static_cast<uint8_t>(frame.data & 0xFF);
    switch (s_pending_reply.kind) {
    case pending_reply_kind_t::Brightness:
        publish_brightness_matches(s_pending_reply.matches, s_pending_reply.match_count, value);
        clear_pending_reply();
        return;
    case pending_reply_kind_t::ColorTemperatureHigh:
        s_pending_reply.color_temperature_high = value;
        return;
    case pending_reply_kind_t::ColorTemperatureLow: {
        const uint16_t mired =
            static_cast<uint16_t>((static_cast<uint16_t>(s_pending_reply.color_temperature_high) << 8) | value);
        if (mired != 0) {
            publish_color_temperature_matches(s_pending_reply.matches,
                                              s_pending_reply.match_count,
                                              kelvin_from_mired(mired));
        }
        clear_pending_reply();
        return;
    }
    case pending_reply_kind_t::RgbwChannel:
        publish_rgbw_matches(s_pending_reply.matches, s_pending_reply.match_count, s_pending_reply.rgbw_channel, value);
        clear_pending_reply();
        return;
    case pending_reply_kind_t::None:
        return;
    }
}

void publish_target_brightness(const dali_frame_description_t &description, uint8_t level)
{
    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  false,
                                                                  0,
                                                                  matches,
                                                                  kMaxDeviceMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve brightness target: %s", esp_err_to_name(err));
        return;
    }

    // Один DALI target может соответствовать нескольким нашим устройствам:
    // group и broadcast разворачиваются в список сохранённых адресов.
    publish_brightness_matches(matches, match_count, level);
}

void query_brightness_matches(const devices_api_device_match_t *matches,
                              size_t match_count,
                              const char *reason)
{
    for (size_t i = 0; i < match_count; ++i) {
        const uint8_t address_byte = static_cast<uint8_t>((matches[i].address << 1) | 0x01);

        // Backward reply не содержит адреса, поэтому перед каждым follow-up
        // QUERY_ACTUAL_LEVEL запоминаем ровно то устройство, которое сейчас
        // опрашиваем.
        record_pending_reply(pending_reply_kind_t::Brightness, &matches[i], 1);

        const esp_err_t send_err = dali_sniffer_send_frame(address_byte, kDaliQueryActualLevel);
        if (send_err != ESP_OK) {
            ESP_LOGW(kTag,
                     "Failed to query brightness for device %u after %s: %s",
                     static_cast<unsigned>(matches[i].address),
                     reason != nullptr ? reason : "brightness command",
                     esp_err_to_name(send_err));
            clear_pending_reply();
            continue;
        }

        // Небольшая пауза снижает шанс отправить следующий query до того, как
        // устройство успеет ответить на предыдущий.
        vTaskDelay(kDaliReplyWaitAfterQueryTicks);
    }
}

void query_scene_brightness(const dali_frame_description_t &description)
{
    if (!description.has_command_index || description.command_index > 15) {
        return;
    }

    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  true,
                                                                  description.command_index,
                                                                  matches,
                                                                  kMaxDeviceMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve GO_TO_SCENE target: %s", esp_err_to_name(err));
        return;
    }

    query_brightness_matches(matches, match_count, "GO_TO_SCENE");
}

void query_relative_brightness(const dali_frame_description_t &description)
{
    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_brightness_devices_for_description(description,
                                                                  false,
                                                                  0,
                                                                  matches,
                                                                  kMaxDeviceMatches,
                                                                  &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve relative brightness target: %s", esp_err_to_name(err));
        return;
    }

    query_brightness_matches(matches,
                             match_count,
                             description.has_command_name ? description.command_name : "relative brightness command");
}

void remember_dt8_temporary_rgb(const dali_frame_description_t &description)
{
    if (!s_dt8_color_temperature.has_dtr0 ||
        !s_dt8_color_temperature.has_dtr1 ||
        !s_dt8_color_temperature.has_dtr2) {
        ESP_LOGW(kTag, "Skipping DT8 temporary RGB without complete DTR0/DTR1/DTR2 context");
        clear_dt8_temporary_rgb();
        return;
    }

    size_t match_count = 0;
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_rgbw_devices_for_description(description,
                                                            matches,
                                                            kMaxDeviceMatches,
                                                            &match_count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to resolve DT8 temporary RGB target: %s", esp_err_to_name(err));
        clear_dt8_temporary_rgb();
        return;
    }

    if (match_count == 0) {
        clear_dt8_temporary_rgb();
        return;
    }

    std::memcpy(s_dt8_temporary_rgb.matches, matches, sizeof(matches[0]) * match_count);
    s_dt8_temporary_rgb.match_count = match_count;
    s_dt8_temporary_rgb.red = s_dt8_color_temperature.dtr0;
    s_dt8_temporary_rgb.green = s_dt8_color_temperature.dtr1;
    s_dt8_temporary_rgb.blue = s_dt8_color_temperature.dtr2;
    s_dt8_temporary_rgb.has_rgb = true;
}

void publish_dt8_temporary_rgb()
{
    if (!s_dt8_temporary_rgb.has_rgb || s_dt8_temporary_rgb.match_count == 0) {
        return;
    }

    publish_rgbw_matches(s_dt8_temporary_rgb.matches,
                         s_dt8_temporary_rgb.match_count,
                         rgbw_channel_t::Red,
                         s_dt8_temporary_rgb.red);
    publish_rgbw_matches(s_dt8_temporary_rgb.matches,
                         s_dt8_temporary_rgb.match_count,
                         rgbw_channel_t::Green,
                         s_dt8_temporary_rgb.green);
    publish_rgbw_matches(s_dt8_temporary_rgb.matches,
                         s_dt8_temporary_rgb.match_count,
                         rgbw_channel_t::Blue,
                         s_dt8_temporary_rgb.blue);
    clear_dt8_temporary_rgb();
}

void remember_dt8_color_temperature_dtr(const dali_frame_description_t &description)
{
    if (!description.has_arg) {
        return;
    }

    if (description_command_is(description, "DATA_TRANSFER_REGISTER0")) {
        s_dt8_color_temperature.dtr0 = description.arg;
        s_dt8_color_temperature.has_dtr0 = true;
        s_dt8_query_colour_value_index = description.arg;
        s_has_dt8_query_colour_value_index = true;
        return;
    }

    if (description_command_is(description, "DATA_TRANSFER_REGISTER1")) {
        s_dt8_color_temperature.dtr1 = description.arg;
        s_dt8_color_temperature.has_dtr1 = true;
        return;
    }

    if (description_command_is(description, "DATA_TRANSFER_REGISTER2")) {
        s_dt8_color_temperature.dtr2 = description.arg;
        s_dt8_color_temperature.has_dtr2 = true;
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
    devices_api_device_match_t matches[kMaxDeviceMatches] = {};
    const esp_err_t err = find_color_temperature_devices_for_description(description,
                                                                         matches,
                                                                         kMaxDeviceMatches,
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
        publish_pending_reply(frame);
        return;
    }

    if (description_command_is(description, "QUERY_ACTUAL_LEVEL")) {
        remember_query_actual_level_target(description);
        return;
    }

    if (description_is_dt8_query_colour_value(description)) {
        remember_dt8_query_colour_value_target(description);
        return;
    }

    if (description_command_is(description, "QUERY_CONTENT_DTR")) {
        remember_query_content_dtr_target();
        return;
    }

    clear_pending_reply();

    // DTR0/DTR1/DTR2 сами по себе не адресованы устройству. Они подготавливают
    // DT8 Tc/RGB значения для следующих DT8-команд. При
    // query-последовательностях DTR0 также выбирает DT8 colour value index.
    if (description_command_is(description, "DATA_TRANSFER_REGISTER0") ||
        description_command_is(description, "DATA_TRANSFER_REGISTER1") ||
        description_command_is(description, "DATA_TRANSFER_REGISTER2")) {
        remember_dt8_color_temperature_dtr(description);
        return;
    }

    // DAPC/ArcPower явно несёт raw DALI уровень яркости.
    if (description_is_arc_power_level(description)) {
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

    if (description_is_relative_brightness_command(description)) {
        query_relative_brightness(description);
        return;
    }

    if (description_command_is(description, "DT8_SET_TEMPORARY_RGB_DIMLEVEL")) {
        remember_dt8_temporary_rgb(description);
        return;
    }

    if (description_command_is(description, "DT8_ACTIVATE")) {
        publish_dt8_temporary_rgb();
        return;
    }

    if (description_command_is(description, "DT8_SET_COLOUR_TEMP_TC")) {
        publish_target_color_temperature(description);
    }
}

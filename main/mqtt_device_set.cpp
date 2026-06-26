#include "mqtt_device_set.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kTopicBufferSize = 96;
constexpr size_t kPayloadBufferSize = 48;

// MQTT set-топики поддерживают только short address и group address.
// Broadcast намеренно не добавлен: в плане управления он не описан, а случайная
// broadcast-команда из MQTT слишком легко затронет всю DALI-шину.
enum class target_kind_t : uint8_t {
    Device,
    Group,
};

// Разделяем "не наш topic" и "наш topic, но ошибка". Первое молча игнорируется,
// второе публикуется как rejected command_result, чтобы пользователь видел
// проблему в MQTT events.
enum class parse_topic_status_t : uint8_t {
    NotMatched,
    Invalid,
    Parsed,
};

struct parsed_topic_t {
    target_kind_t kind;
    uint8_t address;
    char state[32];
};

char *trim_ascii(char *text)
{
    if (text == nullptr) {
        return nullptr;
    }

    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) {
        ++text;
    }

    char *end = text + std::strlen(text);
    while (end > text && std::isspace(static_cast<unsigned char>(end[-1])) != 0) {
        --end;
    }
    *end = '\0';
    return text;
}

void set_invalid(mqtt_device_set_result_t *result, const char *message)
{
    result->status = mqtt_device_set_status_t::Invalid;
    std::snprintf(result->error, sizeof(result->error), "%s", message != nullptr ? message : "Invalid set topic");
}

bool ends_with(const char *text, const char *suffix)
{
    if (text == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t text_len = std::strlen(text);
    const size_t suffix_len = std::strlen(suffix);
    return text_len >= suffix_len && std::strcmp(text + text_len - suffix_len, suffix) == 0;
}

bool copy_slice(char *buffer, size_t buffer_size, const char *value, int value_len)
{
    if (buffer == nullptr || buffer_size == 0 || value == nullptr || value_len < 0 ||
        static_cast<size_t>(value_len) >= buffer_size) {
        return false;
    }

    std::memcpy(buffer, value, static_cast<size_t>(value_len));
    buffer[value_len] = '\0';
    return true;
}

bool parse_uint_range(const char *text, unsigned min_value, unsigned max_value, unsigned *value)
{
    if (text == nullptr || value == nullptr) {
        return false;
    }

    // strtoul допускает пробелы в начале, а хвост проверяем вручную. Это даёт
    // нормальное поведение для payload вроде " 128\n", но отклоняет "128abc".
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (text == end || *trim_ascii(end) != '\0' || parsed < min_value || parsed > max_value) {
        return false;
    }

    *value = static_cast<unsigned>(parsed);
    return true;
}

bool parse_u8_csv(char *payload, uint8_t *values, size_t expected_count)
{
    if (payload == nullptr || values == nullptr || expected_count == 0) {
        return false;
    }

    // payload изменяется на месте: запятые заменяются на '\0', чтобы не
    // тащить сюда отдельный tokenizer ради коротких r,g,b / w,a,f строк.
    size_t value_count = 0;
    char *cursor = payload;
    while (cursor != nullptr && *cursor != '\0') {
        char *next = std::strchr(cursor, ',');
        if (next != nullptr) {
            *next++ = '\0';
        }

        if (value_count >= expected_count) {
            return false;
        }

        unsigned parsed = 0;
        if (!parse_uint_range(trim_ascii(cursor), 0, 255, &parsed)) {
            return false;
        }

        values[value_count++] = static_cast<uint8_t>(parsed);
        cursor = next;
    }

    return value_count == expected_count;
}

uint8_t command_address_byte(target_kind_t kind, uint8_t address, bool command)
{
    // DALI forward address byte:
    //   short DAPC:  AAAAAA0
    //   short cmd:   AAAAAA1
    //   group DAPC:  100AAAA0
    //   group cmd:   100AAAA1
    if (kind == target_kind_t::Group) {
        return static_cast<uint8_t>(0x80 | (address << 1) | (command ? 0x01 : 0x00));
    }

    return static_cast<uint8_t>((address << 1) | (command ? 0x01 : 0x00));
}

bool append_frame(mqtt_device_set_command_t *command, uint8_t address_byte, uint8_t data_byte)
{
    if (command == nullptr || command->frame_count >= kMaxDaliTxFrames) {
        return false;
    }

    command->frames[command->frame_count].bit_length = 16;
    command->frames[command->frame_count].data[0] = address_byte;
    command->frames[command->frame_count].data[1] = data_byte;
    command->frames[command->frame_count].data[2] = 0;
    command->frame_count = command->frame_count + 1;
    return true;
}

bool append_dt8_enable(mqtt_device_set_command_t *command)
{
    // ENABLE DEVICE TYPE 8. Его повторяют перед DT8-командами, как и в
    // существующем DT8 RGB/Tc коде, чтобы следующая команда трактовалась как DT8.
    return append_frame(command, 0xC1, 0x08);
}

bool append_dt8_temporary_waf(mqtt_device_set_command_t *command,
                              uint8_t address_byte,
                              uint8_t selector,
                              uint8_t value)
{
    // WAF строится так же, как RGB temporary dimlevel:
    //   DTR1 = selector канала W/A/F
    //   DTR0 = значение канала
    //   ENABLE DT8
    //   target -> DT8_SET_TEMPORARY_WAF_DIMLEVEL (0xEC)
    return append_frame(command, 0xC3, selector) &&
           append_frame(command, 0xA3, value) &&
           append_dt8_enable(command) &&
           append_frame(command, address_byte, 0xEC);
}

bool append_dt8_temporary_rgb(mqtt_device_set_command_t *command,
                              uint8_t address_byte,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue)
{
    // Такой порядок повторяет последовательность внешнего DALI-контроллера:
    //   DTR0 = R
    //   DTR1 = G
    //   DTR2 = B
    //   ENABLE DT8
    //   target -> DT8_SET_TEMPORARY_RGB_DIMLEVEL (0xEB)
    return append_frame(command, 0xA3, red) &&
           append_frame(command, 0xC3, green) &&
           append_frame(command, 0xC5, blue) &&
           append_dt8_enable(command) &&
           append_frame(command, address_byte, 0xEB);
}

bool topic_prefix_matches(const char *topic, const char *root_topic, const char *prefix, const char **rest)
{
    if (topic == nullptr || root_topic == nullptr || prefix == nullptr || rest == nullptr) {
        return false;
    }

    const size_t topic_len = std::strlen(topic);
    const size_t root_len = std::strlen(root_topic);
    const size_t prefix_len = std::strlen(prefix);

    if (topic_len < root_len + prefix_len ||
        std::strncmp(topic, root_topic, root_len) != 0 ||
        std::strncmp(topic + root_len, prefix, prefix_len) != 0) {
        return false;
    }

    *rest = topic + root_len + prefix_len;
    return true;
}

void set_topic_command_label(const parsed_topic_t &topic, mqtt_device_set_command_t *command)
{
    if (command == nullptr) {
        return;
    }

    std::snprintf(command->command_text,
                  sizeof(command->command_text),
                  "%s %u %s",
                  topic.kind == target_kind_t::Group ? "group" : "device",
                  static_cast<unsigned>(topic.address),
                  topic.state);
}

parse_topic_status_t parse_topic(const char *root_topic, const char *topic, parsed_topic_t *parsed)
{
    const char *rest = nullptr;
    unsigned max_address = 63;

    if (topic_prefix_matches(topic, root_topic, "/device/", &rest)) {
        parsed->kind = target_kind_t::Device;
        max_address = 63;
    } else if (topic_prefix_matches(topic, root_topic, "/group/", &rest)) {
        parsed->kind = target_kind_t::Group;
        max_address = 15;
    } else {
        return parse_topic_status_t::NotMatched;
    }

    // После root ожидается ровно device/<0..63>/<state> или group/<0..15>/<state>.
    // Лишние сегменты отклоняются ниже проверкой '/' в state.
    char *end = nullptr;
    const unsigned long address = std::strtoul(rest, &end, 10);
    if (rest == end || address > max_address || end == nullptr || *end != '/') {
        return parse_topic_status_t::Invalid;
    }

    const char *state = end + 1;
    if (*state == '\0' || std::strchr(state, '/') != nullptr || std::strlen(state) >= sizeof(parsed->state)) {
        return parse_topic_status_t::Invalid;
    }

    parsed->address = static_cast<uint8_t>(address);
    std::snprintf(parsed->state, sizeof(parsed->state), "%s", state);
    return parse_topic_status_t::Parsed;
}

const char *target_text(target_kind_t kind)
{
    return kind == target_kind_t::Group ? "group" : "lamp";
}

void build_brightness_command(const parsed_topic_t &topic, unsigned level, mqtt_device_set_command_t *command)
{
    // brightness_set - единственный простой DAPC случай: data byte содержит
    // raw arc power level, а младший бит address byte должен быть 0.
    command->kind = mqtt_device_set_command_kind_t::Frames;
    std::snprintf(command->command_text,
                  sizeof(command->command_text),
                  "%s %u brightness_set %u",
                  topic.kind == target_kind_t::Group ? "group" : "device",
                  static_cast<unsigned>(topic.address),
                  level);
    append_frame(command,
                 command_address_byte(topic.kind, topic.address, false),
                 static_cast<uint8_t>(level));
}

void build_text_command(const parsed_topic_t &topic,
                        const char *action_format,
                        unsigned first,
                        unsigned second,
                        unsigned third,
                        mqtt_device_set_command_t *command)
{
    // Для color_temperature_set переиспользуем существующий текстовый DALI
    // parser. Так mired-конверсия DT8 Tc остаётся в одном месте.
    command->kind = mqtt_device_set_command_kind_t::Text;
    std::snprintf(command->command_text,
                  sizeof(command->command_text),
                  "%s %u -> ",
                  target_text(topic.kind),
                  static_cast<unsigned>(topic.address));
    const size_t prefix_len = std::strlen(command->command_text);
    if (second == 0xFFFFFFFFU) {
        std::snprintf(command->command_text + prefix_len,
                      sizeof(command->command_text) - prefix_len,
                      action_format,
                      first);
    } else {
        std::snprintf(command->command_text + prefix_len,
                      sizeof(command->command_text) - prefix_len,
                      action_format,
                      first,
                      second,
                      third);
    }
}

bool build_rgb_command(const parsed_topic_t &topic, const uint8_t *values, mqtt_device_set_command_t *command)
{
    const uint8_t address_byte = command_address_byte(topic.kind, topic.address, true);
    command->kind = mqtt_device_set_command_kind_t::Frames;
    std::snprintf(command->command_text,
                  sizeof(command->command_text),
                  "%s %u rgb_set %u,%u,%u",
                  topic.kind == target_kind_t::Group ? "group" : "device",
                  static_cast<unsigned>(topic.address),
                  static_cast<unsigned>(values[0]),
                  static_cast<unsigned>(values[1]),
                  static_cast<unsigned>(values[2]));

    return append_dt8_temporary_rgb(command, address_byte, values[0], values[1], values[2]) &&
           append_dt8_enable(command) &&
           append_frame(command, address_byte, 0xE2);
}

bool build_white_command(const parsed_topic_t &topic, const uint8_t *values, mqtt_device_set_command_t *command)
{
    // В DALI DT8 WAF channels выбираются через DTR1 bitmask:
    //   0x08 = W, 0x10 = A, 0x20 = F.
    // После записи трёх temporary значений отправляем DT8_ACTIVATE.
    constexpr uint8_t kWafSelectors[3] = {0x08, 0x10, 0x20};
    const uint8_t address_byte = command_address_byte(topic.kind, topic.address, true);
    command->kind = mqtt_device_set_command_kind_t::Frames;
    std::snprintf(command->command_text,
                  sizeof(command->command_text),
                  "%s %u white_set %u,%u,%u",
                  topic.kind == target_kind_t::Group ? "group" : "device",
                  static_cast<unsigned>(topic.address),
                  static_cast<unsigned>(values[0]),
                  static_cast<unsigned>(values[1]),
                  static_cast<unsigned>(values[2]));

    for (size_t i = 0; i < 3; ++i) {
        if (!append_dt8_temporary_waf(command, address_byte, kWafSelectors[i], values[i])) {
            return false;
        }
    }

    return append_dt8_enable(command) && append_frame(command, address_byte, 0xE2);
}

}  // namespace

void mqtt_device_set_build_command(const char *root_topic,
                                   const char *topic,
                                   int topic_len,
                                   const char *payload,
                                   int payload_len,
                                   bool retain,
                                   mqtt_device_set_result_t *result)
{
    if (result == nullptr) {
        return;
    }

    std::memset(result, 0, sizeof(*result));
    result->status = mqtt_device_set_status_t::NotMatched;

    char topic_text[kTopicBufferSize] = {};
    char payload_text[kPayloadBufferSize] = {};
    parsed_topic_t parsed = {};

    if (root_topic == nullptr || !copy_slice(topic_text, sizeof(topic_text), topic, topic_len)) {
        return;
    }

    // MQTT callbacks дают topic/payload как pointer + length без гарантии
    // завершающего '\0', поэтому перед обычным C string parsing копируем в
    // локальные буферы фиксированного размера.
    const parse_topic_status_t parse_status = parse_topic(root_topic, topic_text, &parsed);
    if (parse_status == parse_topic_status_t::NotMatched) {
        return;
    }

    if (parse_status == parse_topic_status_t::Invalid) {
        if (ends_with(topic_text, "_set")) {
            std::snprintf(result->command.command_text, sizeof(result->command.command_text), "%s", topic_text);
            set_invalid(result, "Invalid device/group set topic");
        }
        return;
    }

    set_topic_command_label(parsed, &result->command);

    if (std::strcmp(parsed.state, "brightness_set") != 0 &&
        std::strcmp(parsed.state, "color_temperature_set") != 0 &&
        std::strcmp(parsed.state, "rgb_set") != 0 &&
        std::strcmp(parsed.state, "white_set") != 0) {
        if (ends_with(parsed.state, "_set")) {
            set_invalid(result, "Unsupported device/group set topic");
        }
        return;
    }

    // Retained *_set сообщения нельзя исполнять: иначе reconnect к брокеру может
    // повторить старую команду управления светом.
    if (retain) {
        result->status = mqtt_device_set_status_t::IgnoredRetained;
        return;
    }

    if (!copy_slice(payload_text, sizeof(payload_text), payload, payload_len)) {
        set_invalid(result, "Set payload is too long");
        return;
    }

    char *value_text = trim_ascii(payload_text);
    if (std::strcmp(parsed.state, "brightness_set") == 0) {
        unsigned level = 0;
        if (!parse_uint_range(value_text, 0, 254, &level)) {
            set_invalid(result, "brightness_set payload must be 0..254");
            return;
        }

        build_brightness_command(parsed, level, &result->command);
        result->status = mqtt_device_set_status_t::Ready;
        return;
    }

    if (std::strcmp(parsed.state, "color_temperature_set") == 0) {
        unsigned kelvin = 0;
        if (!parse_uint_range(value_text, 2700, 6500, &kelvin)) {
            set_invalid(result, "color_temperature_set payload must be Kelvin 2700..6500");
            return;
        }

        build_text_command(parsed, "ct %uK", kelvin, 0xFFFFFFFFU, 0, &result->command);
        result->status = mqtt_device_set_status_t::Ready;
        return;
    }

    if (std::strcmp(parsed.state, "rgb_set") == 0) {
        uint8_t values[3] = {};
        if (!parse_u8_csv(value_text, values, 3)) {
            set_invalid(result, "rgb_set payload must be r,g,b with values 0..255");
            return;
        }

        if (!build_rgb_command(parsed, values, &result->command)) {
            set_invalid(result, "Failed to build rgb_set command");
            return;
        }
        result->status = mqtt_device_set_status_t::Ready;
        return;
    }

    uint8_t values[3] = {};
    if (!parse_u8_csv(value_text, values, 3)) {
        set_invalid(result, "white_set payload must be w,a,f with values 0..255");
        return;
    }

    if (!build_white_command(parsed, values, &result->command)) {
        set_invalid(result, "Failed to build white_set command");
        return;
    }
    result->status = mqtt_device_set_status_t::Ready;
}

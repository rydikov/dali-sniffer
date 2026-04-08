#include "dali_protocol.h"

#include <cctype>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

enum class DaliCommandMode : uint8_t {
    Targeted,
    Raw,
};

const char *dali_command_name(uint8_t command)
{
    switch (command) {
    case 0x00:
        return "OFF";
    case 0x01:
        return "UP";
    case 0x02:
        return "DOWN";
    case 0x03:
        return "STEP_UP";
    case 0x04:
        return "STEP_DOWN";
    case 0x05:
        return "RECALL_MAX_LEVEL";
    case 0x06:
        return "RECALL_MIN_LEVEL";
    case 0x07:
        return "STEP_DOWN_AND_OFF";
    case 0x08:
        return "ON_AND_STEP_UP";
    case 0x09:
        return "ENABLE_DAPC_SEQUENCE";
    case 0x20:
        return "RESET";
    case 0x21:
        return "STORE_ACTUAL_LEVEL";
    case 0x22:
        return "SAVE_PERSISTENT_VARIABLES";
    case 0x23:
        return "SET_OPERATING_MODE";
    case 0x24:
        return "RESET_MEMORY_BANK";
    case 0x25:
        return "IDENTIFY_DEVICE";
    case 0x2A:
        return "STORE_DTR_AS_MAX_LEVEL";
    case 0x2B:
        return "STORE_DTR_AS_MIN_LEVEL";
    case 0x2C:
        return "STORE_DTR_AS_SYSTEM_FAILURE_LEVEL";
    case 0x2D:
        return "STORE_DTR_AS_POWER_ON_LEVEL";
    case 0x2E:
        return "STORE_DTR_AS_FADE_TIME";
    case 0x2F:
        return "STORE_DTR_AS_FADE_RATE";
    case 0x30:
        return "STORE_DTR_AS_EXTENDED_FADE_TIME";
    case 0x80:
        return "STORE_DTR_AS_SHORT_ADDRESS";
    case 0x81:
        return "ENABLE_WRITE_MEMORY";
    case 0x90:
        return "QUERY_STATUS";
    case 0x91:
        return "QUERY_CONTROL_GEAR";
    case 0x92:
        return "QUERY_LAMP_FAILURE";
    case 0x93:
        return "QUERY_LAMP_POWER_ON";
    case 0x94:
        return "QUERY_LIMIT_ERROR";
    case 0x95:
        return "QUERY_RESET_STATE";
    case 0x96:
        return "QUERY_MISSING_SHORT_ADDRESS";
    case 0x97:
        return "QUERY_VERSION";
    case 0x98:
        return "QUERY_CONTENT_DTR";
    case 0x99:
        return "QUERY_DEVICE_TYPE";
    case 0x9A:
        return "QUERY_PHYSICAL_MINIMUM_LEVEL";
    case 0x9B:
        return "QUERY_POWER_FAILURE";
    case 0x9C:
        return "QUERY_CONTENT_DTR1";
    case 0x9D:
        return "QUERY_CONTENT_DTR2";
    case 0x9E:
        return "QUERY_OPERATING_MODE";
    case 0x9F:
        return "QUERY_LIGHT_SOURCE_TYPE";
    case 0xA0:
        return "QUERY_ACTUAL_LEVEL";
    case 0xA1:
        return "QUERY_MAX_LEVEL";
    case 0xA2:
        return "QUERY_MIN_LEVEL";
    case 0xA3:
        return "QUERY_POWER_ON_LEVEL";
    case 0xA4:
        return "QUERY_SYSTEM_FAILURE_LEVEL";
    case 0xA5:
        return "QUERY_FADE_TIME_FADE_RATE";
    case 0xA6:
        return "QUERY_MANUFACTURER_SPECIFIC_MODE";
    case 0xC0:
        return "QUERY_GROUPS_0_7";
    case 0xC1:
        return "QUERY_GROUPS_8_15";
    case 0xC2:
        return "QUERY_RANDOM_ADDRESS_H";
    case 0xC3:
        return "QUERY_RANDOM_ADDRESS_M";
    case 0xC4:
        return "QUERY_RANDOM_ADDRESS_L";
    case 0xC5:
        return "READ_MEMORY_LOCATION";
    case 0xE2:
        return "DT8_ACTIVATE";
    case 0xE7:
        return "DT8_SET_COLOUR_TEMP_TC";
    case 0xEB:
        return "DT8_SET_TEMPORARY_RGB_DIMLEVEL";
    case 0xED:
        return "QUERY_GEAR_TYPE";
    case 0xEE:
        return "QUERY_DIMMING_CURVE";
    case 0xEF:
        return "QUERY_POSSIBLE_OPERATING_MODE";
    case 0xF0:
        return "QUERY_FEATURES";
    case 0xF1:
        return "QUERY_FAILURE_STATUS";
    case 0xF2:
        return "QUERY_SHORT_CIRCUIT";
    case 0xF3:
        return "QUERY_OPEN_CIRCUIT";
    case 0xF4:
        return "QUERY_LOAD_DECREASE";
    case 0xF5:
        return "QUERY_LOAD_INCREASE";
    case 0xF6:
        return "QUERY_CURRENT_PROTECTOR_ACTIVE";
    case 0xF7:
        return "DT8_QUERY_COLOUR_STATUS";
    case 0xF8:
        return "DT8_QUERY_COLOUR_TYPE_FEATURES";
    case 0xF9:
        return "DT8_QUERY_COLOUR_VALUE";
    case 0xFA:
        return "QUERY_REFERENCE_RUNNING";
    case 0xFB:
        return "QUERY_REFERENCE_MEASUREMENT_FAILED";
    case 0xFC:
        return "QUERY_OPERATING_MODE_207";
    case 0xFD:
        return "QUERY_FAST_FADE_TIME";
    case 0xFE:
        return "QUERY_MIN_FAST_FADE_TIME";
    case 0xFF:
        return "QUERY_EXTENDED_VERSION_NUMBER";
    default:
        return nullptr;
    }
}

const char *dali_special_command_name(uint8_t command)
{
    switch (command) {
    case 0xA1:
        return "TERMINATE";
    case 0xA3:
        return "DATA_TRANSFER_REGISTER0";
    case 0xA5:
        return "INITIALISE";
    case 0xA7:
        return "RANDOMISE";
    case 0xA9:
        return "COMPARE";
    case 0xAB:
        return "WITHDRAW";
    case 0xAF:
        return "PING";
    case 0xB1:
        return "SEARCHADDRH";
    case 0xB3:
        return "SEARCHADDRM";
    case 0xB5:
        return "SEARCHADDRL";
    case 0xB7:
        return "PROGRAM_SHORT_ADDRESS";
    case 0xB9:
        return "VERIFY_SHORT_ADDRESS";
    case 0xBB:
        return "QUERY_SHORT_ADDRESS";
    case 0xBD:
        return "PHYSICAL_SELECTION";
    case 0xC1:
        return "ENABLE_DEVICE_TYPE_X";
    case 0xC3:
        return "DATA_TRANSFER_REGISTER1";
    case 0xC5:
        return "DATA_TRANSFER_REGISTER2";
    case 0xC7:
        return "WRITE_MEMORY_LOCATION";
    case 0xC9:
        return "WRITE_MEMORY_LOCATION_NO_REPLY";
    default:
        return nullptr;
    }
}

const char *dali_input_command_name(uint8_t command)
{
    switch (command) {
    case 0x00:
        return "INPUT_INITIALISE";
    case 0x01:
        return "INPUT_RANDOMISE";
    case 0x02:
        return "INPUT_COMPARE";
    case 0x03:
        return "INPUT_WITHDRAW";
    case 0x04:
        return "INPUT_PING";
    case 0x05:
        return "INPUT_RESET";
    case 0x06:
        return "INPUT_TERMINATE";
    case 0x07:
        return "INPUT_PROGRAM_SHORT_ADDR";
    case 0x08:
        return "INPUT_SEARCHADDRH";
    case 0x09:
        return "INPUT_SEARCHADDRM";
    case 0x0A:
        return "INPUT_SEARCHADDRL";
    case 0x0B:
        return "INPUT_QUERY_SHORT_ADDR";
    case 0x10:
        return "INPUT_QUERY_STATUS";
    case 0x3C:
        return "INPUT_READ_MEMORY_LOCATION";
    default:
        return nullptr;
    }
}

bool dali_command_has_index(uint8_t command, const char **name, uint8_t *index)
{
    if (command >= 0x10 && command <= 0x1F) {
        *name = "GO_TO_SCENE";
        *index = command - 0x10;
        return true;
    }

    if (command >= 0x40 && command <= 0x4F) {
        *name = "STORE_DTR_AS_SCENE";
        *index = command - 0x40;
        return true;
    }

    if (command >= 0x50 && command <= 0x5F) {
        *name = "REMOVE_FROM_SCENE";
        *index = command - 0x50;
        return true;
    }

    if (command >= 0x60 && command <= 0x6F) {
        *name = "ADD_TO_GROUP";
        *index = command - 0x60;
        return true;
    }

    if (command >= 0x70 && command <= 0x7F) {
        *name = "REMOVE_FROM_GROUP";
        *index = command - 0x70;
        return true;
    }

    if (command >= 0xB0 && command <= 0xBF) {
        *name = "QUERY_SCENE_LEVEL";
        *index = command - 0xB0;
        return true;
    }

    return false;
}

void format_dali_target(uint8_t address_byte, const char **target_type, uint8_t *target_index)
{
    if ((address_byte & 0x80) == 0) {
        *target_type = "short";
        *target_index = (address_byte >> 1) & 0x3F;
        return;
    }

    if ((address_byte & 0xE0) == 0x80) {
        *target_type = "group";
        *target_index = (address_byte >> 1) & 0x0F;
        return;
    }

    if (address_byte == 0xFE || address_byte == 0xFF) {
        *target_type = "broadcast";
        *target_index = 0;
        return;
    }

    *target_type = nullptr;
    *target_index = 0;
}

char *trim_ascii(char *text)
{
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) {
        ++text;
    }

    size_t length = std::strlen(text);
    while (length > 0 && std::isspace(static_cast<unsigned char>(text[length - 1])) != 0) {
        text[length - 1] = '\0';
        --length;
    }

    return text;
}

void lowercase_ascii(char *text)
{
    while (*text != '\0') {
        *text = static_cast<char>(std::tolower(static_cast<unsigned char>(*text)));
        ++text;
    }
}

bool parse_uint8_range(const char *text, uint8_t min_value, uint8_t max_value, uint8_t *value)
{
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);

    if (text == end || *trim_ascii(end) != '\0' || parsed < min_value || parsed > max_value) {
        return false;
    }

    *value = static_cast<uint8_t>(parsed);
    return true;
}

bool parse_uint16_range(const char *text, uint16_t min_value, uint16_t max_value, uint16_t *value)
{
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);

    if (text == end || *trim_ascii(end) != '\0' || parsed < min_value || parsed > max_value) {
        return false;
    }

    *value = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_prefixed_index(const char *text, const char *prefix, uint8_t max_value, uint8_t *value)
{
    const size_t prefix_length = std::strlen(prefix);
    if (std::strncmp(text, prefix, prefix_length) != 0) {
        return false;
    }

    return parse_uint8_range(trim_ascii(const_cast<char *>(text + prefix_length)), 0, max_value, value);
}

uint8_t dali_address_byte(const dali_target_t &target, bool is_command)
{
    switch (target.kind) {
    case DaliTargetKind::Lamp:
        return static_cast<uint8_t>((target.index << 1) | (is_command ? 1 : 0));
    case DaliTargetKind::Group:
        return static_cast<uint8_t>(0x80 | (target.index << 1) | (is_command ? 1 : 0));
    case DaliTargetKind::Broadcast:
        return is_command ? 0xFF : 0xFE;
    }

    return 0xFF;
}

uint8_t dali_percent_to_level(uint8_t percent)
{
    if (percent == 0) {
        return 0;
    }

    if (percent >= 100) {
        return 254;
    }

    return static_cast<uint8_t>((percent * 254 + 50) / 100);
}

void set_single_frame_plan(dali_tx_plan_t *plan, const dali_target_t &target, bool is_command, uint8_t data_byte)
{
    plan->frame_count = 1;
    plan->frames[0].bit_length = 16;
    plan->frames[0].data[0] = dali_address_byte(target, is_command);
    plan->frames[0].data[1] = data_byte;
    plan->frames[0].data[2] = 0;
}

bool append_forward_frame(dali_tx_plan_t *plan, const uint8_t *data, uint8_t bit_length)
{
    if (plan->frame_count >= kMaxDaliTxFrames) {
        return false;
    }

    if (bit_length != 16 && bit_length != 24) {
        return false;
    }

    const uint8_t byte_length = static_cast<uint8_t>(bit_length / 8);
    plan->frames[plan->frame_count].bit_length = bit_length;
    plan->frames[plan->frame_count].data[0] = 0;
    plan->frames[plan->frame_count].data[1] = 0;
    plan->frames[plan->frame_count].data[2] = 0;

    for (uint8_t i = 0; i < byte_length; ++i) {
        plan->frames[plan->frame_count].data[i] = data[i];
    }

    ++plan->frame_count;
    return true;
}

bool append_frame(dali_tx_plan_t *plan, uint8_t address_byte, uint8_t data_byte)
{
    const uint8_t data[2] = {address_byte, data_byte};
    return append_forward_frame(plan, data, 16);
}

bool append_special_frame(dali_tx_plan_t *plan, uint8_t special_command, uint8_t value)
{
    return append_frame(plan, special_command, value);
}

bool append_command_frame(dali_tx_plan_t *plan, const dali_target_t &target, uint8_t command)
{
    return append_frame(plan, dali_address_byte(target, true), command);
}

bool append_dt8_enable(dali_tx_plan_t *plan)
{
    return append_special_frame(plan, 0xC1, 0x08);
}

bool build_dt8_color_temp_plan(dali_tx_plan_t *plan, const dali_target_t &target, uint16_t kelvin)
{
    const uint16_t mired = static_cast<uint16_t>(1000000UL / kelvin);

    plan->frame_count = 0;
    return append_special_frame(plan, 0xC3, static_cast<uint8_t>((mired >> 8) & 0xFF)) &&
           append_special_frame(plan, 0xA3, static_cast<uint8_t>(mired & 0xFF)) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xE7) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xE2);
}

bool build_dt8_rgb_plan(dali_tx_plan_t *plan, const dali_target_t &target, uint8_t red, uint8_t green, uint8_t blue)
{
    plan->frame_count = 0;
    return append_special_frame(plan, 0xC3, 0x01) &&
           append_special_frame(plan, 0xA3, red) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xEB) &&
           append_special_frame(plan, 0xC3, 0x02) &&
           append_special_frame(plan, 0xA3, green) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xEB) &&
           append_special_frame(plan, 0xC3, 0x04) &&
           append_special_frame(plan, 0xA3, blue) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xEB) &&
           append_dt8_enable(plan) &&
           append_command_frame(plan, target, 0xE2);
}

bool parse_dali_target(char *text, dali_target_t *target)
{
    if (std::strcmp(text, "all") == 0 || std::strcmp(text, "broadcast") == 0) {
        target->kind = DaliTargetKind::Broadcast;
        target->index = 0;
        return true;
    }

    uint8_t value = 0;
    if (parse_prefixed_index(text, "lamp ", 63, &value)) {
        target->kind = DaliTargetKind::Lamp;
        target->index = value;
        return true;
    }

    if (parse_prefixed_index(text, "group ", 15, &value)) {
        target->kind = DaliTargetKind::Group;
        target->index = value;
        return true;
    }

    return false;
}

bool parse_hex_byte_token(const char *text, uint8_t *value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }

    if (std::strlen(text) > 2 && text[0] == '0' && text[1] == 'x') {
        text += 2;
    }

    if (*text == '\0') {
        return false;
    }

    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 16);

    if (text == end || *trim_ascii(end) != '\0' || parsed < 0 || parsed > 0xFF) {
        return false;
    }

    *value = static_cast<uint8_t>(parsed);
    return true;
}

bool parse_raw_action(char *text, dali_tx_plan_t *plan, char *error_text, size_t error_text_size)
{
    constexpr size_t kMaxRawBytes = 3;
    uint8_t raw_bytes[kMaxRawBytes] = {};
    size_t raw_byte_count = 0;
    char *cursor = trim_ascii(text);

    if (*cursor == '\0') {
        std::snprintf(error_text, error_text_size, "Use raw -> <byte1> <byte2> [byte3]");
        return false;
    }

    while (*cursor != '\0') {
        if (raw_byte_count >= kMaxRawBytes) {
            std::snprintf(error_text, error_text_size, "Raw command supports only 2 or 3 bytes");
            return false;
        }

        char *token_end = cursor;
        while (*token_end != '\0' && std::isspace(static_cast<unsigned char>(*token_end)) == 0) {
            ++token_end;
        }

        const char separator = *token_end;
        *token_end = '\0';
        if (!parse_hex_byte_token(cursor, &raw_bytes[raw_byte_count])) {
            std::snprintf(error_text, error_text_size, "Invalid raw byte: %s", cursor);
            return false;
        }

        ++raw_byte_count;
        if (separator == '\0') {
            break;
        }

        cursor = trim_ascii(token_end + 1);
    }

    if (raw_byte_count != 2 && raw_byte_count != 3) {
        std::snprintf(error_text, error_text_size, "Raw command requires exactly 2 or 3 bytes");
        return false;
    }

    plan->frame_count = 0;
    return append_forward_frame(plan, raw_bytes, raw_byte_count == 2 ? 16 : 24);
}

bool parse_dali_action(char *text,
                       const dali_target_t &target,
                       dali_tx_plan_t *plan,
                       char *error_text,
                       size_t error_text_size)
{
    uint8_t value = 0;
    const size_t length = std::strlen(text);
    uint16_t kelvin = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    if (length > 1 && text[length - 1] == '%') {
        text[length - 1] = '\0';
        if (parse_uint8_range(trim_ascii(text), 0, 100, &value)) {
            set_single_frame_plan(plan, target, false, dali_percent_to_level(value));
            return true;
        }
        return false;
    }

    if (std::strcmp(text, "off") == 0) {
        set_single_frame_plan(plan, target, true, 0x00);
        return true;
    }
    if (std::strcmp(text, "on") == 0) {
        set_single_frame_plan(plan, target, false, 254);
        return true;
    }
    if (std::strcmp(text, "max") == 0) {
        set_single_frame_plan(plan, target, true, 0x05);
        return true;
    }
    if (std::strcmp(text, "min") == 0) {
        set_single_frame_plan(plan, target, true, 0x06);
        return true;
    }
    if (std::strcmp(text, "up") == 0) {
        set_single_frame_plan(plan, target, true, 0x01);
        return true;
    }
    if (std::strcmp(text, "down") == 0) {
        set_single_frame_plan(plan, target, true, 0x02);
        return true;
    }
    if (std::strcmp(text, "step up") == 0) {
        set_single_frame_plan(plan, target, true, 0x03);
        return true;
    }
    if (std::strcmp(text, "step down") == 0) {
        set_single_frame_plan(plan, target, true, 0x04);
        return true;
    }
    if (std::strcmp(text, "step up on") == 0) {
        set_single_frame_plan(plan, target, true, 0x08);
        return true;
    }
    if (std::strcmp(text, "step down off") == 0) {
        set_single_frame_plan(plan, target, true, 0x07);
        return true;
    }
    if (parse_prefixed_index(text, "scene ", 15, &value)) {
        set_single_frame_plan(plan, target, true, static_cast<uint8_t>(0x10 + value));
        return true;
    }

    if (std::strcmp(text, "query status") == 0) {
        set_single_frame_plan(plan, target, true, 0x90);
        return true;
    }
    if (std::strcmp(text, "query present") == 0) {
        set_single_frame_plan(plan, target, true, 0x91);
        return true;
    }
    if (std::strcmp(text, "query failure") == 0) {
        set_single_frame_plan(plan, target, true, 0x92);
        return true;
    }
    if (std::strcmp(text, "query lamp on") == 0) {
        set_single_frame_plan(plan, target, true, 0x93);
        return true;
    }
    if (std::strcmp(text, "query level") == 0) {
        set_single_frame_plan(plan, target, true, 0xA0);
        return true;
    }
    if (std::strcmp(text, "query max") == 0) {
        set_single_frame_plan(plan, target, true, 0xA1);
        return true;
    }
    if (std::strcmp(text, "query min") == 0) {
        set_single_frame_plan(plan, target, true, 0xA2);
        return true;
    }
    if (std::strcmp(text, "query power on") == 0) {
        set_single_frame_plan(plan, target, true, 0xA3);
        return true;
    }
    if (std::strcmp(text, "query version") == 0) {
        set_single_frame_plan(plan, target, true, 0x97);
        return true;
    }
    if (std::strcmp(text, "query device type") == 0) {
        set_single_frame_plan(plan, target, true, 0x99);
        return true;
    }
    if (std::strcmp(text, "query groups") == 0) {
        plan->frame_count = 2;
        plan->frames[0].bit_length = 16;
        plan->frames[0].data[0] = dali_address_byte(target, true);
        plan->frames[0].data[1] = 0xC0;
        plan->frames[0].data[2] = 0;
        plan->frames[1].bit_length = 16;
        plan->frames[1].data[0] = dali_address_byte(target, true);
        plan->frames[1].data[1] = 0xC1;
        plan->frames[1].data[2] = 0;
        return true;
    }
    if (parse_prefixed_index(text, "query scene ", 15, &value)) {
        set_single_frame_plan(plan, target, true, static_cast<uint8_t>(0xB0 + value));
        return true;
    }
    if (parse_prefixed_index(text, "add to group ", 15, &value)) {
        set_single_frame_plan(plan, target, true, static_cast<uint8_t>(0x60 + value));
        return true;
    }
    if (parse_prefixed_index(text, "remove from group ", 15, &value)) {
        set_single_frame_plan(plan, target, true, static_cast<uint8_t>(0x70 + value));
        return true;
    }
    if (parse_prefixed_index(text, "remove scene ", 15, &value)) {
        set_single_frame_plan(plan, target, true, static_cast<uint8_t>(0x50 + value));
        return true;
    }

    if (std::strncmp(text, "ct ", 3) == 0) {
        char *kelvin_text = trim_ascii(text + 3);
        const size_t kelvin_length = std::strlen(kelvin_text);

        if (target.kind == DaliTargetKind::Broadcast) {
            std::snprintf(error_text, error_text_size, "DT8 ct supports only lamp/group targets");
            return false;
        }

        if (kelvin_length < 2 || kelvin_text[kelvin_length - 1] != 'k') {
            std::snprintf(error_text, error_text_size, "Use ct <kelvin>K, for example: ct 4000K");
            return false;
        }

        kelvin_text[kelvin_length - 1] = '\0';
        if (!parse_uint16_range(trim_ascii(kelvin_text), 1000, 65535, &kelvin)) {
            std::snprintf(error_text, error_text_size, "Invalid color temperature: %sK", trim_ascii(kelvin_text));
            return false;
        }

        if (!build_dt8_color_temp_plan(plan, target, kelvin)) {
            std::snprintf(error_text, error_text_size, "Failed to build DT8 ct command");
            return false;
        }
        return true;
    }

    if (std::strncmp(text, "rgb ", 4) == 0) {
        char *rgb_text = trim_ascii(text + 4);
        char *second = rgb_text;
        char *third = nullptr;

        if (target.kind == DaliTargetKind::Broadcast) {
            std::snprintf(error_text, error_text_size, "DT8 rgb supports only lamp/group targets");
            return false;
        }

        while (*second != '\0' && std::isspace(static_cast<unsigned char>(*second)) == 0) {
            ++second;
        }
        if (*second == '\0') {
            std::snprintf(error_text, error_text_size, "Use rgb <r> <g> <b>, for example: rgb 255 120 0");
            return false;
        }
        *second++ = '\0';
        second = trim_ascii(second);
        third = second;
        while (*third != '\0' && std::isspace(static_cast<unsigned char>(*third)) == 0) {
            ++third;
        }
        if (*third == '\0') {
            std::snprintf(error_text, error_text_size, "Use rgb <r> <g> <b>, for example: rgb 255 120 0");
            return false;
        }
        *third++ = '\0';
        third = trim_ascii(third);

        if (!parse_uint8_range(rgb_text, 0, 255, &red) ||
            !parse_uint8_range(second, 0, 255, &green) ||
            !parse_uint8_range(third, 0, 255, &blue)) {
            std::snprintf(error_text, error_text_size, "RGB values must be in range 0..255");
            return false;
        }

        if (!build_dt8_rgb_plan(plan, target, red, green, blue)) {
            std::snprintf(error_text, error_text_size, "Failed to build DT8 rgb command");
            return false;
        }
        return true;
    }

    return false;
}

void set_address_metadata(dali_frame_description_t *description,
                          const char *kind,
                          bool has_value,
                          int value,
                          const char *label)
{
    std::snprintf(description->address_kind, sizeof(description->address_kind), "%s", kind);
    description->has_address_value = has_value;
    description->address_value = value;
    std::snprintf(description->address_label, sizeof(description->address_label), "%s", label);
}

void set_target_metadata(dali_frame_description_t *description, const char *target_type, uint8_t target_index)
{
    if (target_type == nullptr) {
        set_address_metadata(description, "unknown", false, 0, "unknown");
        return;
    }

    if (std::strcmp(target_type, "broadcast") == 0) {
        set_address_metadata(description, "broadcast", true, 0, "broadcast");
        return;
    }

    char label[24];
    std::snprintf(label, sizeof(label), "%s[%u]", target_type, target_index);
    set_address_metadata(description, target_type, true, target_index, label);
}

void set_command_name(dali_frame_description_t *description, const char *command_name)
{
    if (command_name == nullptr) {
        description->has_command_name = false;
        description->command_name[0] = '\0';
        return;
    }

    description->has_command_name = true;
    std::snprintf(description->command_name, sizeof(description->command_name), "%s", command_name);
}

void build_raw_hex(const dali_frame_event_t &frame, char *buffer, size_t buffer_size)
{
    unsigned width = 8;

    if (frame.is_backward_frame) {
        width = 2;
    } else if (frame.length == 16) {
        width = 4;
    } else if (frame.length == 24) {
        width = 6;
    }

    std::snprintf(buffer, buffer_size, "%0*" PRIX32, width, frame.data);
}

}  // namespace

bool dali_build_tx_plan(const char *command_text, dali_tx_plan_t *plan, char *error_text, size_t error_text_size)
{
    char command_copy[128];
    std::snprintf(command_copy, sizeof(command_copy), "%s", command_text != nullptr ? command_text : "");
    lowercase_ascii(command_copy);
    if (error_text_size > 0) {
        error_text[0] = '\0';
    }

    char *separator = std::strstr(command_copy, "->");
    if (separator == nullptr) {
        std::snprintf(error_text, error_text_size, "Use format: <target> -> <action>");
        return false;
    }

    *separator = '\0';
    char *target_text = trim_ascii(command_copy);
    char *action_text = trim_ascii(separator + 2);
    dali_target_t target = {};
    DaliCommandMode mode = DaliCommandMode::Targeted;

    if (std::strcmp(target_text, "raw") == 0) {
        mode = DaliCommandMode::Raw;
    }

    if (mode == DaliCommandMode::Raw) {
        return parse_raw_action(action_text, plan, error_text, error_text_size);
    }

    if (!parse_dali_target(target_text, &target)) {
        std::snprintf(error_text, error_text_size, "Unknown target: %s", target_text);
        return false;
    }

    if (!parse_dali_action(action_text, target, plan, error_text, error_text_size)) {
        if (error_text[0] == '\0') {
            std::snprintf(error_text, error_text_size, "Unknown action: %s", action_text);
        }
        return false;
    }

    return true;
}

esp_err_t dali_execute_command_text(const char *command_text, dali_command_exec_result_t *result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    result->accepted = false;
    result->sent = false;
    result->frame_count = 0;
    result->feedback[0] = '\0';

    dali_tx_plan_t plan = {};
    if (!dali_build_tx_plan(command_text, &plan, result->feedback, sizeof(result->feedback))) {
        if (result->feedback[0] == '\0') {
            std::snprintf(result->feedback, sizeof(result->feedback), "Invalid command");
        }
        return ESP_OK;
    }

    result->accepted = true;
    result->frame_count = plan.frame_count;
    const esp_err_t send_err = dali_sniffer_send_frames(plan.frames, plan.frame_count);
    if (send_err == ESP_OK) {
        result->sent = true;
        std::snprintf(result->feedback, sizeof(result->feedback), "Sent: %s", command_text);
    } else {
        std::snprintf(result->feedback,
                      sizeof(result->feedback),
                      "Failed to send DALI command \"%s\": %s",
                      command_text,
                      esp_err_to_name(send_err));
    }

    return send_err == ESP_OK ? ESP_OK : send_err;
}

void dali_describe_frame(const dali_frame_event_t &frame, dali_frame_description_t *description)
{
    if (description == nullptr) {
        return;
    }

    std::memset(description, 0, sizeof(*description));
    description->is_backward_frame = frame.is_backward_frame;
    description->bit_length = frame.length;
    description->raw_value = frame.data;
    build_raw_hex(frame, description->raw_hex, sizeof(description->raw_hex));
    set_address_metadata(description, "unknown", false, 0, "unknown");

    if (frame.is_backward_frame) {
        set_address_metadata(description, "reply", false, 0, "reply");
        std::snprintf(description->text, sizeof(description->text), "DALI reply: 0x%02" PRIX32, frame.data & 0xFF);
        return;
    }

    if (frame.length == 24) {
        const uint8_t address = (frame.data >> 16) & 0xFF;
        const uint8_t opcode = (frame.data >> 8) & 0xFF;
        const uint8_t parameter = frame.data & 0xFF;
        const char *target_type = nullptr;
        uint8_t target_index = 0;
        format_dali_target(address, &target_type, &target_index);
        set_target_metadata(description, target_type, target_index);
        description->has_opcode = true;
        description->opcode = opcode;
        description->has_arg = true;
        description->arg = parameter;

        if (const char *name = dali_input_command_name(opcode); name != nullptr) {
            set_command_name(description, name);
            if (target_type == nullptr) {
                std::snprintf(description->text,
                              sizeof(description->text),
                              "DALI input cmd addr=0x%02X: %s arg=0x%02X raw=0x%06" PRIX32,
                              address,
                              name,
                              parameter,
                              frame.data & 0xFFFFFF);
                return;
            }

            if (std::strcmp(target_type, "broadcast") == 0) {
                std::snprintf(description->text,
                              sizeof(description->text),
                              "DALI input cmd broadcast: %s arg=0x%02X raw=0x%06" PRIX32,
                              name,
                              parameter,
                              frame.data & 0xFFFFFF);
                return;
            }

            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI input cmd %s[%u]: %s arg=0x%02X raw=0x%06" PRIX32,
                          target_type,
                          target_index,
                          name,
                          parameter,
                          frame.data & 0xFFFFFF);
            return;
        }

        std::snprintf(description->text,
                      sizeof(description->text),
                      "DALI 24-bit frame: addr=0x%02X opcode=0x%02X param=0x%02X raw=0x%06" PRIX32,
                      address,
                      opcode,
                      parameter,
                      frame.data & 0xFFFFFF);
        return;
    }

    if (frame.length != 16) {
        std::snprintf(description->text,
                      sizeof(description->text),
                      "DALI frame (%u bit): 0x%08" PRIX32,
                      frame.length,
                      frame.data);
        return;
    }

    const uint8_t address_byte = (frame.data >> 8) & 0xFF;
    const uint8_t command_byte = frame.data & 0xFF;

    const char *special_name = dali_special_command_name(address_byte);
    if (special_name != nullptr) {
        set_address_metadata(description, "special", false, 0, "special");
        set_command_name(description, special_name);
        description->has_arg = true;
        description->arg = command_byte;
        std::snprintf(description->text,
                      sizeof(description->text),
                      "DALI special: %s arg=0x%02X raw=0x%04" PRIX32,
                      special_name,
                      command_byte,
                      frame.data & 0xFFFF);
        return;
    }

    if ((address_byte & 0x01) == 0) {
        description->has_level = true;
        description->level = command_byte;
        set_command_name(description, "DAPC");

        if ((address_byte & 0x80) == 0) {
            set_address_metadata(description,
                                 "short",
                                 true,
                                 (address_byte >> 1) & 0x3F,
                                 "");
            std::snprintf(description->address_label,
                          sizeof(description->address_label),
                          "short[%u]",
                          (address_byte >> 1) & 0x3F);
            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI DAPC short[%u] level=%u raw=0x%04" PRIX32,
                          (address_byte >> 1) & 0x3F,
                          command_byte,
                          frame.data & 0xFFFF);
            return;
        }

        if ((address_byte & 0xE0) == 0x80) {
            set_address_metadata(description,
                                 "group",
                                 true,
                                 (address_byte >> 1) & 0x0F,
                                 "");
            std::snprintf(description->address_label,
                          sizeof(description->address_label),
                          "group[%u]",
                          (address_byte >> 1) & 0x0F);
            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI DAPC group[%u] level=%u raw=0x%04" PRIX32,
                          (address_byte >> 1) & 0x0F,
                          command_byte,
                          frame.data & 0xFFFF);
            return;
        }

        if (address_byte == 0xFE) {
            set_address_metadata(description, "broadcast", true, 0, "broadcast");
            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI DAPC broadcast level=%u raw=0x%04" PRIX32,
                          command_byte,
                          frame.data & 0xFFFF);
            return;
        }
    }

    const char *name = dali_command_name(command_byte);
    const char *indexed_name = nullptr;
    uint8_t indexed_value = 0;
    const bool has_index = dali_command_has_index(command_byte, &indexed_name, &indexed_value);
    const char *target_type = nullptr;
    uint8_t target_index = 0;
    format_dali_target(address_byte, &target_type, &target_index);
    set_target_metadata(description, target_type, target_index);

    if (has_index) {
        set_command_name(description, indexed_name);
        description->has_command_index = true;
        description->command_index = indexed_value;
    } else {
        set_command_name(description, name);
    }

    if (target_type != nullptr) {
        if (has_index) {
            if (std::strcmp(target_type, "broadcast") == 0) {
                std::snprintf(description->text,
                              sizeof(description->text),
                              "DALI command broadcast: %s[%u] raw=0x%04" PRIX32,
                              indexed_name,
                              indexed_value,
                              frame.data & 0xFFFF);
                return;
            }

            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI command %s[%u]: %s[%u] raw=0x%04" PRIX32,
                          target_type,
                          target_index,
                          indexed_name,
                          indexed_value,
                          frame.data & 0xFFFF);
            return;
        }

        if (std::strcmp(target_type, "broadcast") == 0) {
            std::snprintf(description->text,
                          sizeof(description->text),
                          "DALI command broadcast: %s raw=0x%04" PRIX32,
                          name != nullptr ? name : "UNKNOWN",
                          frame.data & 0xFFFF);
            return;
        }

        std::snprintf(description->text,
                      sizeof(description->text),
                      "DALI command %s[%u]: %s raw=0x%04" PRIX32,
                      target_type,
                      target_index,
                      name != nullptr ? name : "UNKNOWN",
                      frame.data & 0xFFFF);
        return;
    }

    std::snprintf(description->text,
                  sizeof(description->text),
                  "DALI frame addr=0x%02X cmd=0x%02X raw=0x%04" PRIX32,
                  address_byte,
                  command_byte,
                  frame.data & 0xFFFF);
}

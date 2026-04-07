#include "web_server.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>

extern "C" {
#include "cJSON.h"
}

#include "dali_sniffer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *kTag = "web_server";

// Веб-ресурсы подключаются в прошивку на этапе линковки как бинарные массивы.
extern const char web_index_html_start[] asm("_binary_index_html_start");
extern const char web_index_html_end[] asm("_binary_index_html_end");
extern const char web_app_css_start[] asm("_binary_app_css_start");
extern const char web_app_css_end[] asm("_binary_app_css_end");
extern const char web_app_js_start[] asm("_binary_app_js_start");
extern const char web_app_js_end[] asm("_binary_app_js_end");

httpd_handle_t s_server = nullptr;

typedef struct {
    const char *uri;
    const char *content_path;
    const char *start;
    const char *end;
} embedded_asset_t;

enum class DaliTargetKind : uint8_t {
    Lamp,
    Group,
    Broadcast,
};

struct dali_target_t {
    DaliTargetKind kind;
    uint8_t index;
};

enum class DaliCommandMode : uint8_t {
    Targeted,
    Raw,
};

constexpr size_t kMaxDaliTxFrames = 16;

struct dali_tx_plan_t {
    size_t frame_count;
    dali_tx_frame_t frames[kMaxDaliTxFrames];
};

// Таблица статических файлов, которые отдаются напрямую из памяти устройства.
const embedded_asset_t s_assets[] = {
    {
        .uri = "/",
        .content_path = "/index.html",
        .start = web_index_html_start,
        .end = web_index_html_end,
    },
    {
        .uri = "/assets/app.css",
        .content_path = "/assets/app.css",
        .start = web_app_css_start,
        .end = web_app_css_end,
    },
    {
        .uri = "/assets/app.js",
        .content_path = "/assets/app.js",
        .start = web_app_js_start,
        .end = web_app_js_end,
    },
};

const char *content_type_for_path(const char *path)
{
    const char *extension = std::strrchr(path, '.');

    // Если расширение отсутствует, отдаем содержимое как обычный текст.
    if (extension == nullptr) {
        return "text/plain; charset=utf-8";
    }

    if (std::strcmp(extension, ".html") == 0) {
        return "text/html; charset=utf-8";
    }

    if (std::strcmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }

    if (std::strcmp(extension, ".js") == 0) {
        return "text/javascript; charset=utf-8";
    }

    if (std::strcmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }

    if (std::strcmp(extension, ".png") == 0) {
        return "image/png";
    }

    if (std::strcmp(extension, ".woff2") == 0) {
        return "font/woff2";
    }

    return "application/octet-stream";
}

char *build_json_string_message(const char *type, const char *value)
{
    char *payload = nullptr;
    cJSON *root = cJSON_CreateObject();

    if (root == nullptr) {
        return nullptr;
    }

    if (cJSON_AddStringToObject(root, "type", type) == nullptr ||
        cJSON_AddStringToObject(root, "value", value) == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

char *build_command_ack_json(const char *command, bool accepted)
{
    char *payload = nullptr;
    cJSON *root = cJSON_CreateObject();

    if (root == nullptr) {
        return nullptr;
    }

    if (cJSON_AddStringToObject(root, "type", "command_ack") == nullptr ||
        cJSON_AddStringToObject(root, "command", command) == nullptr ||
        cJSON_AddBoolToObject(root, "accepted", accepted) == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

esp_err_t send_embedded_file(httpd_req_t *req, const char *path, const char *start, const char *end)
{
    // У бинарных символов, сгенерированных линкером, в конце обычно есть завершающий ноль,
    // поэтому при отправке вычитаем один байт из диапазона.
    const ptrdiff_t content_length = end - start - 1;

    httpd_resp_set_type(req, content_type_for_path(path));
    return httpd_resp_send(req, start, content_length > 0 ? content_length : 0);
}

const embedded_asset_t *find_asset(const char *uri)
{
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); ++i) {
        if (std::strcmp(uri, s_assets[i].uri) == 0) {
            return &s_assets[i];
        }
    }

    return nullptr;
}

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

bool build_dali_tx_plan(const char *command_text, dali_tx_plan_t *plan, char *error_text, size_t error_text_size)
{
    char command_copy[128];
    std::snprintf(command_copy, sizeof(command_copy), "%s", command_text);
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
        if (!parse_raw_action(action_text, plan, error_text, error_text_size)) {
            return false;
        }
        return true;
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

esp_err_t ws_send_json(httpd_handle_t server, int fd, const char *json)
{
    // Для отправки из фоновой задачи используем API, которое само маршалит вызов в контекст httpd.
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = reinterpret_cast<uint8_t *>(const_cast<char *>(json)),
        .len = std::strlen(json),
    };

    return httpd_ws_send_data(server, fd, &frame);
}

esp_err_t ws_send_message(httpd_req_t *req, const char *text)
{
    char *payload = build_json_string_message("message", text);
    if (payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    httpd_ws_frame_t response = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = reinterpret_cast<uint8_t *>(payload),
        .len = std::strlen(payload),
    };

    const esp_err_t err = httpd_ws_send_frame(req, &response);
    cJSON_free(payload);
    return err;
}

void build_dali_message_text(const dali_frame_event_t &frame, char *buffer, size_t buffer_size)
{
    if (frame.is_backward_frame) {
        std::snprintf(buffer, buffer_size, "DALI reply: 0x%02" PRIX32, frame.data & 0xFF);
        return;
    }

    if (frame.length == 24) {
        const uint8_t address = (frame.data >> 16) & 0xFF;
        const uint8_t opcode = (frame.data >> 8) & 0xFF;
        const uint8_t parameter = frame.data & 0xFF;
        const char *target_type = nullptr;
        uint8_t target_index = 0;
        format_dali_target(address, &target_type, &target_index);

        if (const char *name = dali_input_command_name(opcode); name != nullptr) {
            if (target_type == nullptr) {
                std::snprintf(buffer,
                              buffer_size,
                              "DALI input cmd addr=0x%02X: %s arg=0x%02X raw=0x%06" PRIX32,
                              address,
                              name,
                              parameter,
                              frame.data & 0xFFFFFF);
                return;
            }

            if (std::strcmp(target_type, "broadcast") == 0) {
                std::snprintf(buffer,
                              buffer_size,
                              "DALI input cmd broadcast: %s arg=0x%02X raw=0x%06" PRIX32,
                              name,
                              parameter,
                              frame.data & 0xFFFFFF);
                return;
            }

            std::snprintf(buffer,
                          buffer_size,
                          "DALI input cmd %s[%u]: %s arg=0x%02X raw=0x%06" PRIX32,
                          target_type,
                          target_index,
                          name,
                          parameter,
                          frame.data & 0xFFFFFF);
            return;
        }

        std::snprintf(buffer,
                      buffer_size,
                      "DALI 24-bit frame: addr=0x%02X opcode=0x%02X param=0x%02X raw=0x%06" PRIX32,
                      address,
                      opcode,
                      parameter,
                      frame.data & 0xFFFFFF);
        return;
    }

    if (frame.length != 16) {
        std::snprintf(buffer, buffer_size, "DALI frame (%u bit): 0x%08" PRIX32, frame.length, frame.data);
        return;
    }

    const uint8_t address_byte = (frame.data >> 8) & 0xFF;
    const uint8_t command_byte = frame.data & 0xFF;

    const char *special_name = dali_special_command_name(address_byte);
    if (special_name != nullptr) {
        std::snprintf(buffer,
                      buffer_size,
                      "DALI special: %s arg=0x%02X raw=0x%04" PRIX32,
                      special_name,
                      command_byte,
                      frame.data & 0xFFFF);
        return;
    }

    if ((address_byte & 0x01) == 0) {
        if ((address_byte & 0x80) == 0) {
            std::snprintf(buffer,
                          buffer_size,
                          "DALI DAPC short[%u] level=%u raw=0x%04" PRIX32,
                          (address_byte >> 1) & 0x3F,
                          command_byte,
                          frame.data & 0xFFFF);
            return;
        }

        if ((address_byte & 0xE0) == 0x80) {
            std::snprintf(buffer,
                          buffer_size,
                          "DALI DAPC group[%u] level=%u raw=0x%04" PRIX32,
                          (address_byte >> 1) & 0x0F,
                          command_byte,
                          frame.data & 0xFFFF);
            return;
        }

        if (address_byte == 0xFE) {
            std::snprintf(buffer,
                          buffer_size,
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

    if (target_type != nullptr) {
        if (has_index) {
            if (std::strcmp(target_type, "broadcast") == 0) {
                std::snprintf(buffer,
                              buffer_size,
                              "DALI command broadcast: %s[%u] raw=0x%04" PRIX32,
                              indexed_name,
                              indexed_value,
                              frame.data & 0xFFFF);
                return;
            }

            std::snprintf(buffer,
                          buffer_size,
                          "DALI command %s[%u]: %s[%u] raw=0x%04" PRIX32,
                          target_type,
                          target_index,
                          indexed_name,
                          indexed_value,
                          frame.data & 0xFFFF);
            return;
        }

        if (std::strcmp(target_type, "broadcast") == 0) {
            std::snprintf(buffer,
                          buffer_size,
                          "DALI command broadcast: %s raw=0x%04" PRIX32,
                          name != nullptr ? name : "UNKNOWN",
                          frame.data & 0xFFFF);
            return;
        }

        std::snprintf(buffer,
                      buffer_size,
                      "DALI command %s[%u]: %s raw=0x%04" PRIX32,
                      target_type,
                      target_index,
                      name != nullptr ? name : "UNKNOWN",
                      frame.data & 0xFFFF);
        return;
    }

    std::snprintf(buffer,
                  buffer_size,
                  "DALI frame addr=0x%02X cmd=0x%02X raw=0x%04" PRIX32,
                  address_byte,
                  command_byte,
                  frame.data & 0xFFFF);
}

void broadcast_message(const dali_frame_event_t &frame)
{
    char message[96];
    char *payload;
    int client_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;

    if (s_server == nullptr) {
        return;
    }

    build_dali_message_text(frame, message, sizeof(message));
    payload = build_json_string_message("message", message);
    if (payload == nullptr) {
        ESP_LOGW(kTag, "Failed to build broadcast JSON payload");
        return;
    }

    if (httpd_get_client_list(s_server, &clients, client_fds) != ESP_OK) {
        cJSON_free(payload);
        return;
    }

    // Проходим только по активным WebSocket-клиентам и отправляем им сообщение.
    for (size_t i = 0; i < clients; ++i) {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            ESP_LOGI(kTag, "Sending message to fd %d: %s", client_fds[i], message);
            const esp_err_t err = ws_send_json(s_server, client_fds[i], payload);

            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Failed to send message to fd %d: %s", client_fds[i], esp_err_to_name(err));
            }
        }
    }

    cJSON_free(payload);
}

void websocket_event_task(void *arg)
{
    (void)arg;
    const QueueHandle_t queue = dali_sniffer_get_event_queue();
    dali_frame_event_t frame = {};

    if (queue == nullptr) {
        ESP_LOGW(kTag, "DALI event queue is unavailable");
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        if (xQueueReceive(queue, &frame, portMAX_DELAY) == pdTRUE) {
            broadcast_message(frame);
        }
    }
}

esp_err_t http_get_handler(httpd_req_t *req)
{
    const embedded_asset_t *asset = find_asset(req->uri);

    if (asset == nullptr) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Asset not found");
        return ESP_ERR_NOT_FOUND;
    }

    // Имена ассетов фиксированные, поэтому для разработки отключаем агрессивное кэширование,
    // чтобы браузер подхватывал свежую сборку после перепрошивки.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_embedded_file(req, asset->content_path, asset->start, asset->end);
}

esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Первый GET нужен для апгрейда HTTP-соединения до WebSocket.
        ESP_LOGI(kTag, "WebSocket client connected, fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    char *ack = nullptr;
    esp_err_t err;

    err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len == 0 || frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    // Выделяем буфер под входящее сообщение и добавляем завершающий ноль для удобной работы как со строкой.
    frame.payload = static_cast<uint8_t *>(std::malloc(frame.len + 1));
    if (frame.payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        std::free(frame.payload);
        return err;
    }

    reinterpret_cast<char *>(frame.payload)[frame.len] = '\0';
    ESP_LOGI(kTag, "Received WS payload: %s", reinterpret_cast<char *>(frame.payload));

    cJSON *request = cJSON_Parse(reinterpret_cast<char *>(frame.payload));
    if (request != nullptr) {
        cJSON *command = cJSON_GetObjectItemCaseSensitive(request, "command");
        const char *command_value = cJSON_IsString(command) ? command->valuestring : "";
        bool accepted = false;
        char feedback[160] = {};

        if (cJSON_IsString(command)) {
            dali_tx_plan_t plan = {};

            if (build_dali_tx_plan(command_value, &plan, feedback, sizeof(feedback))) {
                const esp_err_t send_err = dali_sniffer_send_frames(plan.frames, plan.frame_count);
                if (send_err == ESP_OK) {
                    accepted = true;
                    std::snprintf(feedback, sizeof(feedback), "Sent: %s", command_value);
                } else {
                    std::snprintf(feedback,
                                  sizeof(feedback),
                                  "Failed to send DALI command \"%s\": %s",
                                  command_value,
                                  esp_err_to_name(send_err));
                }
            }
        } else {
            std::snprintf(feedback, sizeof(feedback), "Missing string field \"command\"");
        }

        ack = build_command_ack_json(command_value, accepted);
        if (feedback[0] != '\0') {
            const esp_err_t message_err = ws_send_message(req, feedback);
            if (message_err != ESP_OK) {
                ESP_LOGW(kTag, "Failed to send WS feedback: %s", esp_err_to_name(message_err));
            }
        }
        cJSON_Delete(request);
    }

    std::free(frame.payload);

    if (ack == nullptr) {
        ack = build_command_ack_json("", false);
        if (ack == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_ws_frame_t response = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = reinterpret_cast<uint8_t *>(ack),
        .len = std::strlen(ack),
    };

    err = httpd_ws_send_frame(req, &response);
    cJSON_free(ack);
    return err;
}

}  // namespace

extern "C" esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Разрешаем wildcard-маршруты, чтобы один обработчик обслуживал /assets/*.
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = http_get_handler;

    httpd_uri_t assets_uri = {};
    assets_uri.uri = "/assets/*";
    assets_uri.method = HTTP_GET;
    assets_uri.handler = http_get_handler;

    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = ws_handler;
    ws_uri.is_websocket = true;
    BaseType_t task_created;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Регистрируем HTTP- и WebSocket-обработчики после успешного запуска сервера.
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ws_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &assets_uri));

    // Отдельная задача нужна, чтобы пушить обновления статуса независимо от входящих запросов.
    task_created = xTaskCreate(websocket_event_task, "ws_dali", 4096, nullptr, 5, nullptr);
    if (task_created != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "HTTP server started");
    return ESP_OK;
}

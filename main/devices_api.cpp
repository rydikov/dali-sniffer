#include "devices_api.h"

#include <cstdio>

extern "C" {
#include "cJSON.h"
}

#include "nvs.h"

namespace {

constexpr const char *kDevicesNamespace = "dali_devices";
constexpr uint8_t kDeviceBlobVersion = 1;
constexpr size_t kMaxDeviceRequestSize = 1024;

typedef struct {
    uint8_t version;
    uint8_t address;
    uint8_t flags;
    uint8_t reserved;
    uint16_t groups_mask;
    uint16_t scenes_mask;
} device_record_t;

static_assert(sizeof(device_record_t) == 8, "Unexpected device record size");

void set_json_type(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
}

esp_err_t send_json_text(httpd_req_t *req, const char *status, const char *json)
{
    if (status != nullptr) {
        httpd_resp_set_status(req, status);
    }

    set_json_type(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, json);
}

esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "error", message);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = send_json_text(req, status, payload);
    cJSON_free(payload);
    return err;
}

void device_key(uint8_t address, char *buffer, size_t buffer_size)
{
    std::snprintf(buffer, buffer_size, "dev_%02u", static_cast<unsigned>(address));
}

bool read_device(nvs_handle_t handle, uint8_t address, device_record_t *record)
{
    char key[8] = {};
    size_t blob_size = sizeof(*record);

    device_key(address, key, sizeof(key));
    if (nvs_get_blob(handle, key, record, &blob_size) != ESP_OK || blob_size != sizeof(*record)) {
        return false;
    }

    return record->version == kDeviceBlobVersion && record->address == address;
}

cJSON *array_from_mask(uint16_t mask)
{
    cJSON *array = cJSON_CreateArray();
    if (array == nullptr) {
        return nullptr;
    }

    for (uint8_t i = 0; i < 16; ++i) {
        if ((mask & (1U << i)) != 0U) {
            cJSON_AddItemToArray(array, cJSON_CreateNumber(i));
        }
    }

    return array;
}

cJSON *device_to_json(const device_record_t &record)
{
    cJSON *device = cJSON_CreateObject();
    cJSON *groups = nullptr;
    cJSON *scenes = nullptr;

    if (device == nullptr) {
        return nullptr;
    }

    groups = array_from_mask(record.groups_mask);
    scenes = array_from_mask(record.scenes_mask);
    if (groups == nullptr || scenes == nullptr ||
        cJSON_AddNumberToObject(device, "address", record.address) == nullptr ||
        cJSON_AddBoolToObject(device, "brightness", (record.flags & 0x01U) != 0U) == nullptr ||
        cJSON_AddBoolToObject(device, "colorTemperature", (record.flags & 0x02U) != 0U) == nullptr ||
        cJSON_AddBoolToObject(device, "rgbw", (record.flags & 0x04U) != 0U) == nullptr) {
        cJSON_Delete(groups);
        cJSON_Delete(scenes);
        cJSON_Delete(device);
        return nullptr;
    }

    cJSON_AddItemToObject(device, "groups", groups);
    cJSON_AddItemToObject(device, "scenes", scenes);
    return device;
}

bool parse_bool_field(cJSON *root, const char *name, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *value = cJSON_IsTrue(item);
    return true;
}

bool parse_number_array_mask(cJSON *root, const char *name, uint16_t *mask)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsArray(array)) {
        return false;
    }

    *mask = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, array)
    {
        if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 15 ||
            item->valuedouble != static_cast<double>(item->valueint)) {
            return false;
        }

        *mask |= static_cast<uint16_t>(1U << static_cast<uint8_t>(item->valueint));
    }

    return true;
}

bool parse_device_json(cJSON *root, device_record_t *record)
{
    cJSON *address = cJSON_GetObjectItemCaseSensitive(root, "address");
    bool brightness = false;
    bool color_temperature = false;
    bool rgbw = false;

    if (!cJSON_IsObject(root) || !cJSON_IsNumber(address) || address->valuedouble < 0 || address->valuedouble > 63 ||
        address->valuedouble != static_cast<double>(address->valueint)) {
        return false;
    }

    if (!parse_bool_field(root, "brightness", &brightness) ||
        !parse_bool_field(root, "colorTemperature", &color_temperature) ||
        !parse_bool_field(root, "rgbw", &rgbw) ||
        !parse_number_array_mask(root, "groups", &record->groups_mask) ||
        !parse_number_array_mask(root, "scenes", &record->scenes_mask)) {
        return false;
    }

    record->version = kDeviceBlobVersion;
    record->address = static_cast<uint8_t>(address->valueint);
    record->flags = 0;
    record->reserved = 0;
    if (brightness) {
        record->flags |= 0x01U;
    }
    if (color_temperature) {
        record->flags |= 0x02U;
    }
    if (rgbw) {
        record->flags |= 0x04U;
    }

    return true;
}

esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (req->content_len == 0 || req->content_len >= buffer_size || req->content_len > kMaxDeviceRequestSize) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }

        received += static_cast<size_t>(ret);
    }

    buffer[received] = '\0';
    return ESP_OK;
}

}  // namespace

esp_err_t devices_api_get_handler(httpd_req_t *req)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READONLY, &handle);
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    if (root == nullptr || devices == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(devices);
        if (err == ESP_OK) {
            nvs_close(handle);
        }
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "devices", devices);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (payload == nullptr) {
            return ESP_ERR_NO_MEM;
        }

        err = send_json_text(req, nullptr, payload);
        cJSON_free(payload);
        return err;
    }
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_json_error(req, "500 Internal Server Error", "Failed to open device storage");
    }

    for (uint8_t address = 0; address < 64; ++address) {
        device_record_t record = {};
        if (read_device(handle, address, &record)) {
            cJSON *device = device_to_json(record);
            if (device == nullptr) {
                nvs_close(handle);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddItemToArray(devices, device);
        }
    }

    nvs_close(handle);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = send_json_text(req, nullptr, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t devices_api_post_handler(httpd_req_t *req)
{
    char body[kMaxDeviceRequestSize + 1] = {};
    device_record_t record = {};
    device_record_t existing = {};
    nvs_handle_t handle = 0;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "Invalid request body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == nullptr) {
        return send_json_error(req, "400 Bad Request", "Invalid JSON payload");
    }

    const bool is_valid = parse_device_json(root, &record);
    cJSON_Delete(root);
    if (!is_valid) {
        return send_json_error(req, "400 Bad Request", "Invalid device payload");
    }

    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to open device storage");
    }

    if (read_device(handle, record.address, &existing)) {
        nvs_close(handle);
        return send_json_error(req, "409 Conflict", "Device address already exists");
    }

    char key[8] = {};
    device_key(record.address, key, sizeof(key));
    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to save device");
    }

    cJSON *response = cJSON_CreateObject();
    cJSON *device = device_to_json(record);
    if (response == nullptr || device == nullptr) {
        cJSON_Delete(response);
        cJSON_Delete(device);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(response, "device", device);
    char *payload = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = send_json_text(req, "201 Created", payload);
    cJSON_free(payload);
    return err;
}

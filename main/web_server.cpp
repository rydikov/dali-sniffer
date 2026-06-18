#include "web_server.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cJSON.h"
}

#include "dali_protocol.h"
#include "dali_sniffer.h"
#include "mqtt_bridge.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr const char *kTag = "web_server";
constexpr const char *kDevicesNamespace = "dali_devices";
constexpr uint8_t kDeviceBlobVersion = 1;
constexpr size_t kMaxDeviceRequestSize = 1024;

extern const char web_index_html_start[] asm("_binary_index_html_start");
extern const char web_index_html_end[] asm("_binary_index_html_end");
extern const char web_app_css_start[] asm("_binary_app_css_start");
extern const char web_app_css_end[] asm("_binary_app_css_end");
extern const char web_app_js_start[] asm("_binary_app_js_start");
extern const char web_app_js_end[] asm("_binary_app_js_end");

httpd_handle_t s_server = nullptr;

typedef struct {
    uint8_t version;
    uint8_t address;
    uint8_t flags;
    uint8_t reserved;
    uint16_t groups_mask;
    uint16_t scenes_mask;
} device_record_t;

static_assert(sizeof(device_record_t) == 8, "Unexpected device record size");

typedef struct {
    const char *uri;
    const char *content_path;
    const char *start;
    const char *end;
} embedded_asset_t;

const embedded_asset_t s_assets[] = {
    {
        .uri = "/",
        .content_path = "/index.html",
        .start = web_index_html_start,
        .end = web_index_html_end,
    },
    {
        .uri = "/devices",
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

const char *content_type_for_path(const char *path)
{
    const char *extension = std::strrchr(path, '.');

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

esp_err_t ws_send_json(httpd_handle_t server, int fd, const char *json)
{
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

void broadcast_message(const dali_frame_event_t &frame)
{
    dali_frame_description_t description = {};
    char *payload = nullptr;
    int client_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;

    if (s_server == nullptr) {
        return;
    }

    dali_describe_frame(frame, &description);
    payload = build_json_string_message("message", description.text);
    if (payload == nullptr) {
        ESP_LOGW(kTag, "Failed to build broadcast JSON payload");
        return;
    }

    if (httpd_get_client_list(s_server, &clients, client_fds) != ESP_OK) {
        cJSON_free(payload);
        return;
    }

    for (size_t i = 0; i < clients; ++i) {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
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
            mqtt_bridge_publish_sniffer_event(frame);
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

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_embedded_file(req, asset->content_path, asset->start, asset->end);
}

esp_err_t devices_get_handler(httpd_req_t *req)
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

esp_err_t devices_post_handler(httpd_req_t *req)
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

esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(kTag, "WebSocket client connected, fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    char *ack = nullptr;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len == 0 || frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

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
        const char *command_value = cJSON_IsString(command) && command->valuestring != nullptr ? command->valuestring : "";
        dali_command_exec_result_t result = {};

        if (cJSON_IsString(command) && command->valuestring != nullptr) {
            mqtt_bridge_publish_command_request("ws", command_value, true);
            dali_execute_command_text(command_value, &result);
        } else {
            std::snprintf(result.feedback, sizeof(result.feedback), "Missing string field \"command\"");
        }

        mqtt_bridge_publish_command_result("ws", command_value, result);
        ack = build_command_ack_json(command_value, result.sent);
        if (result.feedback[0] != '\0') {
            const esp_err_t message_err = ws_send_message(req, result.feedback);
            if (message_err != ESP_OK) {
                ESP_LOGW(kTag, "Failed to send WS feedback: %s", esp_err_to_name(message_err));
            }
        }
        cJSON_Delete(request);
    } else {
        const char *feedback = "Invalid JSON payload";
        ack = build_command_ack_json("", false);
        const esp_err_t message_err = ws_send_message(req, feedback);
        if (message_err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to send WS feedback: %s", esp_err_to_name(message_err));
        }
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
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = http_get_handler;

    httpd_uri_t devices_uri = {};
    devices_uri.uri = "/devices";
    devices_uri.method = HTTP_GET;
    devices_uri.handler = http_get_handler;

    httpd_uri_t devices_api_get_uri = {};
    devices_api_get_uri.uri = "/api/devices";
    devices_api_get_uri.method = HTTP_GET;
    devices_api_get_uri.handler = devices_get_handler;

    httpd_uri_t devices_api_post_uri = {};
    devices_api_post_uri.uri = "/api/devices";
    devices_api_post_uri.method = HTTP_POST;
    devices_api_post_uri.handler = devices_post_handler;

    httpd_uri_t assets_uri = {};
    assets_uri.uri = "/assets/*";
    assets_uri.method = HTTP_GET;
    assets_uri.handler = http_get_handler;

    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = ws_handler;
    ws_uri.is_websocket = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ws_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &devices_api_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &devices_api_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &devices_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &assets_uri));

    if (xTaskCreate(websocket_event_task, "ws_dali", 4096, nullptr, 5, nullptr) != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "HTTP server started");
    return ESP_OK;
}

#include "web_server.h"

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
    const char *start;
    const char *end;
} embedded_asset_t;

// Таблица статических файлов, которые отдаются напрямую из памяти устройства.
const embedded_asset_t s_assets[] = {
    {
        .uri = "/",
        .start = web_index_html_start,
        .end = web_index_html_end,
    },
    {
        .uri = "/assets/app.css",
        .start = web_app_css_start,
        .end = web_app_css_end,
    },
    {
        .uri = "/assets/app.js",
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

void build_dali_message_text(const dali_frame_event_t &frame, char *buffer, size_t buffer_size)
{
    const char *direction = frame.is_backward_frame ? "backward" : "forward";
    int width = 0;

    switch (frame.length) {
    case 8:
        width = 2;
        break;
    case 16:
        width = 4;
        break;
    case 24:
        width = 6;
        break;
    default:
        width = 8;
        break;
    }

    std::snprintf(buffer,
                  buffer_size,
                  "DALI %s frame (%u bit): 0x%0*" PRIX32,
                  direction,
                  frame.length,
                  width,
                  frame.data);
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
    return send_embedded_file(req, asset->uri, asset->start, asset->end);
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
        const bool accepted = cJSON_IsString(command);

        ack = build_command_ack_json(command_value, accepted);
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

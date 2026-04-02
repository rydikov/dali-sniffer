#include "web_server.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";
static const TickType_t STATUS_BROADCAST_PERIOD = pdMS_TO_TICKS(5000);

// Веб-ресурсы подключаются в прошивку на этапе линковки как бинарные массивы.
extern const char web_index_html_start[] asm("_binary_index_html_start");
extern const char web_index_html_end[] asm("_binary_index_html_end");
extern const char web_app_css_start[] asm("_binary_app_css_start");
extern const char web_app_css_end[] asm("_binary_app_css_end");
extern const char web_app_js_start[] asm("_binary_app_js_start");
extern const char web_app_js_end[] asm("_binary_app_js_end");

static httpd_handle_t s_server;

typedef struct {
    const char *uri;
    const char *start;
    const char *end;
} embedded_asset_t;

// Таблица статических файлов, которые отдаются напрямую из памяти устройства.
static const embedded_asset_t s_assets[] = {
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

static const char *content_type_for_path(const char *path)
{
    const char *extension = strrchr(path, '.');

    // Если расширение отсутствует, отдаем содержимое как обычный текст.
    if (extension == NULL) {
        return "text/plain; charset=utf-8";
    }

    if (strcmp(extension, ".html") == 0) {
        return "text/html; charset=utf-8";
    }

    if (strcmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }

    if (strcmp(extension, ".js") == 0) {
        return "text/javascript; charset=utf-8";
    }

    if (strcmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }

    if (strcmp(extension, ".png") == 0) {
        return "image/png";
    }

    if (strcmp(extension, ".woff2") == 0) {
        return "font/woff2";
    }

    return "application/octet-stream";
}

static bool extract_json_string(const char *json, const char *key, char *output, size_t output_size)
{
    char pattern[32];
    const char *start;
    size_t i = 0;

    // Ищем в простом JSON фрагмент вида "key":"value" без полноценного парсера.
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    start = strstr(json, pattern);
    if (start == NULL) {
        return false;
    }

    start += strlen(pattern);
    // Копируем значение до закрывающей кавычки, учитывая экранированные символы.
    while (*start != '\0' && *start != '"' && i + 1 < output_size) {
        if (*start == '\\' && start[1] != '\0') {
            ++start;
        }
        output[i++] = *start++;
    }
    output[i] = '\0';

    return *start == '"';
}

static void json_escape_string(const char *input, char *output, size_t output_size)
{
    size_t j = 0;

    // Экранируем строку перед вставкой в JSON-ответ.
    for (size_t i = 0; input[i] != '\0' && j + 1 < output_size; ++i) {
        const char ch = input[i];

        if ((ch == '\\' || ch == '"') && j + 2 < output_size) {
            output[j++] = '\\';
            output[j++] = ch;
        } else if ((ch == '\n' || ch == '\r' || ch == '\t') && j + 2 < output_size) {
            output[j++] = '\\';
            output[j++] = (ch == '\n') ? 'n' : (ch == '\r' ? 'r' : 't');
        } else if ((unsigned char)ch >= 0x20) {
            output[j++] = ch;
        }
    }

    output[j] = '\0';
}

static esp_err_t send_embedded_file(httpd_req_t *req,
                                    const char *path,
                                    const char *start,
                                    const char *end)
{
    // У бинарных символов, сгенерированных линкером, в конце обычно есть завершающий ноль,
    // поэтому при отправке вычитаем один байт из диапазона.
    const ptrdiff_t content_length = end - start - 1;

    httpd_resp_set_type(req, content_type_for_path(path));
    return httpd_resp_send(req, start, content_length > 0 ? content_length : 0);
}

static const embedded_asset_t *find_asset(const char *uri)
{
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); ++i) {
        if (strcmp(uri, s_assets[i].uri) == 0) {
            return &s_assets[i];
        }
    }

    return NULL;
}

static esp_err_t ws_send_json_async(httpd_handle_t server, int fd, const char *json)
{
    // Для рассылки статуса используем асинхронную отправку текстового WebSocket-кадра.
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };

    return httpd_ws_send_frame_async(server, fd, &frame);
}

static void broadcast_message(void)
{
    static const char *message = "Hello world";
    char payload[96];
    int client_fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t clients = CONFIG_LWIP_MAX_SOCKETS;

    if (s_server == NULL) {
        return;
    }

    snprintf(payload, sizeof(payload), "{\"type\":\"current_time\",\"value\":\"%s\"}", message);

    if (httpd_get_client_list(s_server, &clients, client_fds) != ESP_OK) {
        return;
    }

    // Проходим только по активным WebSocket-клиентам и отправляем им сообщение.
    for (size_t i = 0; i < clients; ++i) {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            ESP_LOGI(TAG, "Sending current time to fd %d: %s", client_fds[i], message);
            esp_err_t err = ws_send_json_async(s_server, client_fds[i], payload);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send current time to fd %d: %s", client_fds[i], esp_err_to_name(err));
            }
        }
    }
}

static void websocket_broadcast_task(void *arg)
{
    (void)arg;

    // Фоновая задача периодически публикует сообщение всем браузерам.
    while (1) {
        broadcast_message();
        vTaskDelay(STATUS_BROADCAST_PERIOD);
    }
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    // Главную страницу не кэшируем, чтобы браузер сразу видел обновленную прошивку/UI.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_embedded_file(req, "/index.html", web_index_html_start, web_index_html_end);
}

static esp_err_t asset_get_handler(httpd_req_t *req)
{
    const embedded_asset_t *asset = find_asset(req->uri);

    if (asset == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Asset not found");
        return ESP_ERR_NOT_FOUND;
    }

    // Для ассетов включаем агрессивное кэширование: имена файлов фиксированы внутри сборки.
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return send_embedded_file(req, req->uri, asset->start, asset->end);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Первый GET нужен для апгрейда HTTP-соединения до WebSocket.
        ESP_LOGI(TAG, "WebSocket client connected, fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
    };
    char command[128];
    char escaped_command[192];
    char ack[288];
    esp_err_t err;

    err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len == 0 || frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    // Выделяем буфер под входящее сообщение и добавляем завершающий ноль для удобной работы как со строкой.
    frame.payload = malloc(frame.len + 1);
    if (frame.payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(frame.payload);
        return err;
    }

    ((char *)frame.payload)[frame.len] = '\0';
    ESP_LOGI(TAG, "Received WS payload: %s", (char *)frame.payload);

    // Сейчас сервер подтверждает получение команды, если в JSON есть поле "command".
    if (extract_json_string((char *)frame.payload, "command", command, sizeof(command))) {
        json_escape_string(command, escaped_command, sizeof(escaped_command));
        snprintf(ack,
                 sizeof(ack),
                 "{\"type\":\"command_ack\",\"command\":\"%s\",\"accepted\":true}",
                 escaped_command);
    } else {
        snprintf(ack, sizeof(ack), "{\"type\":\"command_ack\",\"command\":\"\",\"accepted\":false}");
    }

    free(frame.payload);

    httpd_ws_frame_t response = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)ack,
        .len = strlen(ack),
    };

    return httpd_ws_send_frame(req, &response);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Разрешаем wildcard-маршруты, чтобы один обработчик обслуживал /assets/*.
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_uri_t asset_uri = {
        .uri = "/assets/*",
        .method = HTTP_GET,
        .handler = asset_get_handler,
    };
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    BaseType_t task_created;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Регистрируем HTTP- и WebSocket-обработчики после успешного запуска сервера.
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &asset_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ws_uri));

    // Отдельная задача нужна, чтобы пушить обновления статуса независимо от входящих запросов.
    task_created = xTaskCreate(websocket_broadcast_task, "ws_status", 4096, NULL, 5, NULL);
    if (task_created != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

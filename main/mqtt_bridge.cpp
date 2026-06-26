#include "mqtt_bridge.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C" {
#include "cJSON.h"
}

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "mqtt_device_set.h"

namespace {

constexpr const char *kTag = "mqtt_bridge";
constexpr size_t kTopicBufferSize = 96;
constexpr size_t kBrokerUriSize = 160;
constexpr size_t kCommandTextSize = 128;
constexpr size_t kQueueSize = 8;

struct mqtt_command_job_t {
    mqtt_device_set_command_kind_t kind;
    char command[kCommandTextSize];
    size_t frame_count;
    dali_tx_frame_t frames[kMaxDaliTxFrames];
};

esp_mqtt_client_handle_t s_client = nullptr;
bool s_enabled = false;
bool s_connected = false;
QueueHandle_t s_command_queue = nullptr;
char s_root_topic[kTopicBufferSize] = {};
char s_status_topic[kTopicBufferSize] = {};
char s_command_request_topic[kTopicBufferSize] = {};
char s_command_result_topic[kTopicBufferSize] = {};
char s_command_execute_topic[kTopicBufferSize] = {};
char s_device_set_topic[kTopicBufferSize] = {};
char s_group_set_topic[kTopicBufferSize] = {};
char s_broker_uri[kBrokerUriSize] = {};

bool set_topic(char *buffer, size_t buffer_size, const char *suffix)
{
    const size_t root_length = std::strlen(s_root_topic);
    const size_t suffix_length = std::strlen(suffix);

    if (root_length + suffix_length + 1 > buffer_size) {
        return false;
    }

    std::memcpy(buffer, s_root_topic, root_length);
    std::memcpy(buffer + root_length, suffix, suffix_length + 1);
    return true;
}

bool build_topics()
{
    const int root_length = std::snprintf(s_root_topic, sizeof(s_root_topic), "dali/%s", CONFIG_MQTT_CUSTOM_ID);
    if (root_length < 0 || static_cast<size_t>(root_length) >= sizeof(s_root_topic)) {
        return false;
    }

    return set_topic(s_status_topic, sizeof(s_status_topic), "/status") &&
           set_topic(s_command_request_topic, sizeof(s_command_request_topic), "/event/command/request") &&
           set_topic(s_command_result_topic, sizeof(s_command_result_topic), "/event/command/result") &&
           set_topic(s_command_execute_topic, sizeof(s_command_execute_topic), "/command/execute") &&
           set_topic(s_device_set_topic, sizeof(s_device_set_topic), "/device/+/+") &&
           set_topic(s_group_set_topic, sizeof(s_group_set_topic), "/group/+/+");
}

bool publish_json(const char *topic, cJSON *root)
{
    if (!s_enabled || !s_connected || s_client == nullptr || root == nullptr) {
        return false;
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == nullptr) {
        return false;
    }

    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
    cJSON_free(payload);
    return msg_id >= 0;
}

void add_common_event_fields(cJSON *root, const char *type, const char *origin)
{
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "origin", origin);
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
}

void publish_invalid_mqtt_command(const char *command_text, const char *feedback)
{
    dali_command_exec_result_t result = {};
    result.accepted = false;
    result.sent = false;
    result.frame_count = 0;
    std::snprintf(result.feedback, sizeof(result.feedback), "%s", feedback);
    mqtt_bridge_publish_command_request("mqtt", command_text, false);
    mqtt_bridge_publish_command_result("mqtt", command_text, result);
}

void command_executor_task(void *arg)
{
    (void)arg;
    mqtt_command_job_t job = {};

    while (true) {
        if (xQueueReceive(s_command_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        dali_command_exec_result_t result = {};
        if (job.kind == mqtt_device_set_command_kind_t::Frames) {
            result.accepted = job.frame_count > 0;
            result.frame_count = job.frame_count;
            const esp_err_t send_err = result.accepted ? dali_sniffer_send_frames(job.frames, job.frame_count)
                                                       : ESP_ERR_INVALID_ARG;
            result.sent = send_err == ESP_OK;
            if (send_err == ESP_OK) {
                std::snprintf(result.feedback, sizeof(result.feedback), "Sent: %s", job.command);
            } else {
                std::snprintf(result.feedback,
                              sizeof(result.feedback),
                              "Failed to send DALI command \"%s\": %s",
                              job.command,
                              esp_err_to_name(send_err));
            }
        } else {
            dali_execute_command_text(job.command, &result);
        }
        mqtt_bridge_publish_command_result("mqtt", job.command, result);
    }
}

void enqueue_mqtt_command(const mqtt_command_job_t &job)
{
    if (xQueueSend(s_command_queue, &job, 0) != pdTRUE) {
        publish_invalid_mqtt_command(job.command, "MQTT command queue is full");
        return;
    }

    mqtt_bridge_publish_command_request("mqtt", job.command, true);
}

void handle_mqtt_command_payload(const char *payload, int payload_len)
{
    char command_text[kCommandTextSize] = {};
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == nullptr) {
        publish_invalid_mqtt_command("", "Invalid JSON payload");
        return;
    }

    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(command) || command->valuestring == nullptr || command->valuestring[0] == '\0') {
        cJSON_Delete(root);
        publish_invalid_mqtt_command("", "Missing string field \"command\"");
        return;
    }

    std::snprintf(command_text, sizeof(command_text), "%s", command->valuestring);
    cJSON_Delete(root);

    mqtt_command_job_t job = {};
    job.kind = mqtt_device_set_command_kind_t::Text;
    std::snprintf(job.command, sizeof(job.command), "%s", command_text);
    enqueue_mqtt_command(job);
}

void handle_device_set_message(esp_mqtt_event_handle_t event)
{
    if (event == nullptr || event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        return;
    }

    mqtt_device_set_result_t result = {};
    mqtt_device_set_build_command(s_root_topic,
                                  event->topic,
                                  event->topic_len,
                                  event->data,
                                  event->data_len,
                                  event->retain,
                                  &result);

    switch (result.status) {
    case mqtt_device_set_status_t::NotMatched:
    case mqtt_device_set_status_t::IgnoredRetained:
        return;
    case mqtt_device_set_status_t::Invalid:
        publish_invalid_mqtt_command(result.command.command_text, result.error);
        return;
    case mqtt_device_set_status_t::Ready: {
        mqtt_command_job_t job = {};
        job.kind = result.command.kind;
        std::snprintf(job.command, sizeof(job.command), "%s", result.command.command_text);
        job.frame_count = result.command.frame_count;
        if (job.frame_count > 0) {
            std::memcpy(job.frames, result.command.frames, sizeof(job.frames[0]) * job.frame_count);
        }
        enqueue_mqtt_command(job);
        return;
    }
    }
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(kTag,
                 "Connected to broker, subscribing to %s, %s and %s",
                 s_command_execute_topic,
                 s_device_set_topic,
                 s_group_set_topic);
        esp_mqtt_client_subscribe(s_client, s_command_execute_topic, 0);
        esp_mqtt_client_subscribe(s_client, s_device_set_topic, 0);
        esp_mqtt_client_subscribe(s_client, s_group_set_topic, 0);
        mqtt_bridge_publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(kTag, "Disconnected from broker");
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == static_cast<int>(std::strlen(s_command_execute_topic)) &&
            std::strncmp(event->topic, s_command_execute_topic, event->topic_len) == 0) {
            if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
                publish_invalid_mqtt_command("", "Fragmented MQTT payload is not supported");
                break;
            }
            handle_mqtt_command_payload(event->data, event->data_len);
        } else {
            handle_device_set_message(event);
        }
        break;
    default:
        break;
    }
}

void build_broker_uri(char *buffer, size_t buffer_size)
{
    if (std::strncmp(CONFIG_MQTT_BROKER_ADDRESS, "mqtt://", 7) == 0 ||
        std::strncmp(CONFIG_MQTT_BROKER_ADDRESS, "mqtts://", 8) == 0) {
        std::snprintf(buffer, buffer_size, "%s", CONFIG_MQTT_BROKER_ADDRESS);
        return;
    }

    std::snprintf(buffer, buffer_size, "mqtt://%s", CONFIG_MQTT_BROKER_ADDRESS);
}

void add_ip_address(cJSON *root)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    char ip_text[16] = "";

    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_text, sizeof(ip_text));
    }

    cJSON_AddStringToObject(root, "ip", ip_text);
}

}  // namespace

esp_err_t mqtt_bridge_start(void)
{
    if (CONFIG_MQTT_BROKER_ADDRESS[0] == '\0') {
        ESP_LOGI(kTag, "MQTT disabled: broker address is empty");
        return ESP_OK;
    }

    if (!build_topics()) {
        ESP_LOGE(kTag, "MQTT topic is too long. Shorten CONFIG_MQTT_CUSTOM_ID.");
        return ESP_ERR_INVALID_SIZE;
    }
    s_command_queue = xQueueCreate(kQueueSize, sizeof(mqtt_command_job_t));
    if (s_command_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(command_executor_task, "mqtt_cmd", 4096, nullptr, 5, nullptr) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = nullptr;
        return ESP_FAIL;
    }

    build_broker_uri(s_broker_uri, sizeof(s_broker_uri));
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = s_broker_uri;

    s_client = esp_mqtt_client_init(&config);
    if (s_client == nullptr) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr));
    s_enabled = true;
    ESP_LOGI(kTag, "Starting MQTT bridge with root topic %s", s_root_topic);
    return esp_mqtt_client_start(s_client);
}

bool mqtt_bridge_is_enabled(void)
{
    return s_enabled;
}

void mqtt_bridge_publish_status(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddBoolToObject(root, "mqtt_enabled", s_enabled);
    cJSON_AddBoolToObject(root, "mqtt_connected", s_connected);
    cJSON_AddStringToObject(root, "custom_id", CONFIG_MQTT_CUSTOM_ID);
    cJSON_AddStringToObject(root, "root_topic", s_root_topic);
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    add_ip_address(root);
    publish_json(s_status_topic, root);
    cJSON_Delete(root);
}

void mqtt_bridge_publish_command_request(const char *origin, const char *command_text, bool accepted)
{
    if (!s_enabled) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }

    add_common_event_fields(root, "command_request", origin);
    cJSON_AddStringToObject(root, "command_text", command_text != nullptr ? command_text : "");
    cJSON_AddBoolToObject(root, "accepted", accepted);
    publish_json(s_command_request_topic, root);
    cJSON_Delete(root);
}

void mqtt_bridge_publish_command_result(const char *origin,
                                        const char *command_text,
                                        const dali_command_exec_result_t &result)
{
    if (!s_enabled) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }

    add_common_event_fields(root, "command_result", origin);
    cJSON_AddStringToObject(root, "command_text", command_text != nullptr ? command_text : "");
    cJSON_AddBoolToObject(root, "accepted", result.accepted);
    cJSON_AddBoolToObject(root, "sent", result.sent);
    cJSON_AddNumberToObject(root, "frame_count", result.frame_count);
    cJSON_AddStringToObject(root, "feedback", result.feedback);
    publish_json(s_command_result_topic, root);
    cJSON_Delete(root);
}

static void publish_device_state_number(uint8_t address, const char *state_name, unsigned value)
{
    if (!s_enabled || !s_connected || s_client == nullptr || address > 63 || state_name == nullptr) {
        return;
    }

    char topic[kTopicBufferSize] = {};
    char payload[11] = {};
    const int topic_length = std::snprintf(topic,
                                           sizeof(topic),
                                           "%s/device/%u/%s",
                                           s_root_topic,
                                           static_cast<unsigned>(address),
                                           state_name);
    if (topic_length < 0 || static_cast<size_t>(topic_length) >= sizeof(topic)) {
        ESP_LOGW(kTag,
                 "Device state topic is too long for device %u state %s",
                 static_cast<unsigned>(address),
                 state_name);
        return;
    }

    std::snprintf(payload, sizeof(payload), "%u", value);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
}

static void publish_device_state_text(uint8_t address, const char *state_name, const char *payload)
{
    if (!s_enabled || !s_connected || s_client == nullptr || address > 63 || state_name == nullptr || payload == nullptr) {
        return;
    }

    char topic[kTopicBufferSize] = {};
    const int topic_length = std::snprintf(topic,
                                           sizeof(topic),
                                           "%s/device/%u/%s",
                                           s_root_topic,
                                           static_cast<unsigned>(address),
                                           state_name);
    if (topic_length < 0 || static_cast<size_t>(topic_length) >= sizeof(topic)) {
        ESP_LOGW(kTag,
                 "Device state topic is too long for device %u state %s",
                 static_cast<unsigned>(address),
                 state_name);
        return;
    }

    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
}

void mqtt_bridge_publish_device_brightness(uint8_t address, uint8_t level)
{
    publish_device_state_number(address, "brightness", static_cast<unsigned>(level));
}

void mqtt_bridge_publish_device_color_temperature(uint8_t address, uint32_t kelvin)
{
    publish_device_state_number(address, "color_temperature", static_cast<unsigned>(kelvin));
}

void mqtt_bridge_publish_device_rgb(uint8_t address, uint8_t red, uint8_t green, uint8_t blue)
{
    char payload[12] = {};
    std::snprintf(payload,
                  sizeof(payload),
                  "%u,%u,%u",
                  static_cast<unsigned>(red),
                  static_cast<unsigned>(green),
                  static_cast<unsigned>(blue));
    publish_device_state_text(address, "rgb", payload);
}

void mqtt_bridge_publish_device_white(uint8_t address, uint8_t level)
{
    publish_device_state_number(address, "white", static_cast<unsigned>(level));
}

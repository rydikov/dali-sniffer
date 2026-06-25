#include "devices_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cJSON.h"
}

#include "nvs.h"

namespace {

// Этот файл реализует небольшой REST API для страницы /devices.
// Данные хранятся в NVS (Non-Volatile Storage)
//
// Снаружи API работает с обычным JSON:
//   GET    /api/devices          -> список устройств
//   POST   /api/devices          -> создать устройство
//   PUT    /api/devices/<addr>   -> обновить устройство
//   DELETE /api/devices/<addr>   -> удалить устройство
//
// Внутри каждое устройство хранится отдельным NVS blob под ключом dev_00..dev_63.
constexpr const char *kDevicesNamespace = "dali_devices";
constexpr const char *kDeviceUriPrefix = "/api/devices/";
constexpr size_t kMaxDeviceRequestSize = 1024;

// Представление устройства в NVS.
typedef struct {
    // DALI short address: допустимый диапазон 0..63.
    uint8_t address;

    devices_api_device_status_t status;

    // Возможности
    bool brightness;
    bool color_temperature;
    bool rgbw;

    // Группы и сцены хранятся битовыми масками. Например, если устройство в
    // группах 0 и 3, groups_mask будет 0000 0000 0000 1001.
    uint16_t groups_mask;
    uint16_t scenes_mask;
} device_record_t;

// Защита от случайного изменения размера структуры. Если добавить поле,
// сборка остановится, и формат NVS придется пересмотреть. Если формат поменялся,
// старые записи NVS нужно очистить вручную перед использованием новой прошивки.
static_assert(sizeof(device_record_t) == 10, "Unexpected device record size");

void set_json_type(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
}

esp_err_t send_json_text(httpd_req_t *req, const char *status, const char *json)
{
    // status может быть nullptr: тогда ESP HTTP server оставит статус 200 OK.
    if (status != nullptr) {
        httpd_resp_set_status(req, status);
    }

    set_json_type(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, json);
}

esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    // Все ошибки API возвращаем в одном простом формате:
    // {"error":"..."}
    // Так легче обрабатывать 400/404/409/500.
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
    // Ключ NVS строится из адреса. Ширина 2 символа делает ключи стабильными:
    // dev_00, dev_01, ..., dev_63.
    std::snprintf(buffer, buffer_size, "dev_%02u", static_cast<unsigned>(address));
}

bool read_device(nvs_handle_t handle, uint8_t address, device_record_t *record)
{
    // Читаем конкретный blob из NVS и дополнительно проверяем, что адрес внутри
    // blob совпадает с ключом. Если формат структуры поменялся, NVS нужно
    // очистить вручную, иначе старые записи могут не пройти проверку размера.
    char key[8] = {};
    size_t blob_size = sizeof(*record);

    device_key(address, key, sizeof(key));
    if (nvs_get_blob(handle, key, record, &blob_size) != ESP_OK ||
        blob_size != sizeof(*record) ||
        record->address != address ||
        record->status > DEVICES_API_DEVICE_STATUS_FAILURE) {
        return false;
    }

    return true;
}

const char *device_status_to_json(devices_api_device_status_t status)
{
    switch (status) {
    case DEVICES_API_DEVICE_STATUS_ON:
        return "on";
    case DEVICES_API_DEVICE_STATUS_FAILURE:
        return "failure";
    case DEVICES_API_DEVICE_STATUS_OFF:
        return "off";
    }

    return "off";
}

cJSON *array_from_mask(uint16_t mask)
{
    // В NVS группы/сцены лежат как uint16_t, а во внешнем API они выглядят как
    // массив чисел. Например mask 0b00000101 превращается в [0, 2].
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
    // Преобразуем внутренний бинарный record в JSON, который ожидает frontend.
    // Важно: cJSON_AddItemToObject забирает владение объектами groups/scenes,
    // поэтому после успешного добавления вручную освобождать их уже не нужно.
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
        cJSON_AddStringToObject(device, "status", device_status_to_json(record.status)) == nullptr ||
        cJSON_AddBoolToObject(device, "brightness", record.brightness) == nullptr ||
        cJSON_AddBoolToObject(device, "colorTemperature", record.color_temperature) == nullptr ||
        cJSON_AddBoolToObject(device, "rgbw", record.rgbw) == nullptr) {
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
    // Поля возможностей должны быть именно boolean. Если frontend случайно
    // пришлет строку "true" или число 1, такая нагрузка считается невалидной.
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *value = cJSON_IsTrue(item);
    return true;
}

bool parse_number_array_mask(cJSON *root, const char *name, uint16_t *mask)
{
    // groups и scenes приходят как массивы чисел 0..15. Здесь мы валидируем
    // каждый элемент и сразу собираем компактную битовую маску для NVS.
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsArray(array)) {
        return false;
    }

    *mask = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, array)
    {
        // Проверяем не только диапазон, но и что число целое. cJSON хранит
        // number одновременно как double и int; сравнение отсекает 1.5.
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
    // Это центральная валидация входящего JSON для POST/PUT. На выходе получаем
    // уже готовый device_record_t, который можно записывать в NVS.
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

    record->address = static_cast<uint8_t>(address->valueint);
    record->status = DEVICES_API_DEVICE_STATUS_OFF;
    record->brightness = brightness;
    record->color_temperature = color_temperature;
    record->rgbw = rgbw;

    return true;
}

bool parse_device_uri_address(httpd_req_t *req, uint8_t *address)
{
    // Для PUT/DELETE адрес берется из URI вида /api/devices/12.
    // ESP HTTP server уже сматчил route /api/devices/*, но конкретное значение
    // после префикса нам нужно разобрать вручную.
    if (req == nullptr || address == nullptr) {
        return false;
    }

    const size_t prefix_length = std::strlen(kDeviceUriPrefix);
    if (std::strncmp(req->uri, kDeviceUriPrefix, prefix_length) != 0) {
        return false;
    }

    const char *address_text = req->uri + prefix_length;
    if (*address_text == '\0') {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(address_text, &end, 10);
    // *end должен указывать на конец строки. Так /api/devices/12abc не пройдет.
    if (*end != '\0' || parsed > 63) {
        return false;
    }

    *address = static_cast<uint8_t>(parsed);
    return true;
}

esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    // HTTP body читается потоково: httpd_req_recv может вернуть только часть
    // данных, поэтому крутим цикл, пока не получим весь content_len.
    // Буфер на один байт больше лимита, чтобы в конце поставить '\0' для cJSON.
    if (req->content_len == 0 || req->content_len >= buffer_size || req->content_len > kMaxDeviceRequestSize) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            // Таймаут не всегда означает ошибку запроса: данных может просто
            // еще не быть в сокете. В этом случае продолжаем ждать.
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
    // GET /api/devices
    // Возвращает {"devices":[...]}.
    //
    // NVS не умеет "выбрать все ключи с префиксом" так же удобно, как база
    // данных, поэтому мы просто перебираем все возможные DALI-адреса 0..63 и
    // пытаемся прочитать соответствующий ключ dev_XX.
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
        // Namespace еще не создан. Это нормальная ситуация для первой загрузки:
        // устройств нет, значит возвращаем пустой список вместо ошибки.
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
            // Поврежденные записи read_device просто не отдаст наружу.
            // API показывает только валидные устройства.
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
    // POST /api/devices
    // Создает новое устройство. Адрес должен быть свободен, иначе возвращаем
    // 409 Conflict, чтобы frontend мог показать понятное сообщение.
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
        // Адрес DALI в нашем списке уникален. Повторное создание по тому же
        // адресу запрещаем, а изменение существующего устройства делаем через PUT.
        nvs_close(handle);
        return send_json_error(req, "409 Conflict", "Device address already exists");
    }

    char key[8] = {};
    device_key(record.address, key, sizeof(key));
    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK) {
        // nvs_set_blob только подготавливает изменение. nvs_commit делает его
        // постоянным, чтобы запись пережила перезагрузку.
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

esp_err_t devices_api_put_handler(httpd_req_t *req)
{
    // PUT /api/devices/<address>
    // Обновляет существующее устройство. Адрес менять не разрешаем: он является
    // частью URI и ключом NVS. Если нужно "сменить адрес", проще удалить старое
    // устройство и создать новое.
    uint8_t uri_address = 0;
    char body[kMaxDeviceRequestSize + 1] = {};
    device_record_t record = {};
    device_record_t existing = {};
    nvs_handle_t handle = 0;

    if (!parse_device_uri_address(req, &uri_address)) {
        return send_json_error(req, "400 Bad Request", "Invalid device address");
    }

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "Invalid request body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == nullptr) {
        return send_json_error(req, "400 Bad Request", "Invalid JSON payload");
    }

    const bool is_valid = parse_device_json(root, &record);
    cJSON_Delete(root);
    if (!is_valid || record.address != uri_address) {
        // record.address должен совпадать с адресом в URI. Это защищает от
        // запроса PUT /api/devices/1 с JSON {"address":2,...}.
        return send_json_error(req, "400 Bad Request", "Invalid device payload");
    }

    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to open device storage");
    }

    if (!read_device(handle, uri_address, &existing)) {
        // PUT обновляет только существующие записи. Создание через PUT здесь
        // специально не поддерживаем, чтобы поведение API было явным.
        nvs_close(handle);
        return send_json_error(req, "404 Not Found", "Device not found");
    }
    record.status = existing.status;

    char key[8] = {};
    device_key(uri_address, key, sizeof(key));
    // Для обновления перезаписываем весь blob целиком. Сейчас запись маленькая,
    // поэтому это проще и надежнее, чем частично менять отдельные поля.
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

    err = send_json_text(req, nullptr, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t devices_api_delete_handler(httpd_req_t *req)
{
    // DELETE /api/devices/<address>
    // Удаляет существующее устройство из NVS. Успешный ответ без тела:
    // 204 No Content.
    uint8_t address = 0;
    nvs_handle_t handle = 0;
    device_record_t existing = {};

    if (!parse_device_uri_address(req, &address)) {
        return send_json_error(req, "400 Bad Request", "Invalid device address");
    }

    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to open device storage");
    }

    if (!read_device(handle, address, &existing)) {
        // Если ключа нет или blob поврежден/не той версии, для API это выглядит
        // как отсутствие устройства по этому адресу.
        nvs_close(handle);
        return send_json_error(req, "404 Not Found", "Device not found");
    }

    char key[8] = {};
    device_key(address, key, sizeof(key));
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to delete device");
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // Для 204 тело ответа не отправляем.
    return httpd_resp_send(req, nullptr, 0);
}

enum class DeviceCapability : uint8_t {
    Any,
    Brightness,
    ColorTemperature,
    Rgbw,
};

static bool device_has_capability(const device_record_t &record, DeviceCapability capability)
{
    switch (capability) {
    case DeviceCapability::Any:
        return true;
    case DeviceCapability::Brightness:
        return record.brightness;
    case DeviceCapability::ColorTemperature:
        return record.color_temperature;
    case DeviceCapability::Rgbw:
        return record.rgbw;
    }

    return false;
}

static esp_err_t find_devices_by_capability(const char *address_kind,
                                            bool has_address_value,
                                            int address_value,
                                            bool require_scene,
                                            uint8_t scene,
                                            DeviceCapability capability,
                                            devices_api_device_match_t *matches,
                                            size_t max_matches,
                                            size_t *match_count)
{
    if (address_kind == nullptr || matches == nullptr || match_count == nullptr || scene > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    *match_count = 0;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t address = 0; address < 64 && *match_count < max_matches; ++address) {
        device_record_t record = {};
        if (!read_device(handle, address, &record) || !device_has_capability(record, capability)) {
            continue;
        }

        if (require_scene && (record.scenes_mask & (1U << scene)) == 0U) {
            continue;
        }

        bool matched = false;
        if (std::strcmp(address_kind, "short") == 0) {
            matched = has_address_value && address_value == record.address;
        } else if (std::strcmp(address_kind, "group") == 0) {
            matched = has_address_value && address_value >= 0 && address_value <= 15 &&
                      (record.groups_mask & (1U << static_cast<uint8_t>(address_value))) != 0U;
        } else if (std::strcmp(address_kind, "broadcast") == 0) {
            matched = true;
        }

        if (matched) {
            matches[*match_count].address = record.address;
            *match_count = *match_count + 1;
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t devices_api_update_device_status(uint8_t address,
                                           devices_api_device_status_t status,
                                           devices_api_device_status_t *previous_status)
{
    if (address > 63 || status > DEVICES_API_DEVICE_STATUS_FAILURE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kDevicesNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }

    device_record_t record = {};
    if (!read_device(handle, address, &record)) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    if (previous_status != nullptr) {
        *previous_status = record.status;
    }

    if (record.status == status) {
        nvs_close(handle);
        return ESP_OK;
    }

    record.status = status;

    char key[8] = {};
    device_key(address, key, sizeof(key));
    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t devices_api_find_devices(const char *address_kind,
                                   bool has_address_value,
                                   int address_value,
                                   bool require_scene,
                                   uint8_t scene,
                                   devices_api_device_match_t *matches,
                                   size_t max_matches,
                                   size_t *match_count)
{
    return find_devices_by_capability(address_kind,
                                      has_address_value,
                                      address_value,
                                      require_scene,
                                      scene,
                                      DeviceCapability::Any,
                                      matches,
                                      max_matches,
                                      match_count);
}

esp_err_t devices_api_find_brightness_devices(const char *address_kind,
                                              bool has_address_value,
                                              int address_value,
                                              bool require_scene,
                                              uint8_t scene,
                                              devices_api_device_match_t *matches,
                                              size_t max_matches,
                                              size_t *match_count)
{
    return find_devices_by_capability(address_kind,
                                      has_address_value,
                                      address_value,
                                      require_scene,
                                      scene,
                                      DeviceCapability::Brightness,
                                      matches,
                                      max_matches,
                                      match_count);
}

esp_err_t devices_api_find_color_temperature_devices(const char *address_kind,
                                                     bool has_address_value,
                                                     int address_value,
                                                     bool require_scene,
                                                     uint8_t scene,
                                                     devices_api_device_match_t *matches,
                                                     size_t max_matches,
                                                     size_t *match_count)
{
    return find_devices_by_capability(address_kind,
                                      has_address_value,
                                      address_value,
                                      require_scene,
                                      scene,
                                      DeviceCapability::ColorTemperature,
                                      matches,
                                      max_matches,
                                      match_count);
}

esp_err_t devices_api_find_rgbw_devices(const char *address_kind,
                                        bool has_address_value,
                                        int address_value,
                                        bool require_scene,
                                        uint8_t scene,
                                        devices_api_device_match_t *matches,
                                        size_t max_matches,
                                        size_t *match_count)
{
    return find_devices_by_capability(address_kind,
                                      has_address_value,
                                      address_value,
                                      require_scene,
                                      scene,
                                      DeviceCapability::Rgbw,
                                      matches,
                                      max_matches,
                                      match_count);
}

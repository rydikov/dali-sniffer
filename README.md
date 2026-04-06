| Supported Targets | ESP32 |
| ----------------- | ----- |

# Wi-Fi WebSocket Chat Example

Этот проект демонстрирует приложение для `ESP32`, которое подключается к Wi‑Fi, поднимает встроенный HTTP/WebSocket сервер и отдаёт собранный фронтенд через встроенную production-статику.

После старта прошивка также поднимает sniffer DALI-шины и отправляет события на шине в WebSocket UI.

## Поддерживаемая платформа

Поддерживается только `ESP32`.

Перед сборкой укажите целевой чип:

```bash
idf.py set-target esp32
```

## Настройка проекта

Откройте меню конфигурации:

```bash
idf.py menuconfig
```

В разделе `WiFi Configuration` доступны параметры:

* `Wi-Fi SSID` - имя беспроводной сети;
* `Wi-Fi password` - пароль беспроводной сети;
* `DALI RX GPIO Pin` - GPIO для чтения состояния DALI-шины;
* `DALI TX GPIO Pin` - GPIO для удержания DALI-трансивера в released-состоянии.

Пример локальной настройки:

```text
Example Configuration  --->
    Wi-Fi SSID = MyNetwork
    Wi-Fi password = MyPassword
    DALI RX GPIO Pin = 16
    DALI TX GPIO Pin = 17
```

Значения сохраняются в `sdkconfig`, поэтому рабочие параметры подключения могут храниться прямо в конфиге проекта.

## Фронтенд

Исходники UI находятся в `webui/`, а во firmware вшиваются уже собранные файлы из `main/web_dist/`.

Для локальной разработки фронтенда:

```bash
cd webui
npm install
npm run dev
```

Если страница открыта через `Vite` dev server, для подключения к устройству передайте IP ESP32 в query-параметре:

```text
http://localhost:5173/?ws=192.168.1.42:80
```

Чтобы пересобрать production assets для прошивки:

```bash
cd webui
npm install
npm run build
```

Команда `npm run build` обновляет встроенные файлы в `main/web_dist/`, которые затем подхватываются `idf.py build`.

## Сборка и прошивка

Соберите проект:

```bash
npm --prefix webui run build
idf.py build
```

Прошейте плату и откройте монитор порта:

```bash
idf.py -p PORT flash monitor
```

Чтобы выйти из монитора, нажмите `Ctrl-]`.

## WebSocket события

UI продолжает получать сообщения в формате:

```json
{
  "type": "message",
  "value": "DALI forward frame (16 bit): 0xA1B2"
}
```

Вместо тестового `Hello world` теперь публикуются реальные кадры DALI-шины:

* `DALI forward frame (16 bit): 0x....`
* `DALI forward frame (24 bit): 0x......`
* `DALI backward frame (8 bit): 0x..`


Для общей информации по настройке ESP-IDF см. [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html).

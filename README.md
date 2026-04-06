# Dali sniffer

После старта прошивка поднимает sniffer DALI-шины и отправляет события на шине в WebSocket UI.

## Поддерживаемая платформа

Тестировал на `ESP32-S3` но должно работать и на `ESP32-C6`.

Перед сборкой укажите целевой чип:

```bash
idf.py set-target esp32-s3
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

ESP32-S3-Pico: Uses GPIO14 for DALI RX and GPIO17 for DALI TX_i.
ESP32-C6-Pico: Uses GPIO5 for DALI RX and GPIO14 for DALI TX_i.

Пример локальной настройки:

```text
Example Configuration  --->
    Wi-Fi SSID = MyNetwork
    Wi-Fi password = MyPassword
    DALI RX GPIO Pin = 14
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

В UI публикуются кадры DALI-шины:

* `DALI forward frame (16 bit): 0x....`
* `DALI forward frame (24 bit): 0x......`
* `DALI backward frame (8 bit): 0x..`

## Управление из чата

В поле ввода UI можно отправлять команды в формате:

```text
<TARGET> -> <ACTION>
```

Поддерживаемые цели:

* `lamp <n>` - короткий адрес устройства `0..63`;
* `group <n>` - группа `0..15`;
* `all` - broadcast-команда;
* `broadcast` - то же, что `all`.

Поддерживаемые действия:

* `off`
* `on`
* `max`
* `min`
* `up`
* `down`
* `step up`
* `step down`
* `step up on`
* `step down off`
* `scene <0..15>`
* `<percent>%`
* `query status`
* `query present`
* `query failure`
* `query lamp on`
* `query level`
* `query max`
* `query min`
* `query power on`
* `query version`
* `query device type`
* `query groups`
* `query scene <0..15>`
* `add to group <0..15>`
* `remove from group <0..15>`
* `remove scene <0..15>`

Примеры:

```text
lamp 1 -> off
lamp 1 -> on
lamp 1 -> 50%
lamp 3 -> scene 4

group 2 -> max
group 2 -> step down
group 2 -> query groups

all -> off
broadcast -> query status

lamp 5 -> query level
lamp 5 -> query device type
lamp 5 -> add to group 3
lamp 5 -> remove from group 3
lamp 5 -> remove scene 2
```

После успешной отправки UI покажет подтверждение вида:

```text
Message: Sent: lamp 1 -> off
Command "lamp 1 -> off" accepted
```

Если строка не распознана или кадр не удалось отправить на шину, в чате появится сообщение об ошибке, а `command_ack` придёт с `accepted: false`.


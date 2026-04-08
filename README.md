# Dali sniffer

Этот проект превращает `ESP32` в DALI sniffer и WebSocket-консоль для локальной сети.

## Поддерживаемая платформа

Разработка велась на `ESP32-S3` но должно работать и на `ESP32-C6`.

Для работы с шиной DALI используется плата:

![Board](https://github.com/rydikov/dali-sniffer/blob/develop/docs/dali_board.jpg)

* На момент разработки плату Dali можно купить на OZON: https://ozon.ru/t/iNTS5aK
* ESP32 желательно купить с посадочнми гнезами под плату Dali (S3 или C6): https://ali.click/jkle513

Интерфейс Web приложения:
![Interface](https://github.com/rydikov/dali-sniffer/blob/develop/docs/interface.png)

После старта прошивка:

* подключается к Wi‑Fi;
* поднимает встроенный HTTP/WebSocket сервер;
* прослушивает DALI-шину и декодирует кадры;
* публикует события шины в чат браузера;
* принимает текстовые команды из UI и отправляет их обратно в DALI-шину.

Проект удобен в двух сценариях:

* пассивный мониторинг шины: смотреть, какие команды и ответы реально ходят между контроллером и устройствами;
* активное управление: отправлять команды на `lamp`, `group` или DT8-совместимые устройства прямо из строки чата в браузере;
* отправка raw команды, которую не поддерживает ваш контроллер.

В интерфейсе браузера можно:

* видеть декодированные DALI-команды и ответы;
* отправлять обычные DALI-команды вроде `lamp 1 -> off` или `group 2 -> query groups`;
* задавать уровень яркости через проценты;
* управлять DT8-параметрами, например цветовой температурой и RGB, если подключённые control gear это поддерживают.


## Сборка и запуск

Установите Espressif 5.5.4 – https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/get-started/index.html#manual-installation

### Настройка сборки проекта

Перед сборкой укажите целевой чип:

```bash
idf.py set-target esp32-s3
```

Откройте меню конфигурации:

```bash
idf.py menuconfig
```

В разделе `WiFi Configuration` доступны параметры:

* `Wi-Fi SSID` - имя беспроводной сети;
* `Wi-Fi password` - пароль беспроводной сети;
* `DALI RX GPIO Pin` - GPIO для чтения состояния DALI-шины;
* `DALI TX GPIO Pin` - GPIO для удержания DALI-трансивера в released-состоянии.

Подсказка по пинам для совместимых плат:

* ESP32-S3-Pico: Uses GPIO14 for DALI RX and GPIO17 for DALI TX_i.
* ESP32-C6-Pico: Uses GPIO5 for DALI RX and GPIO14 for DALI TX_i.

Пример локальной настройки:

```text
    Wi-Fi SSID = MyNetwork
    Wi-Fi password = MyPassword
    DALI RX GPIO Pin = 14
    DALI TX GPIO Pin = 17
```

Значения сохраняются в `sdkconfig`, поэтому рабочие параметры подключения могут храниться прямо в конфиге проекта.

### Сборка и прошивка

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

## Управление из чата

В поле ввода UI можно отправлять команды в формате:

```text
<TARGET> -> <ACTION>
```

Локальная команда UI:

* `help` - показать в чате содержимое этого раздела без отправки команды на устройство;
* `Tab` в поле ввода - автодополнить известную команду или её следующий шаблон.

Поддерживаемые цели:

* `lamp <n>` - короткий адрес устройства `0..63`;
* `group <n>` - группа `0..15`;
* `all` - broadcast-команда;
* `broadcast` - то же, что `all`;
* `raw` - low-level режим для отправки сырого forward DALI-кадра.

Для DT8-команд `ct` и `rgb` в первой версии поддерживаются только:

* `lamp <n>`
* `group <n>`

Поддерживаемые действия:

* `off` - выключить светильник или группу;
* `on` - включить на последний/текущий рабочий уровень;
* `max` - перейти на максимальный уровень яркости;
* `min` - перейти на минимальный уровень яркости;
* `up` - начать плавное увеличение яркости;
* `down` - начать плавное уменьшение яркости;
* `step up` - увеличить яркость на один шаг;
* `step down` - уменьшить яркость на один шаг;
* `step up on` - включить и увеличить яркость на шаг;
* `step down off` - уменьшить яркость на шаг с переходом к выключению;
* `scene <0..15>` - вызвать сохранённую сцену;
* `<percent>%` - установить яркость в процентах от `0` до `100`;
* `query status` - запросить статус control gear;
* `query present` - проверить наличие устройства на шине;
* `query failure` - запросить флаг неисправности лампы;
* `query lamp on` - узнать, включена ли лампа;
* `query level` - запросить текущий уровень яркости;
* `query max` - запросить сохранённый максимальный уровень;
* `query min` - запросить сохранённый минимальный уровень;
* `query power on` - запросить уровень при включении питания;
* `query version` - запросить версию DALI-команд устройства;
* `query device type` - запросить тип устройства;
* `query groups` - запросить битовую маску групп устройства;
* `query scene <0..15>` - запросить уровень, сохранённый в сцене;
* `add to group <0..15>` - добавить устройство в группу;
* `remove from group <0..15>` - удалить устройство из группы;
* `remove scene <0..15>` - удалить сохранённую сцену;
* `ct <kelvin>K` - установить цветовую температуру в Kelvin; только для DT8-совместимых control gear;
* `rgb <r> <g> <b>` - установить RGB-цвет значениями `0..255`; только для DT8-совместимых control gear;
* для `raw`: `<byte1> <byte2>` или `<byte1> <byte2> <byte3>` - отправить сырой 16- или 24-битный DALI forward frame в hex-формате.

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

lamp 1 -> ct 4000K
group 2 -> ct 2700K
lamp 5 -> rgb 255 120 0
group 3 -> rgb 0 0 255
```

Примеры `raw` с расшифровкой:

* `raw -> 03 00` - команда `OFF` для `lamp 1`;
* `raw -> 05 A0` - запрос `QUERY ACTUAL LEVEL` для `lamp 2`;
* `raw -> 85 05` - команда `RECALL MAX LEVEL` для `group 2`;
* `raw -> FF 00` - broadcast-команда `OFF`;
* `raw -> FE 80` - broadcast DAPC с уровнем `128`;
* `raw -> C1 08` - special-команда записи `DTR = 0x08`, полезно для DT8-последовательностей;
* `raw -> C3 01 80` - пример 24-битного forward frame, где `0xC3` это special opcode, а дальше идут два байта параметра.

После успешной отправки UI покажет подтверждение вида:

```text
Message: Sent: lamp 1 -> off
Command "lamp 1 -> off" accepted
```

Если строка не распознана или кадр не удалось отправить на шину, в чате появится сообщение об ошибке, а `command_ack` придёт с `accepted: false`.

Для `ct` значение вводится в Kelvin, а внутри прошивки конвертируется в DALI DT8 `mired`. Перед отправкой DT8-команд прошивка не делает предварительный `query features`, поэтому несовместимые устройства просто не отреагируют или вернут обычное поведение шины.

Команда `raw` отправляет байты в шину без проверки DALI-семантики адреса и opcode. В первой версии поддерживаются только forward-кадры длиной `16` и `24` бит, то есть ровно `2` или `3` байта. Байт можно указывать как `AA` или `0xAA`.

## MQTT

MQTT включается только если в `menuconfig` заполнен `MQTT broker address`.

В разделе `WiFi Configuration` доступны MQTT-параметры:

* `MQTT broker address` - адрес MQTT брокера;
* `MQTT custom id` - суффикс корневого topic, по умолчанию `A`.

Поддерживаемые форматы `MQTT broker address`:

* `192.168.1.50`
* `192.168.1.50:1883`
* `mqtt://192.168.1.50:1883`
* `mqtts://broker.example.com:8883`

Если схема `mqtt://` или `mqtts://` не указана, прошивка автоматически добавит `mqtt://`.

Корневой topic всегда строится в формате:

```text
dali/<custom_id>
```

Например, при `MQTT custom id = A` дерево будет таким:

```text
dali/A/status
dali/A/event/sniffer
dali/A/event/command/request
dali/A/event/command/result
dali/A/command/execute
```

Что публикуется:

* `dali/<custom_id>/status` - состояние MQTT и устройства;
* `dali/<custom_id>/event/sniffer` - все кадры, увиденные сниффером;
* `dali/<custom_id>/event/command/request` - факт приёма команды из `ws` или `mqtt`;
* `dali/<custom_id>/event/command/result` - результат исполнения команды.

Пример `status`:

```json
{
  "type": "status",
  "mqtt_enabled": true,
  "mqtt_connected": true,
  "custom_id": "A",
  "root_topic": "dali/A",
  "ip": "192.168.1.42",
  "uptime_ms": 123456
}
```

Пример события сниффера:

```json
{
  "type": "sniffer_event",
  "origin": "sniffer",
  "uptime_ms": 123456,
  "bit_length": 16,
  "is_backward_frame": false,
  "raw_hex": "0190",
  "raw_value": 400,
  "text": "DALI command short[0]: QUERY_STATUS raw=0x0190",
  "address": {
    "kind": "short",
    "value": 0,
    "label": "short[0]"
  },
  "command": "QUERY_STATUS"
}
```

В `event/sniffer` дополнительно могут появляться поля:

* `command_index` - индекс сцены/группы для индексируемых команд;
* `level` - уровень для DAPC;
* `arg` - аргумент special/input команды;
* `opcode` - opcode для generic 24-bit frame;
* `command: null` - если человекочитаемое имя команды определить не удалось.

Поле `address` содержит:

* `kind` - `short`, `group`, `broadcast`, `special`, `reply`, `unknown`;
* `value` - номер short/group адреса, `0` для `broadcast` или `null`, если численного адреса нет;
* `label` - готовая строка вроде `short[5]`, `group[2]`, `broadcast`, `special`.

Примеры событий исполнения команды:

```json
{
  "type": "command_request",
  "origin": "ws",
  "uptime_ms": 123456,
  "command_text": "lamp 1 -> off",
  "accepted": true
}
```

```json
{
  "type": "command_result",
  "origin": "mqtt",
  "uptime_ms": 123789,
  "command_text": "lamp 1 -> off",
  "accepted": true,
  "sent": true,
  "frame_count": 1,
  "feedback": "Sent: lamp 1 -> off"
}
```

Команды на исполнение принимаются через:

```text
dali/<custom_id>/command/execute
```

Payload должен быть JSON:

```json
{
  "command": "lamp 1 -> off"
}
```

Поддерживаются те же строки, что и в UI и WebSocket:

* `lamp 1 -> off`
* `group 2 -> query groups`
* `raw -> FF 00`
* `lamp 1 -> ct 4000K`

Если JSON битый, поле `command` отсутствует или очередь MQTT-команд переполнена, прошивка не отправляет кадр в шину и публикует `command_request`/`command_result` с `accepted: false`.


## Разработка фронтенда и запуск его локально

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

## WebSocket события

UI продолжает получать сообщения в формате:

```json
{
  "type": "message",
  "value": "DALI command short[0]: QUERY_STATUS raw=0x0190"
}
```

В UI публикуются кадры DALI-шины:

* `DALI command short[0]: QUERY_STATUS raw=0x0190`
* `DALI DAPC short[1] level=128 raw=0x0280`
* `DALI input cmd short[0]: ... raw=0x......`
* `DALI reply: 0x..`

Для query-команд ожидаемая последовательность выглядит так:

```text
Message: DALI command short[0]: QUERY_STATUS raw=0x0190
Message: DALI reply: 0xXX
```

Если после query reply не появился, это обычно означает, что устройство не ответило, отсутствует по этому адресу или на шине произошла коллизия/ошибка.

## Credits

Часть low-level логики приёма/передачи DALI в этом проекте опирается на идеи и структуру из:

* [DALI-Lighting-Interface](https://github.com/qqqlab/DALI-Lighting-Interface/)

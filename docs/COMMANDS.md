## Команды

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

## Также можно отправлять команды через mqtt

Корневой topic всегда строится в формате:

```text
dali/<custom_id>
```

Например, при `MQTT custom id = A` дерево будет таким:

```text
dali/A/status
dali/A/event/command/request
dali/A/event/command/result
dali/A/command/execute
```

Что публикуется:

* `dali/<custom_id>/status` - состояние MQTT и устройства;
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
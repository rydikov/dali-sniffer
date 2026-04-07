# Dali sniffer

Этот проект превращает `ESP32` в DALI sniffer и WebSocket-консоль для локальной сети.

## Поддерживаемая платформа

Разработка велась на `ESP32-S3` но должно работать и на `ESP32-C6`.

Для работы с шиной DALI используется плата:

![Board](https://github.com/rydikov/dali-sniffer/blob/develop/docs/dali_board.jpg)

На момент разработки плату Dali можно купить на OZON: https://ozon.ru/t/iNTS5aK
ESP32 желательно купить с посадочнми гнезами под плату Dali (S3 или C6): https://ali.click/jkle513

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
* активное управление: отправлять команды на `lamp`, `group` или DT8-совместимые устройства прямо из строки чата в браузере.

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

Поддерживаемые цели:

* `lamp <n>` - короткий адрес устройства `0..63`;
* `group <n>` - группа `0..15`;
* `all` - broadcast-команда;
* `broadcast` - то же, что `all`.

Для DT8-команд `ct` и `rgb` в первой версии поддерживаются только:

* `lamp <n>`
* `group <n>`

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
* `ct <kelvin>K` - только для DT8-совместимых control gear
* `rgb <r> <g> <b>` - только для DT8-совместимых control gear

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

После успешной отправки UI покажет подтверждение вида:

```text
Message: Sent: lamp 1 -> off
Command "lamp 1 -> off" accepted
```

Если строка не распознана или кадр не удалось отправить на шину, в чате появится сообщение об ошибке, а `command_ack` придёт с `accepted: false`.

Для `ct` значение вводится в Kelvin, а внутри прошивки конвертируется в DALI DT8 `mired`. Перед отправкой DT8-команд прошивка не делает предварительный `query features`, поэтому несовместимые устройства просто не отреагируют или вернут обычное поведение шины.


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
  "value": "DALI forward frame (16 bit): 0xA1B2"
}
```

В UI публикуются кадры DALI-шины:

* `DALI forward frame (16 bit): 0x....`
* `DALI forward frame (24 bit): 0x......`
* `DALI backward frame (8 bit): 0x..`

## Credits

Часть low-level логики приёма/передачи DALI в этом проекте опирается на идеи и структуру из:

* [DALI-Lighting-Interface](https://github.com/qqqlab/DALI-Lighting-Interface/)
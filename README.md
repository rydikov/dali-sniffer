# Fast Dali

Проект позволяет с помощью `ESP32` и существующей DALI сети сделать:
* Интеграцию с Home Assistant по MQTT 
* Мониторить события в шине 
* Управлять устройствами из интерфейса чата

Особенностью является – быстрая обратная связь о состоянии устройств на шине.


https://github.com/rydikov/fast-dali/blob/main/docs/demo.mp4

## Как это работает

ESP32 мониторит DALI-трафик и публикует состояние (brightness, color_temperature, rgb) устройств в MQTT.


Для получения состояния используются следующие топики:
* `dali/<custom_id>/device/<address>/brightness` - `0..254`;
* `dali/<custom_id>/group/<address>/color_temperature` - `2700..6500`;
* `dali/<custom_id>/device/<address>/rgb` - `r,g,b`, значения `0..255`;
* `dali/<custom_id>/group/<address>/white` - `w,a,f`, значения `0..255`.

Для установки состояния предназначены MQTT топики с постфиксом `_set`, которые инициируют посылку DALI команд устройствам и группам:

* `dali/<custom_id>/device/<address>/brightness_set` - payload `0..254`;
* `dali/<custom_id>/group/<address>/color_temperature_set` - payload Kelvin `2700..6500`;
* `dali/<custom_id>/device/<address>/rgb_set` - payload `r,g,b`, значения `0..255`;
* `dali/<custom_id>/group/<address>/white_set` - payload `w,a,f`, значения `0..255`.

Это позволяет использовать следующие MQTT шаблоны HA для управления:

```yaml
- unique_id: dali_device_0
  name: "DALI: Устрйство 0"
  schema: basic
  command_topic: "dali/A/device/0/brightness_set"
  # Если указать тип brightness, то всегда будет уходить 100, указываем firts
  on_command_type: first
  brightness_command_topic: "dali/A/device/0/brightness_set"
  brightness_state_topic: "dali/A/device/0/brightness"
  brightness_scale: 254
  brightness_value_template: >
    {{ value | int(0) }}
  state_topic: "dali/A/device/0/brightness"
  # Здесь указываются значения payload чтобы правильно определялось состояние on/off
  state_value_template: >
    {{ '50' if value | float(0) > 0 else '0' }}
  payload_off: "0"
  payload_on: "50"
  color_temp_command_topic: "dali/A/device/0/color_temperature_set"
  color_temp_state_topic: "dali/A/device/0/color_temperature"
  color_temp_kelvin: true
  min_kelvin: 2700
  max_kelvin: 6500
  optimistic: false
  retain: false
```

Для того чтобы это работало – необходимо добавить отслеживаемые устройства во вкладке Devices.
И указать параметры, которые будут отслеживаться.

Подробное описание синхронизации находится в [SYNC.md](docs/SYNC.md)

## Управление командами и мониторинг шины

В интерфейсе чата можно:

* Видеть декодированные DALI-команды и ответы.
* Отправлять обычные DALI-команды вроде `lamp 1 -> off` или `group 2 -> query groups`;
* Управлять DT8-параметрами, например цветовой температурой и RGB, если устройства это поддерживают.

Интерфейс чата:
![Interface](https://github.com/rydikov/fast-dali/blob/main/docs/interface.png)


## Поддерживаемая платформа ESP32

Разработка велась на `ESP32-S3` но должно работать и на `ESP32-C6`.

Для работы с шиной DALI используется плата:

![Board](https://github.com/rydikov/fast-dali/blob/main/docs/dali_board.jpg)

* На момент разработки плату Dali можно купить на OZON: https://ozon.ru/t/iNTS5aK
* ESP32 желательно купить с посадочнми гнезами под плату Dali (S3 или C6): https://ali.click/jkle513


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

В разделе `Sniffer Configuration` укажите параметры:

* `Wi-Fi SSID` - имя беспроводной сети;
* `Wi-Fi password` - пароль беспроводной сети;
* `DALI RX GPIO Pin` - GPIO для чтения состояния DALI-шины;
* `DALI TX GPIO Pin` - GPIO для удержания DALI-трансивера в released-состоянии.
* `MQTT broker address` - адрес MQTT брокера;
* `MQTT custom id` - суффикс корневого topic, по умолчанию `A`.
* `DALI status heartbeat interval` - периодический опрос `QUERY_STATUS` для сохранённых устройств, в секундах.

MQTT включается только если в `menuconfig` заполнен `MQTT broker address`. Без него будет работать только интерфейс с командами и мониторинг шины.

`DALI status heartbeat interval` по умолчанию равен `0`, поэтому периодический опрос отключён. Если указать значение больше `0`, прошивка будет с этим интервалом отправлять `QUERY_STATUS` по каждому устройству, добавленному на вкладке Devices. Ответы обрабатываются той же логикой синхронизации статуса, что и обычные DALI query.

Подсказка по пинам для совместимых плат:

* ESP32-S3-Pico: Uses GPIO14 for DALI RX and GPIO17 for DALI TX_i.
* ESP32-C6-Pico: Uses GPIO5 for DALI RX and GPIO14 for DALI TX_i.

Пример настройки для ESP32-S3-Pico:

```text
    DALI RX GPIO Pin = 14
    DALI TX GPIO Pin = 17
```

Значения сохраняются в `sdkconfig`, поэтому параметры можно указать/изменить в файле конфига проекта.

### Сборка и прошивка

Соберите проект:

```bash
idf.py build
```

Прошейте плату и откройте монитор порта:

```bash
idf.py -p PORT flash monitor
```

Чтобы выйти из монитора, нажмите `Ctrl-]`.

## Управление из чата

В чате можно отправлять команды в формате:

```text
<TARGET> -> <ACTION>
```

Подробное описание команд находится в [COMMANDS.md](docs/COMMANDS.md)

## Credits

Часть low-level логики приёма/передачи DALI в этом проекте опирается на идеи и структуру из:

* [DALI-Lighting-Interface](https://github.com/qqqlab/DALI-Lighting-Interface/)

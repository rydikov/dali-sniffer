| Supported Targets | ESP32 |
| ----------------- | ----- |

# Wi-Fi Station Example

Этот проект демонстрирует минимальное приложение для `ESP32`, которое подключается к Wi‑Fi в режиме `station` и пишет статус подключения в лог через `ESP_LOGI`.

## Поддерживаемая платформа

Поддерживается только `ESP32`.

Перед сборкой укажите целевой чип:

```bash
idf.py set-target esp32
```

## Что делает прошивка

После запуска приложение:

* инициализирует `NVS`, сетевой стек и драйвер Wi‑Fi;
* подключается к точке доступа с параметрами из `menuconfig`;
* ждёт получение IP-адреса по DHCP;
* выводит в лог успешное подключение, IP-адрес и события переподключения.

Основная логика находится в файле [main/blink_example_main.c](/Users/rydikov/Projects/HOME/TEST/hello_world/blink/main/blink_example_main.c).

## Настройка проекта

Откройте меню конфигурации:

```bash
idf.py menuconfig
```

В разделе `Example Configuration` доступен параметр:

* `Wi-Fi SSID` - имя беспроводной сети;
* `Wi-Fi password` - пароль беспроводной сети;
* `Log period in ms` - унаследованный параметр от предыдущего примера, в текущей Wi‑Fi логике не используется.

Пример локальной настройки:

```text
Example Configuration  --->
    Wi-Fi SSID = MyNetwork
    Wi-Fi password = MyPassword
```

Значения сохраняются в `sdkconfig`, поэтому рабочие параметры подключения могут храниться прямо в конфиге проекта.

## Сборка и прошивка

Соберите проект:

```bash
idf.py build
```

Прошейте плату и откройте монитор порта:

```bash
idf.py -p PORT flash monitor
```

Чтобы выйти из монитора, нажмите `Ctrl-]`.

## Пример вывода

Пример лога при успешном подключении:

```text
I (xxx) example: Initializing Wi-Fi station
I (xxx) example: Wi-Fi started, connecting to AP "MyNetwork"
I (xxx) example: Connected to AP "MyNetwork"
I (xxx) example: Got IP address: 192.168.1.42
```

## Устранение неполадок

* Если в логе нет сообщений, проверьте, что проект собран с целью `esp32`.
* Если устройство не подключается, проверьте `Wi-Fi SSID` и `Wi-Fi password` в `menuconfig`.
* Если в логе видны постоянные ретраи, убедитесь, что точка доступа доступна и использует совместимый режим безопасности.
* Если монитор не подключается, проверьте значение `PORT` и USB-подключение платы.

Для общей информации по настройке ESP-IDF см. [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html).

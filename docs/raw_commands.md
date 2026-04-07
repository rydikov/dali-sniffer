# Raw-команды на DALI-шину

Документ описывает, какие варианты `raw`-кадров принимает UI/прошивка и как они интерпретируются в текущей реализации.
Описание составлено по коду в `main/web_server.cpp`.

Дополнительный справочник по opcode взят из [`qqqDALI.h`](https://github.com/qqqlab/DALI-Lighting-Interface/blob/main/qqqDALI.h) репозитория [`qqqlab/DALI-Lighting-Interface`](https://github.com/qqqlab/DALI-Lighting-Interface).
Ниже этот каталог приведён в формате, удобном именно для `raw`-запросов.

## Базовый формат

| Формат | Что отправляется | Ограничения | Пример |
| --- | --- | --- | --- |
| `raw -> <byte1> <byte2>` | 16-битный DALI forward frame | Ровно 2 байта | `raw -> FF 00` |
| `raw -> <byte1> <byte2> <byte3>` | 24-битный DALI forward frame | Ровно 3 байта | `raw -> FF 10 00` |

Дополнительно:

- Каждый байт задаётся в hex-формате: `AA` или `0xAA`.
- Регистр не важен: `ff`, `FF`, `0xFf` работают одинаково.
- Команда `raw` не проверяет DALI-семантику адреса или opcode: если строка состоит из 2 или 3 валидных hex-байт, кадр уходит на шину как есть.

## Все варианты `raw`-кадров

| Вариант | Длина | Шаблон | Как определяется | Что это значит | Пример |
| --- | --- | --- | --- | --- | --- |
| Short DAPC | 16 бит | `<AA> <LEVEL>` | Первый байт чётный и в диапазоне `0x00..0x7E` | Прямая установка уровня яркости для short address `AA >> 1` | `raw -> 02 80` |
| Short command | 16 бит | `<AA> <CMD>` | Первый байт нечётный и в диапазоне `0x01..0x7F` | DALI-команда для short address `(AA >> 1) & 0x3F` | `raw -> 03 00` |
| Group DAPC | 16 бит | `<AA> <LEVEL>` | Первый байт чётный и в диапазоне `0x80..0x9E` | Прямая установка уровня яркости для группы `(AA >> 1) & 0x0F` | `raw -> 84 80` |
| Group command | 16 бит | `<AA> <CMD>` | Первый байт нечётный и в диапазоне `0x81..0x9F` | DALI-команда для группы `(AA >> 1) & 0x0F` | `raw -> 85 05` |
| Broadcast DAPC | 16 бит | `FE <LEVEL>` | Первый байт равен `0xFE` | Broadcast-установка уровня яркости | `raw -> FE 80` |
| Broadcast command | 16 бит | `FF <CMD>` | Первый байт равен `0xFF` | Broadcast-DALI-команда | `raw -> FF 00` |
| Special command | 16 бит | `<SC> <ARG>` | Первый байт равен одному из special opcode из таблицы ниже | Специальная DALI-команда, часто для адресации/инициализации/DTR | `raw -> C1 08` |
| 24-bit input command | 24 бита | `<ADDR> <OP> <ARG>` | Второй байт равен одному из 24-битных input opcode из таблицы ниже | 24-битная input-команда, UI попытается декодировать её по имени | `raw -> FF 10 00` |
| Generic 24-bit frame | 24 бита | `<ADDR> <OP> <ARG>` | Любой другой 3-байтовый кадр | Просто 24-битный forward frame без специальной расшифровки имени opcode | `raw -> 11 22 33` |

## Адресный байт для 16-битных raw-кадров

| Назначение | Формула | Диапазон |
| --- | --- | --- |
| Short DAPC | `AA = short_addr << 1` | `0x00..0x7E` |
| Short command | `AA = (short_addr << 1) \| 1` | `0x01..0x7F` |
| Group DAPC | `AA = 0x80 \| (group_addr << 1)` | `0x80..0x9E` |
| Group command | `AA = 0x80 \| (group_addr << 1) \| 1` | `0x81..0x9F` |
| Broadcast DAPC | `AA = 0xFE` | фиксированное значение |
| Broadcast command | `AA = 0xFF` | фиксированное значение |

Примеры:

| Raw | Расшифровка |
| --- | --- |
| `raw -> 03 00` | short address `1`, команда `OFF` |
| `raw -> 05 A0` | short address `2`, команда `QUERY ACTUAL LEVEL` |
| `raw -> 85 05` | group `2`, команда `RECALL MAX LEVEL` |
| `raw -> FF 00` | broadcast, команда `OFF` |
| `raw -> FE 80` | broadcast DAPC, уровень `128` |

## 16-битные special opcode, которые UI распознаёт по имени

| Byte 1 | Имя в UI | Типичный смысл |
| --- | --- | --- |
| `0xA1` | `TERMINATE` | завершение special-последовательности |
| `0xA3` | `DATA_TRANSFER_REGISTER0` | запись значения в `DTR0` |
| `0xA5` | `INITIALISE` | инициализация/вход в режим адресации |
| `0xA7` | `RANDOMISE` | генерация случайного long address |
| `0xA9` | `COMPARE` | сравнение поискового адреса |
| `0xAB` | `WITHDRAW` | исключение устройства из дальнейшего поиска |
| `0xAF` | `PING` | проверка активности устройства |
| `0xB1` | `SEARCHADDRH` | старший байт search address |
| `0xB3` | `SEARCHADDRM` | средний байт search address |
| `0xB5` | `SEARCHADDRL` | младший байт search address |
| `0xB7` | `PROGRAM_SHORT_ADDRESS` | запись short address |
| `0xB9` | `VERIFY_SHORT_ADDRESS` | проверка short address |
| `0xBB` | `QUERY_SHORT_ADDRESS` | чтение short address |
| `0xBD` | `PHYSICAL_SELECTION` | выбор физического устройства |
| `0xC1` | `ENABLE_DEVICE_TYPE_X` | включение device type / DT8-последовательностей |
| `0xC3` | `DATA_TRANSFER_REGISTER1` | запись значения в `DTR1` |
| `0xC5` | `DATA_TRANSFER_REGISTER2` | запись значения в `DTR2` |
| `0xC7` | `WRITE_MEMORY_LOCATION` | запись в память с ответом |
| `0xC9` | `WRITE_MEMORY_LOCATION_NO_REPLY` | запись в память без ответа |

Примеры:

| Raw | Расшифровка |
| --- | --- |
| `raw -> C1 08` | `ENABLE_DEVICE_TYPE_X arg=0x08`, используется в DT8-последовательностях |
| `raw -> A3 7F` | запись `0x7F` в `DTR0` |
| `raw -> C3 01` | запись `0x01` в `DTR1` |

## 24-битные input opcode, которые UI распознаёт по имени

| Byte 2 | Имя в UI | Типичный смысл |
| --- | --- | --- |
| `0x00` | `INPUT_INITIALISE` | инициализация input device |
| `0x01` | `INPUT_RANDOMISE` | randomise для input device |
| `0x02` | `INPUT_COMPARE` | compare для input device |
| `0x03` | `INPUT_WITHDRAW` | withdraw для input device |
| `0x04` | `INPUT_PING` | ping для input device |
| `0x05` | `INPUT_RESET` | reset input device |
| `0x06` | `INPUT_TERMINATE` | terminate input sequence |
| `0x07` | `INPUT_PROGRAM_SHORT_ADDR` | запись short address для input device |
| `0x08` | `INPUT_SEARCHADDRH` | старший байт search address |
| `0x09` | `INPUT_SEARCHADDRM` | средний байт search address |
| `0x0A` | `INPUT_SEARCHADDRL` | младший байт search address |
| `0x0B` | `INPUT_QUERY_SHORT_ADDR` | запрос short address |
| `0x10` | `INPUT_QUERY_STATUS` | запрос статуса |
| `0x3C` | `INPUT_READ_MEMORY_LOCATION` | чтение memory location |

Примеры:

| Raw | Расшифровка |
| --- | --- |
| `raw -> FF 10 00` | broadcast `INPUT_QUERY_STATUS arg=0x00` |
| `raw -> 11 05 00` | input-команда для short address `8`: `INPUT_RESET arg=0x00` |
| `raw -> C3 01 80` | UI покажет `DALI input cmd addr=0xC3: INPUT_RANDOMISE arg=0x80 raw=0xC30180`, потому что в 24-битных кадрах распознавание идёт по `byte2 = 0x01` |

Примечание: для 24-битных raw-кадров текущий декодер ориентируется только на второй байт (`opcode`). Поэтому некоторые кадры, которые вы логически воспринимаете как «special + два байта параметра», в UI могут выглядеть как `INPUT_*`.

## Что UI покажет в чате

| Raw-кадр | Типовой текст в UI |
| --- | --- |
| `raw -> 03 00` | `DALI command short[1]: OFF raw=0x0300` |
| `raw -> 85 05` | `DALI command group[2]: RECALL_MAX_LEVEL raw=0x8505` |
| `raw -> FE 80` | `DALI DAPC broadcast level=128 raw=0xFE80` |
| `raw -> C1 08` | `DALI special: ENABLE_DEVICE_TYPE_X arg=0x08 raw=0xC108` |
| `raw -> FF 10 00` | `DALI input cmd broadcast: INPUT_QUERY_STATUS arg=0x00 raw=0xFF1000` |
| `raw -> 11 22 33` | `DALI 24-bit frame: addr=0x11 opcode=0x22 param=0x33 raw=0x112233` |

## Ограничения текущей реализации

- Поддерживаются только forward frames длиной `16` и `24` бит.
- Backward frame вручную через `raw` отправить нельзя.
- Для `raw` нет встроенной валидации, что opcode действительно допустим по DALI-спецификации для данного адреса.
- Часть кадров UI умеет красиво декодировать по имени, а часть показывает как generic frame. На отправку это не влияет.

## Полный каталог 8-битных opcode для `raw -> <addr> <cmd>`

Эта секция относится к 16-битным command-кадрам вида:

```text
raw -> <address_byte> <opcode>
```

Здесь `<address_byte>` задаёт short/group/broadcast адрес, а `<opcode>` задаёт саму команду.

### 1. Direct Arc Power Control (`S = 0`)

| Opcode byte | Название | Описание |
| --- | --- | --- |
| `0x00` | `DAPC OFF` | Выключение через direct arc level `0` |
| `0x01..0xFE` | `DAPC LEVEL` | Прямое задание уровня яркости |
| `0xFF` | `DAPC MASK` | Маска / no change |

Примеры:

- `raw -> 02 80` — short `1`, direct level `128`
- `raw -> 84 40` — group `2`, direct level `64`
- `raw -> FE 00` — broadcast direct off

### 2. Базовые команды `0x00..0x1F`

| Opcode byte | Имя | Описание |
| --- | --- | --- |
| `0x00` | `OFF` | Немедленно выключить |
| `0x01` | `UP` | Увеличивать уровень 200 мс по fade rate |
| `0x02` | `DOWN` | Уменьшать уровень 200 мс по fade rate |
| `0x03` | `STEP_UP` | Шаг вверх без fade |
| `0x04` | `STEP_DOWN` | Шаг вниз без fade |
| `0x05` | `RECALL_MAX_LEVEL` | Установить максимальный уровень |
| `0x06` | `RECALL_MIN_LEVEL` | Установить минимальный уровень |
| `0x07` | `STEP_DOWN_AND_OFF` | Шаг вниз, с выключением на минимуме |
| `0x08` | `ON_AND_STEP_UP` | Включить и сделать шаг вверх |
| `0x09` | `ENABLE_DAPC_SEQUENCE` | Маркер повторной DAPC-последовательности |
| `0x0A` | `GO_TO_LAST_ACTIVE_LEVEL` | Вернуть последний активный уровень, DALI-2 |
| `0x0B..0x0F` | `RESERVED` | Зарезервировано |
| `0x10..0x1F` | `GO_TO_SCENE[n]` | Вызов сцены `n = opcode - 0x10` |

### 3. Конфигурационные команды `0x20..0x8F`

| Opcode byte | Имя | Описание |
| --- | --- | --- |
| `0x20` | `RESET` | Сброс параметров к значениям по умолчанию |
| `0x21` | `STORE_ACTUAL_LEVEL_IN_DTR0` | Сохранить текущий уровень в `DTR0` |
| `0x22` | `SAVE_PERSISTENT_VARIABLES` | Сохранить переменные в NVM, DALI-2 |
| `0x23` | `SET_OPERATING_MODE` | Установить operating mode из `DTR0`, DALI-2 |
| `0x24` | `RESET_MEMORY_BANK` | Сбросить memory bank, DALI-2 |
| `0x25` | `IDENTIFY_DEVICE` | Перевести устройство в режим идентификации, DALI-2 |
| `0x26..0x29` | `RESERVED` | Зарезервировано |
| `0x2A` | `SET_MAX_LEVEL` | Записать `DTR0` как maximum level |
| `0x2B` | `SET_MIN_LEVEL` | Записать `DTR0` как minimum level |
| `0x2C` | `SET_SYSTEM_FAILURE_LEVEL` | Записать `DTR0` как system failure level |
| `0x2D` | `SET_POWER_ON_LEVEL` | Записать `DTR0` как power-on level |
| `0x2E` | `SET_FADE_TIME` | Записать `DTR0` как fade time |
| `0x2F` | `SET_FADE_RATE` | Записать `DTR0` как fade rate |
| `0x30` | `SET_EXTENDED_FADE_TIME` | Записать extended fade time, DALI-2 |
| `0x31..0x3F` | `RESERVED` | Зарезервировано |
| `0x40..0x4F` | `SET_SCENE[n]` | Сохранить `DTR0` как сцену `n` |
| `0x50..0x5F` | `REMOVE_FROM_SCENE[n]` | Удалить сцену `n` |
| `0x60..0x6F` | `ADD_TO_GROUP[n]` | Добавить устройство в группу `n` |
| `0x70..0x7F` | `REMOVE_FROM_GROUP[n]` | Удалить устройство из группы `n` |
| `0x80` | `SET_SHORT_ADDRESS` | Записать short address из `DTR0` |
| `0x81` | `ENABLE_WRITE_MEMORY` | Разрешить запись в memory bank |
| `0x82..0x8F` | `RESERVED` | Зарезервировано |

Примечание: в каталоге `qqqlab` большинство команд из диапазона `0x20..0x80` помечены как требующие повторной отправки.

### 4. Запросы и диагностика `0x90..0xDF`

| Opcode byte | Имя | Описание |
| --- | --- | --- |
| `0x90` | `QUERY_STATUS` | Прочитать статус устройства |
| `0x91` | `QUERY_CONTROL_GEAR_PRESENT` | Проверка наличия control gear |
| `0x92` | `QUERY_LAMP_FAILURE` | Проверка отказа лампы |
| `0x93` | `QUERY_LAMP_POWER_ON` | Проверка, включена ли лампа |
| `0x94` | `QUERY_LIMIT_ERROR` | Проверка выхода уровня за допустимые пределы |
| `0x95` | `QUERY_RESET_STATE` | Проверка reset state |
| `0x96` | `QUERY_MISSING_SHORT_ADDRESS` | Проверка отсутствия short address |
| `0x97` | `QUERY_VERSION_NUMBER` | Версия стандарта / набора команд |
| `0x98` | `QUERY_CONTENT_DTR0` | Прочитать `DTR0` |
| `0x99` | `QUERY_DEVICE_TYPE` | Прочитать тип устройства |
| `0x9A` | `QUERY_PHYSICAL_MINIMUM_LEVEL` | Минимальный физический уровень |
| `0x9B` | `QUERY_POWER_FAILURE` | Проверка режима power failure |
| `0x9C` | `QUERY_CONTENT_DTR1` | Прочитать `DTR1` |
| `0x9D` | `QUERY_CONTENT_DTR2` | Прочитать `DTR2` |
| `0x9E` | `QUERY_OPERATING_MODE` | Прочитать operating mode, DALI-2 |
| `0x9F` | `QUERY_LIGHT_SOURCE_TYPE` | Тип источника света, DALI-2 |
| `0xA0` | `QUERY_ACTUAL_LEVEL` | Текущий уровень |
| `0xA1` | `QUERY_MAX_LEVEL` | Максимальный уровень |
| `0xA2` | `QUERY_MIN_LEVEL` | Минимальный уровень |
| `0xA3` | `QUERY_POWER_ON_LEVEL` | Power-on level |
| `0xA4` | `QUERY_SYSTEM_FAILURE_LEVEL` | System failure level |
| `0xA5` | `QUERY_FADE_TIME_FADE_RATE` | Fade time/fade rate |
| `0xA6` | `QUERY_MANUFACTURER_SPECIFIC_MODE` | Manufacturer specific mode, DALI-2 |
| `0xA7` | `QUERY_NEXT_DEVICE_TYPE` | Следующий device type, DALI-2 |
| `0xA8` | `QUERY_EXTENDED_FADE_TIME` | Extended fade time, DALI-2 |
| `0xA9` | `QUERY_CONTROL_GEAR_FAILURE` | Состояние отказа control gear, DALI-2 |
| `0xAA..0xAF` | `RESERVED` | Зарезервировано |
| `0xB0..0xBF` | `QUERY_SCENE_LEVEL[n]` | Прочитать уровень сцены `n` |
| `0xC0` | `QUERY_GROUPS_0_7` | Маска групп `0..7` |
| `0xC1` | `QUERY_GROUPS_8_15` | Маска групп `8..15` |
| `0xC2` | `QUERY_RANDOM_ADDRESS_H` | Старший байт random address |
| `0xC3` | `QUERY_RANDOM_ADDRESS_M` | Средний байт random address |
| `0xC4` | `QUERY_RANDOM_ADDRESS_L` | Младший байт random address |
| `0xC5` | `READ_MEMORY_LOCATION` | Чтение memory location |
| `0xC6..0xDF` | `RESERVED` | Зарезервировано |

Примеры диагностических запросов:

Примечание: в примерах ниже используется short address `1`, поэтому адресный байт команды равен `0x03`.

| Raw | Что запрашивает | Что ожидать в ответе |
| --- | --- | --- |
| `raw -> 03 90` | `QUERY_STATUS` для short `1` | backward frame со статусными битами |
| `raw -> 03 91` | `QUERY_CONTROL_GEAR_PRESENT` для short `1` | обычно `YES/NO` через значение backward frame |
| `raw -> 03 92` | `QUERY_LAMP_FAILURE` для short `1` | флаг неисправности лампы |
| `raw -> 03 93` | `QUERY_LAMP_POWER_ON` для short `1` | включена ли лампа |
| `raw -> 03 A0` | `QUERY_ACTUAL_LEVEL` для short `1` | текущий уровень яркости |
| `raw -> 03 A1` | `QUERY_MAX_LEVEL` для short `1` | максимальный уровень |
| `raw -> 03 A2` | `QUERY_MIN_LEVEL` для short `1` | минимальный уровень |
| `raw -> 03 97` | `QUERY_VERSION_NUMBER` для short `1` | номер версии стандарта/набора команд |
| `raw -> 03 99` | `QUERY_DEVICE_TYPE` для short `1` | device type |
| `raw -> 03 B4` | `QUERY_SCENE_LEVEL[4]` для short `1` | уровень, записанный в scene `4` |
| `raw -> 03 C0` | `QUERY_GROUPS_0_7` для short `1` | битовая маска групп `0..7` |
| `raw -> 03 C1` | `QUERY_GROUPS_8_15` для short `1` | битовая маска групп `8..15` |
| `raw -> 03 C2` | `QUERY_RANDOM_ADDRESS_H` для short `1` | старший байт random address |
| `raw -> 03 C3` | `QUERY_RANDOM_ADDRESS_M` для short `1` | средний байт random address |
| `raw -> 03 C4` | `QUERY_RANDOM_ADDRESS_L` для short `1` | младший байт random address |

Пример чтения memory location:

| Шаг | Raw | Назначение |
| --- | --- | --- |
| 1 | `raw -> A3 00` | записать адрес ячейки `0x00` в `DTR0` |
| 2 | `raw -> C3 01` | записать номер memory bank `0x01` в `DTR1` |
| 3 | `raw -> 03 C5` | выполнить `READ_MEMORY_LOCATION` для short `1` |

### 5. Расширения IEC 62386-207 `0xE0..0xFF`

| Opcode byte | Имя | Описание |
| --- | --- | --- |
| `0xE0` | `REFERENCE_SYSTEM_POWER` | Запустить reference system power measurement |
| `0xE1` | `ENABLE_CURRENT_PROTECTOR` | Включить current protector |
| `0xE2` | `DISABLE_CURRENT_PROTECTOR` | Выключить current protector |
| `0xE3` | `SELECT_DIMMING_CURVE` | Выбрать dimming curve |
| `0xE4` | `STORE_DTR_AS_FAST_FADE_TIME` | Записать fast fade time из `DTR` |
| `0xE5..0xEC` | `RESERVED` | Зарезервировано |
| `0xED` | `QUERY_GEAR_TYPE` | Прочитать gear type |
| `0xEE` | `QUERY_DIMMING_CURVE` | Прочитать используемую dimming curve |
| `0xEF` | `QUERY_POSSIBLE_OPERATING_MODE` | Прочитать возможные operating mode |
| `0xF0` | `QUERY_FEATURES` | Прочитать features |
| `0xF1` | `QUERY_FAILURE_STATUS` | Прочитать failure status |
| `0xF2` | `QUERY_SHORT_CIRCUIT` | Проверка short circuit |
| `0xF3` | `QUERY_OPEN_CIRCUIT` | Проверка open circuit |
| `0xF4` | `QUERY_LOAD_DECREASE` | Проверка уменьшения нагрузки |
| `0xF5` | `QUERY_LOAD_INCREASE` | Проверка увеличения нагрузки |
| `0xF6` | `QUERY_CURRENT_PROTECTOR_ACTIVE` | Активен ли current protector |
| `0xF7` | `QUERY_THERMAL_SHUTDOWN` | Проверка thermal shutdown |
| `0xF8` | `QUERY_THERMAL_OVERLOAD` | Проверка thermal overload |
| `0xF9` | `QUERY_REFERENCE_RUNNING` | Идёт ли reference measurement |
| `0xFA` | `QUERY_REFERENCE_MEASUREMENT_FAILED` | Ошибка reference measurement |
| `0xFB` | `QUERY_CURRENT_PROTECTOR_ENABLE` | Включён ли current protector |
| `0xFC` | `QUERY_OPERATING_MODE` | Operating mode из 207 |
| `0xFD` | `QUERY_FAST_FADE_TIME` | Прочитать fast fade time |
| `0xFE` | `QUERY_MIN_FAST_FADE_TIME` | Прочитать minimum fast fade time |
| `0xFF` | `QUERY_EXTENDED_VERSION_NUMBER` | Прочитать extended version |

## Полный каталог special-команд для `raw -> <special> <arg>`

Эта секция относится к 16-битным raw-кадрам, где первый байт — special opcode, а второй — аргумент:

```text
raw -> <special_opcode> <arg>
```

| Special byte | Имя | Описание |
| --- | --- | --- |
| `0xA1` | `TERMINATE` | Завершить режим initialisation |
| `0xA3` | `DATA_TRANSFER_REGISTER0` | Записать байт в `DTR0` |
| `0xA5` | `INITIALISE` | Перевести устройство в initialise state |
| `0xA7` | `RANDOMISE` | Сгенерировать random address |
| `0xA9` | `COMPARE` | Сравнить random address с search address |
| `0xAB` | `WITHDRAW` | Исключить устройство из compare-процесса |
| `0xAD` | `RESERVED` | Зарезервировано |
| `0xAF` | `PING` | Special ping, DALI-2 |
| `0xB1` | `SEARCHADDRH` | Старший байт search address |
| `0xB3` | `SEARCHADDRM` | Средний байт search address |
| `0xB5` | `SEARCHADDRL` | Младший байт search address |
| `0xB7` | `PROGRAM_SHORT_ADDRESS` | Записать short address |
| `0xB9` | `VERIFY_SHORT_ADDRESS` | Проверить short address |
| `0xBB` | `QUERY_SHORT_ADDRESS` | Прочитать short address |
| `0xBD` | `PHYSICAL_SELECTION` | Physical selection mode |
| `0xBF` | `RESERVED` | Зарезервировано |
| `0xC1` | `ENABLE_DEVICE_TYPE_X` | Включить device type / DT extension |
| `0xC3` | `DATA_TRANSFER_REGISTER1` | Записать байт в `DTR1` |
| `0xC5` | `DATA_TRANSFER_REGISTER2` | Записать байт в `DTR2` |
| `0xC7` | `WRITE_MEMORY_LOCATION` | Запись в память с ответом |
| `0xC9` | `WRITE_MEMORY_LOCATION_NO_REPLY` | Запись в память без ответа |
| `0xCB..0xFD` | `RESERVED` | Зарезервированные special opcode |

Примеры:

- `raw -> A3 7F` — записать `0x7F` в `DTR0`
- `raw -> C3 01` — записать `0x01` в `DTR1`
- `raw -> B1 12` / `raw -> B3 34` / `raw -> B5 56` — выставить search address `0x123456`
- `raw -> B7 05` — запрограммировать short address через special-команду

## Пример: инициализация шины и назначение short address устройствам

Ниже пример ручного commissioning через `raw`, когда нужно найти неадресованные устройства и раздать им short address.
Последовательность основана на special-командах из `qqqDALI.cpp` / `qqqDALI.h`.

### 1. Ввести устройства в режим инициализации

Для всех устройств без short address:

```text
raw -> A5 FF
raw -> A5 FF
```

Здесь `A5` это `INITIALISE`, а аргумент:

- `FF` — все устройства без short address
- `00` — вообще все устройства
- `0AAAAAA1` — только конкретный short address

Практически `INITIALISE` обычно отправляют дважды подряд.

### 2. Запустить random addressing

```text
raw -> A7 00
raw -> A7 00
```

Это `RANDOMISE`. После него устройствам нужны случайные long/random address.
На практике после `RANDOMISE` полезно дать шине небольшую паузу порядка `100 ms`.

### 3. Найти одно устройство через search address + compare

Дальше идёт обычный binary search по 24-битному random address.
Для каждого шага нужно выставить `SEARCHADDRH/M/L`, затем сделать `COMPARE`.

Пример для search address `0x123456`:

```text
raw -> B1 12
raw -> B3 34
raw -> B5 56
raw -> A9 00
```

Где:

- `B1` — `SEARCHADDRH`
- `B3` — `SEARCHADDRM`
- `B5` — `SEARCHADDRL`
- `A9` — `COMPARE`

Смысл `COMPARE`: если на шине есть устройство с `random_address <= search_address`, вы увидите активность/ответ на шине.
Так двоичным поиском нужно сузить диапазон до точного random address одного устройства.

### 4. Назначить short address найденному устройству

После того как устройство выбрано search/comparison-процессом, можно дать ему short address.

Формат аргумента для `PROGRAM_SHORT_ADDRESS`:

```text
arg = (short_address << 1) | 0x01
```

Примеры:

| Short address | Arg | Raw |
| --- | --- | --- |
| `0` | `0x01` | `raw -> B7 01` |
| `1` | `0x03` | `raw -> B7 03` |
| `2` | `0x05` | `raw -> B7 05` |
| `10` | `0x15` | `raw -> B7 15` |

Например, чтобы присвоить устройству short address `1`:

```text
raw -> B7 03
```

### 5. Исключить уже адресованное устройство из дальнейшего поиска

```text
raw -> AB 00
```

Это `WITHDRAW`. После него текущее найденное устройство перестаёт участвовать в следующем `COMPARE`, и можно искать следующее.

### 6. Повторить поиск для следующего устройства

Для следующего устройства повторяется цикл:

1. Выставить новый `SEARCHADDRH/M/L`
2. Проверить `COMPARE`
3. Когда найден точный random address, отправить `PROGRAM_SHORT_ADDRESS`
4. Выполнить `WITHDRAW`

Пример для второго устройства, если вы хотите дать ему short address `2`:

```text
raw -> B7 05
raw -> AB 00
```

### 7. Завершить commissioning

```text
raw -> A1 00
```

Это `TERMINATE`, который завершает режим `INITIALISE`.

### Краткая последовательность целиком

```text
raw -> A5 FF
raw -> A5 FF
raw -> A7 00
raw -> A7 00

raw -> B1 12
raw -> B3 34
raw -> B5 56
raw -> A9 00
raw -> B7 03
raw -> AB 00

raw -> A1 00
```

Этот пример показывает схему работы, а не полный автоматический поиск.
Для реального адресования нескольких устройств нужно выполнять binary search по `SEARCHADDRH/M/L` и после каждого найденного устройства делать `WITHDRAW`.

## Пример: сбросить short address у конкретного устройства

Если short address устройства уже известен и его нужно удалить, удобнее использовать обычную команду `SET_SHORT_ADDRESS` с `DTR0 = 0xFF`.

Идея такая:

1. Записать в `DTR0` значение `0xFF` (`MASK`)
2. Отправить `SET_SHORT_ADDRESS` на конкретный short address
3. Повторить `SET_SHORT_ADDRESS` второй раз в пределах `100 ms`

Для устройства с short address `1`:

```text
raw -> A3 FF
raw -> 03 80
raw -> 03 80
```

Где:

- `A3 FF` — записать `0xFF` в `DTR0`
- `03` — адресный байт для short address `1` в command-режиме
- `80` — opcode `SET_SHORT_ADDRESS`

После этого short address устройства считается удалённым.
На старом адресе оно больше не должно отвечать как обычное адресованное устройство.

Ещё несколько примеров:

| Short address | Address byte | Последовательность |
| --- | --- | --- |
| `0` | `0x01` | `raw -> A3 FF`, `raw -> 01 80`, `raw -> 01 80` |
| `1` | `0x03` | `raw -> A3 FF`, `raw -> 03 80`, `raw -> 03 80` |
| `2` | `0x05` | `raw -> A3 FF`, `raw -> 05 80`, `raw -> 05 80` |
| `10` | `0x15` | `raw -> A3 FF`, `raw -> 15 80`, `raw -> 15 80` |

Практическая проверка:

- до сброса можно проверить наличие устройства через `raw -> 03 91`
- после сброса запрос по старому адресу обычно уже не даёт нормального ответа
- дальше устройство можно снова адресовать через обычный commissioning (`INITIALISE` + `RANDOMISE` + `PROGRAM_SHORT_ADDRESS`)

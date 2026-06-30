# Raw-команды DALI

Этот документ объясняет, как отправлять DALI-команды через формат:

```text
raw -> <byte1> <byte2>
raw -> <byte1> <byte2> <byte3>
```

Он собран по двум источникам (чатом GPT и должен применяться с оценкой рисков и перепроверкой команды):

- Beckhoff KL6811 DALI command tables: https://infosys.beckhoff.com/content/1033/kl6811/2214047371.html
- `qqqDALI.h` из `qqqlab/DALI-Lighting-Interface`: https://github.com/qqqlab/DALI-Lighting-Interface/blob/main/qqqDALI.h


## 1. Как устроен `raw`

### 16-битный кадр

```text
raw -> <address_byte> <opcode_or_level>
```

- Если адресный байт указывает `S = 0`, второй байт трактуется как direct arc power level.
- Если адресный байт указывает `S = 1`, второй байт трактуется как DALI opcode.
- Если первый байт — special opcode (`0xA1`, `0xA3`, `0xA5` и т.д.), это special-команда адресации/инициализации.

### 24-битный кадр

```text
raw -> <byte1> <byte2> <byte3>
```

В текущем проекте 24-битные кадры просто отправляются как есть.
UI пытается декодировать некоторые из них как `INPUT_*`, но для отправки это не важно.

### Ограничения

- Поддерживаются только 2 или 3 байта.
- Каждый байт можно писать как `AA` или `0xAA`.
- Проверки “логически корректная ли это DALI-команда” нет: если hex-байты валидны, кадр уходит на шину.

### Термины: short, DTR, DAPC и opcode

`short` или `short address` — это короткий адрес конкретного устройства на DALI-шине.
Диапазон short address: `0..63`.

Когда в документе написано `short 1`, это значит "устройство с коротким адресом 1".

Для обычных 16-битных команд short address кодируется в первом байте так:

- DAPC для short `n`: `n << 1`
- command для short `n`: `(n << 1) | 1`

Битовая схема первого байта для обычного адресованного кадра:

```text
Y AAAAAA S
```

Где:

- `Y` — тип адреса
  - `0` = short address
  - `1` = group address или broadcast
- `AAAAAA` / `AAAA` — сами биты адреса
- `S` — тип второго байта
  - `0` = `DAPC` (во втором байте уровень яркости)
  - `1` = `command` (во втором байте opcode команды)

Поэтому у одного и того же short address получаются два соседних значения:

- short `1`, DAPC: `00000010` = `0x02`
- short `1`, command: `00000011` = `0x03`

Примеры:

- short `1` -> DAPC address byte `0x02`
- short `1` -> command address byte `0x03`
- short `2` -> command address byte `0x05`

`DTR` (`Data Transfer Register`) — это служебный регистр устройства DALI, через который передают параметры для некоторых команд.
Чаще всего используется `DTR0`, а в расширенных сценариях ещё `DTR1` и `DTR2`.

Типичный сценарий такой:

1. Сначала записать значение в `DTR`
2. Потом отправить команду, которая использует это значение

Примеры:

- `raw -> A3 64` — записать `0x64` в `DTR0`
- `raw -> 03 2A` — использовать содержимое `DTR0` как новый `MAX LEVEL` для short `1`
- `raw -> C3 01` — записать `0x01` в `DTR1`, например для `READ_MEMORY_LOCATION`

`DAPC` (`Direct Arc Power Control`) — это не “именованная команда”, а прямое задание уровня яркости вторым байтом кадра.
В таком кадре второй байт — это уровень, а не opcode.

Примеры:

- `raw -> 02 80` — установить short `1` уровень `128`
- `raw -> 84 40` — установить group `2` уровень `64`
- `raw -> FE 00` — broadcast DAPC с уровнем `0`, то есть фактически выключение через direct level

`opcode` — это код самой команды во втором байте 16-битного command-кадра.
Именно opcode определяет, что сделать: `OFF`, `QUERY STATUS`, `ADD TO GROUP`, `RESET` и так далее.

Примеры opcode:

- `0x00` — `OFF`
- `0x05` — `RECALL MAX LEVEL`
- `0x90` — `QUERY STATUS`
- `0x63` — `ADD TO GROUP 3`

Главное различие:

- в DAPC-кадре второй байт — это уровень яркости
- в command-кадре второй байт — это opcode команды

## 2. Адресный байт для обычных 16-битных команд

| Назначение | Формула | Диапазон | Пример |
| --- | --- | --- | --- |
| Short DAPC | `short << 1` | `0x00..0x7E` | short `1` -> `0x02` |
| Short command | `(short << 1) \| 1` | `0x01..0x7F` | short `1` -> `0x03` |
| Group DAPC | `0x80 \| (group << 1)` | `0x80..0x9E` | group `2` -> `0x84` |
| Group command | `0x80 \| (group << 1) \| 1` | `0x81..0x9F` | group `2` -> `0x85` |
| Broadcast DAPC | `0xFE` | фиксированное | `raw -> FE 80` |
| Broadcast command | `0xFF` | фиксированное | `raw -> FF 00` |

Быстрые примеры:

- `raw -> 03 00` — short `1`, команда `OFF`
- `raw -> 05 A0` — short `2`, команда `QUERY ACTUAL LEVEL`
- `raw -> 85 05` — group `2`, команда `RECALL MAX LEVEL`
- `raw -> FE 80` — broadcast DAPC level `128`
- `raw -> FF 00` — broadcast `OFF`

## 3. Базовые управляющие команды

Это обычные команды управления светом. Они отправляются как:

```text
raw -> <command_address_byte> <opcode>
```

Например, для short address `1` command-адресный байт будет `0x03`.

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x00` | `OFF` | Выключить свет |
| `0x01` | `UP` | Плавно увеличить уровень |
| `0x02` | `DOWN` | Плавно уменьшить уровень |
| `0x03` | `STEP UP` | Шаг вверх |
| `0x04` | `STEP DOWN` | Шаг вниз |
| `0x05` | `RECALL MAX LEVEL` | Установить максимум |
| `0x06` | `RECALL MIN LEVEL` | Установить минимум |
| `0x07` | `STEP DOWN AND OFF` | Уменьшить и выключить |
| `0x08` | `ON AND STEP UP` | Включить и увеличить |
| `0x09` | `ENABLE DAPC SEQUENCE` | Маркер DAPC-последовательности |
| `0x0A` | `GO TO LAST ACTIVE LEVEL` | Вернуть последний активный уровень |
| `0x0B..0x0F` | `RESERVED` | Зарезервировано |

Примеры:

- `raw -> 03 00` — `OFF` для short `1`
- `raw -> 03 05` — `RECALL MAX LEVEL` для short `1`
- `raw -> 85 06` — `RECALL MIN LEVEL` для group `2`

## 4. Команды сцен

### Вызов сцены

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x10..0x1F` | `GO TO SCENE 0..15` | Вызов сцены |

Формула:

```text
scene_opcode = 0x10 + scene_number
```

Примеры:

- `raw -> 03 10` — вызвать scene `0` для short `1`
- `raw -> 03 14` — вызвать scene `4` для short `1`
- `raw -> 85 1A` — вызвать scene `10` для group `2`

### Сохранить сцену из DTR

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x40..0x4F` | `STORE DTR AS SCENE 0..15` | Сохранить сцену |

### Удалить сцену

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x50..0x5F` | `REMOVE FROM SCENE 0..15` | Удалить сцену |

Примеры:

- `raw -> 03 42` — сохранить DTR как scene `2` для short `1`
- `raw -> 03 54` — удалить scene `4` для short `1`

## 5. Команды конфигурации

Это команды, которые меняют сохранённые параметры control gear.

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x20` | `RESET` | Сброс устройства |
| `0x21` | `STORE ACTUAL LEVEL IN DTR0` | Сохранить текущий уровень в `DTR0` |
| `0x22` | `SAVE PERSISTENT VARIABLES` | Сохранить переменные, DALI-2 |
| `0x23` | `SET OPERATING MODE` | Установить operating mode из `DTR0` |
| `0x24` | `RESET MEMORY BANK` | Сброс memory bank |
| `0x25` | `IDENTIFY DEVICE` | Идентификация устройства |
| `0x2A` | `STORE DTR AS MAX LEVEL` | Установить MAX |
| `0x2B` | `STORE DTR AS MIN LEVEL` | Установить MIN |
| `0x2C` | `STORE DTR AS SYSTEM FAILURE LEVEL` | Уровень при сбое |
| `0x2D` | `STORE DTR AS POWER ON LEVEL` | Уровень при включении |
| `0x2E` | `STORE DTR AS FADE TIME` | Время плавного перехода |
| `0x2F` | `STORE DTR AS FADE RATE` | Скорость изменения |
| `0x30` | `SET EXTENDED FADE TIME` | Extended fade time |
| `0x80` | `SET SHORT ADDRESS` | Записать short address из `DTR0` |
| `0x81` | `ENABLE WRITE MEMORY` | Разрешить запись в память |

Примеры:

- `raw -> 03 20` — `RESET` для short `1`
- `raw -> 03 2A` — сохранить `DTR0` как MAX LEVEL для short `1`
- `raw -> 03 80` — применить `DTR0` как short address для short `1`

Примечание: часть конфигурационных команд по стандарту отправляется дважды.

## 6. Группы

| HEX | Команда | Описание |
| --- | --- | --- |
| `0x60..0x6F` | `ADD TO GROUP 0..15` | Добавить в группу |
| `0x70..0x7F` | `REMOVE FROM GROUP 0..15` | Удалить из группы |

Формулы:

```text
add_to_group = 0x60 + group
remove_from_group = 0x70 + group
```

Примеры:

- `raw -> 03 63` — добавить short `1` в group `3`
- `raw -> 03 73` — удалить short `1` из group `3`

## 7. Запросы (основные query-команды)

| HEX | Команда | Что возвращает |
| --- | --- | --- |
| `0x90` | `QUERY STATUS` | Статус устройства |
| `0x91` | `QUERY CONTROL GEAR PRESENT` | Есть ли устройство |
| `0x92` | `QUERY LAMP FAILURE` | Ошибка лампы |
| `0x93` | `QUERY LAMP POWER ON` | Включена ли лампа |
| `0x94` | `QUERY LIMIT ERROR` | Ошибка пределов |
| `0x95` | `QUERY RESET STATE` | Был ли сброс |
| `0x96` | `QUERY MISSING SHORT ADDRESS` | Есть ли short address |
| `0x97` | `QUERY VERSION NUMBER` | Версия |
| `0x98` | `QUERY CONTENT DTR0` | Значение `DTR0` |
| `0x99` | `QUERY DEVICE TYPE` | Тип устройства |
| `0x9A` | `QUERY PHYSICAL MINIMUM LEVEL` | Минимальный физический уровень |
| `0x9B` | `QUERY POWER FAILURE` | Был ли сбой питания |
| `0x9C` | `QUERY CONTENT DTR1` | Значение `DTR1` |
| `0x9D` | `QUERY CONTENT DTR2` | Значение `DTR2` |
| `0x9E` | `QUERY OPERATING MODE` | Operating mode |
| `0x9F` | `QUERY LIGHT SOURCE TYPE` | Тип источника света |

Примеры для short `1`:

- `raw -> 03 90` — `QUERY STATUS`
- `raw -> 03 91` — `QUERY CONTROL GEAR PRESENT`
- `raw -> 03 97` — `QUERY VERSION NUMBER`
- `raw -> 03 99` — `QUERY DEVICE TYPE`

## 8. Расширенные запросы

### Уровни и параметры

| HEX | Команда | Описание |
| --- | --- | --- |
| `0xA0` | `QUERY ACTUAL LEVEL` | Текущий уровень |
| `0xA1` | `QUERY MAX LEVEL` | Максимум |
| `0xA2` | `QUERY MIN LEVEL` | Минимум |
| `0xA3` | `QUERY POWER ON LEVEL` | Уровень при включении |
| `0xA4` | `QUERY SYSTEM FAILURE LEVEL` | Уровень при сбое |
| `0xA5` | `QUERY FADE TIME / FADE RATE` | Fade time и fade rate |
| `0xA6` | `QUERY MANUFACTURER SPECIFIC MODE` | Manufacturer specific mode |
| `0xA7` | `QUERY NEXT DEVICE TYPE` | Следующий device type |
| `0xA8` | `QUERY EXTENDED FADE TIME` | Extended fade time |
| `0xA9` | `QUERY CONTROL GEAR FAILURE` | Ошибка control gear |

### Уровни сцен

| HEX | Команда | Описание |
| --- | --- | --- |
| `0xB0..0xBF` | `QUERY SCENE LEVEL 0..15` | Уровни сцен |

Пример:

- `raw -> 03 B4` — запросить уровень scene `4` у short `1`

### Группы и random address

| HEX | Команда | Описание |
| --- | --- | --- |
| `0xC0` | `QUERY GROUPS 0..7` | Маска групп `0..7` |
| `0xC1` | `QUERY GROUPS 8..15` | Маска групп `8..15` |
| `0xC2` | `QUERY RANDOM ADDRESS H` | Старший байт |
| `0xC3` | `QUERY RANDOM ADDRESS M` | Средний байт |
| `0xC4` | `QUERY RANDOM ADDRESS L` | Младший байт |
| `0xC5` | `READ MEMORY LOCATION` | Чтение памяти |

Примеры:

- `raw -> 03 C0` — запросить группы `0..7`
- `raw -> 03 C1` — запросить группы `8..15`
- `raw -> 03 C2` — запросить random address H
- `raw -> 03 C5` — прочитать memory location, если `DTR0/DTR1` уже выставлены

Пример чтения memory location:

```text
raw -> A3 00
raw -> C3 01
raw -> 03 C5
```

Где:

- `A3 00` — записать адрес ячейки `0x00` в `DTR0`
- `C3 01` — записать номер bank `0x01` в `DTR1`
- `03 C5` — выполнить `READ_MEMORY_LOCATION` для short `1`

## 9. Расширения IEC 62386-207

Эти команды относятся к расширениям control gear и не всегда поддерживаются устройством.

| HEX | Команда | Описание |
| --- | --- | --- |
| `0xE0` | `REFERENCE SYSTEM POWER` | Запустить reference measurement |
| `0xE1` | `ENABLE CURRENT PROTECTOR` | Включить current protector |
| `0xE2` | `DISABLE CURRENT PROTECTOR` | Выключить current protector |
| `0xE3` | `SELECT DIMMING CURVE` | Выбрать dimming curve |
| `0xE4` | `STORE DTR AS FAST FADE TIME` | Записать fast fade time |
| `0xED` | `QUERY GEAR TYPE` | Запрос типа gear |
| `0xEE` | `QUERY DIMMING CURVE` | Запрос dimming curve |
| `0xEF` | `QUERY POSSIBLE OPERATING MODE` | Возможные operating mode |
| `0xF0` | `QUERY FEATURES` | Возможности устройства |
| `0xF1` | `QUERY FAILURE STATUS` | Статус ошибок |
| `0xF2` | `QUERY SHORT CIRCUIT` | КЗ |
| `0xF3` | `QUERY OPEN CIRCUIT` | Обрыв |
| `0xF4` | `QUERY LOAD DECREASE` | Уменьшение нагрузки |
| `0xF5` | `QUERY LOAD INCREASE` | Увеличение нагрузки |
| `0xF6` | `QUERY CURRENT PROTECTOR ACTIVE` | Активен ли protector |
| `0xF7` | `QUERY THERMAL SHUTDOWN` | Thermal shutdown |
| `0xF8` | `QUERY THERMAL OVERLOAD` | Thermal overload |
| `0xF9` | `QUERY REFERENCE RUNNING` | Идёт ли reference measurement |
| `0xFA` | `QUERY REFERENCE MEASUREMENT FAILED` | Ошибка measurement |
| `0xFB` | `QUERY CURRENT PROTECTOR ENABLE` | Включён ли protector |
| `0xFC` | `QUERY OPERATING MODE` | Operating mode |
| `0xFD` | `QUERY FAST FADE TIME` | Fast fade time |
| `0xFE` | `QUERY MIN FAST FADE TIME` | Minimum fast fade time |
| `0xFF` | `QUERY EXTENDED VERSION NUMBER` | Extended version |

## 10. Special-команды адресации и инициализации

Это не обычные addressed command-коды. Это отдельный тип 16-битных кадров:

```text
raw -> <special_opcode> <arg>
```

| HEX | Команда | Описание |
| --- | --- | --- |
| `0xA1` | `TERMINATE` | Завершить режим инициализации |
| `0xA3` | `DATA TRANSFER REGISTER0` | Записать байт в `DTR0` |
| `0xA5` | `INITIALISE` | Начать инициализацию |
| `0xA7` | `RANDOMISE` | Случайные адреса |
| `0xA9` | `COMPARE` | Сравнение |
| `0xAB` | `WITHDRAW` | Исключить найденное устройство |
| `0xAF` | `PING` | Special ping |
| `0xB1` | `SEARCHADDRH` | Старший байт search address |
| `0xB3` | `SEARCHADDRM` | Средний байт |
| `0xB5` | `SEARCHADDRL` | Младший байт |
| `0xB7` | `PROGRAM SHORT ADDRESS` | Назначить адрес |
| `0xB9` | `VERIFY SHORT ADDRESS` | Проверка |
| `0xBB` | `QUERY SHORT ADDRESS` | Запрос адреса |
| `0xBD` | `PHYSICAL SELECTION` | Физический выбор |
| `0xC1` | `ENABLE DEVICE TYPE X` | Включить device type / DT extension |
| `0xC3` | `DATA TRANSFER REGISTER1` | Записать байт в `DTR1` |
| `0xC5` | `DATA TRANSFER REGISTER2` | Записать байт в `DTR2` |
| `0xC7` | `WRITE MEMORY LOCATION` | Запись в память с ответом |
| `0xC9` | `WRITE MEMORY LOCATION NO REPLY` | Запись в память без ответа |

Примеры:

- `raw -> A3 7F` — записать `0x7F` в `DTR0`
- `raw -> C3 01` — записать `0x01` в `DTR1`
- `raw -> C1 08` — `ENABLE DEVICE TYPE X` для DT8/спец. последовательностей

## 11. Пример commissioning: назначить short address

### Шаг 1. Ввести неадресованные устройства в initialise

```text
raw -> A5 FF
raw -> A5 FF
```

`FF` здесь означает: все устройства без short address.

### Шаг 2. Запустить random addressing

```text
raw -> A7 00
raw -> A7 00
```

После `RANDOMISE` обычно выдерживают паузу около `100 ms`.

### Шаг 3. Выставить search address и выполнить compare

Пример для `0x123456`:

```text
raw -> B1 12
raw -> B3 34
raw -> B5 56
raw -> A9 00
```

### Шаг 4. Назначить short address

Формат аргумента:

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

### Шаг 5. Исключить найденное устройство и продолжить поиск

```text
raw -> AB 00
```

### Шаг 6. Завершить commissioning

```text
raw -> A1 00
```

## 12. Пример: сбросить short address у конкретного устройства

Если адрес устройства уже известен, можно удалить short address через `DTR0 = 0xFF` и `SET_SHORT_ADDRESS`.

Для short `1`:

```text
raw -> A3 FF
raw -> 03 80
raw -> 03 80
```

Где:

- `A3 FF` — записать `0xFF` в `DTR0`
- `03` — short `1` в command-формате
- `80` — `SET_SHORT_ADDRESS`

После этого устройство обычно перестаёт отвечать по старому short address и может быть снова найдено через commissioning.

## 13. Что увидит UI

Типовые примеры декодирования в текущем проекте:

| Raw | Текст в UI |
| --- | --- |
| `raw -> 03 00` | `DALI command short[1]: OFF raw=0x0300` |
| `raw -> 85 05` | `DALI command group[2]: RECALL_MAX_LEVEL raw=0x8505` |
| `raw -> FE 80` | `DALI DAPC broadcast level=128 raw=0xFE80` |
| `raw -> C1 08` | `DALI special: ENABLE_DEVICE_TYPE_X arg=0x08 raw=0xC108` |
| `raw -> FF 10 00` | `DALI input cmd broadcast: INPUT_QUERY_STATUS arg=0x00 raw=0xFF1000` |
| `raw -> 11 22 33` | `DALI 24-bit frame: addr=0x11 opcode=0x22 param=0x33 raw=0x112233` |

## 14. Короткая памятка

- `0x00..0x0A` — базовое управление светом
- `0x10..0x1F` — вызов сцен
- `0x20..0x81` — конфигурация и сохранение параметров
- `0x60..0x7F` — работа с группами
- `0x90..0xC5` — запросы и чтение параметров
- `0xE0..0xFF` — расширения IEC 62386-207
- `0xA1`, `0xA3`, `0xA5`, ... `0xC9` в special-формате — адресация и commissioning

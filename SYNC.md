# Синхронизация состояния DALI

## Кратко

Слой синхронизации состояния слушает DALI-трафик и публикует состояние устройств в MQTT. 

Устройства создаются на вкладке "Устройства" и задаются следующие параметры:
- `Адрес`
- `Отслеживать яркость`
- `Отслеживать цветовую температуру`
- `Отслеживать RGBW`
- `Группы в которых есть устройство`
- `Сцены, в которых учавствует устройство`

Если выполняется broadcast команда для группы, то меняются параметры у всех устройств, которые входят в группу.
Если выполняется GO_TO_SCENE, то меняются параметры у всех устройств, которые входят в группу.

Если команда с явной передачей значения, например DAPC, Arc Power, то сразу устанавливается значение яркости из команды.
Для цветовой температуры отслеживается цепочка команд и она также устанавливается из параметров команд.
Для команды OFF ставится яркость 0.

Если команды без передачи параметров, то для задействованных устройств вызывается QUERY.

В основном используется обработка ответов на QUERY запросы.
Backward frame не содержит адрес устройства, поэтому прошивка запоминает цель и смысл последнего релевантного forward query, а затем интерпретирует следующий backward byte через этот pending-контекст.

`QUERY_STATUS` используется только как диагностика. Он возвращает 8-битный байт флагов состояния control gear, а не яркость, цветовую температуру или RGBW-значения, поэтому не используется как источник состояния для интерфейса.

## Реализованное Сопоставление Состояния

- `QUERY_ACTUAL_LEVEL` публикует raw DALI яркость `0..254` в `device/<address>/brightness`.
- `OFF` уже обрабатывается как команда изменения состояния и публикует яркость `0` в `device/<address>/brightness`.
- `DAPC` / `ArcPower(<address>, level)` уже обрабатывается как команда изменения состояния и публикует свой raw DALI уровень в `device/<address>/brightness`.
- `GO_TO_SCENE` уже обрабатывается через отправку follow-up запросов `QUERY_ACTUAL_LEVEL` по short address для подходящих сохранённых brightness-устройств; ответы затем публикуются как яркость.
- Последовательности опроса DT8 color temperature публикуют Kelvin в `device/<address>/color_temperature`.
- Последовательности опроса DT8 RGBW публикуют raw DALI уровни каналов `0..254` в:
  - `device/<address>/red`
  - `device/<address>/green`
  - `device/<address>/blue`
  - `device/<address>/white`

## ArcPower / DAPC

`ArcPower(<address>, level)` и `DAPC` считаются одной яркостной командой direct arc power control. Такой forward frame уже содержит целевой адрес и raw DALI уровень `0..254`, поэтому для обновления состояния не нужен backward reply.

Пример: `ArcPower(<address (control gear) 0>,221)` публикует `device/0/brightness = 221`, если устройство с адресом `0` сохранено в `/devices` и у него включён чекбокс `brightness`.

`OFF` остаётся отдельным случаем яркости `0`.

## Команды Без Явного Итогового Уровня

Некоторые brightness-команды меняют уровень, но не несут итоговое значение во фрейме. Для них прошивка сразу отправляет follow-up `QUERY_ACTUAL_LEVEL` по каждому затронутому сохранённому brightness-устройству, а MQTT обновляется только после backward reply.

Follow-up query выполняется для:

- `UP`
- `DOWN`
- `STEP_UP`
- `STEP_DOWN`
- `RECALL_MAX_LEVEL`
- `RECALL_MIN_LEVEL`
- `ON_AND_STEP_UP`
- `STEP_DOWN_AND_OFF`
- `GO_TO_LAST_ACTIVE_LEVEL`

Для short target опрашивается сохранённое устройство с этим адресом. Для group и broadcast target команда разворачивается в подходящие сохранённые устройства с capability `brightness`.

## Поток DT8 Query

- `DTR0(2)` выбирает DT8 Tc color temperature.
- Следующий backward reply на `DT8_QUERY_COLOUR_VALUE` считается старшим байтом значения mired.
- Следующий backward reply на `QUERY_CONTENT_DTR` считается младшим байтом значения mired.
- Прошивка объединяет оба байта как `mired = high << 8 | low`, конвертирует значение в Kelvin и публикует `color_temperature`.
- `DTR0(9)`, `DTR0(10)`, `DTR0(11)` и `DTR0(12)` выбирают RGBW-каналы red, green, blue и white.
- Следующий backward reply на `DT8_QUERY_COLOUR_VALUE` публикуется как значение выбранного канала.

## MQTT Интерфейс

Все payload состояния устройств являются обычными десятичными числами.

- `device/<address>/brightness`: raw DALI яркость `0..254`
- `device/<address>/color_temperature`: Kelvin
- `device/<address>/red`: raw DALI канал red `0..254`
- `device/<address>/green`: raw DALI канал green `0..254`
- `device/<address>/blue`: raw DALI канал blue `0..254`
- `device/<address>/white`: raw DALI канал white `0..254`

Group и broadcast targets разворачиваются в подходящие сохранённые устройства по capability.

## Фильтрация Устройств

Состояние фиксируется только для устройств, которые добавлены на странице `/devices` и сохранены в списке устройств. DALI-трафик от адресов, которых нет в этом списке, не создаёт MQTT state topics.

Дополнительно каждое состояние публикуется только если у сохранённого устройства включён соответствующий чекбокс capability:

- `brightness` нужен для `brightness`, включая `QUERY_ACTUAL_LEVEL`, `OFF`, `DAPC` / `ArcPower`, relative brightness-команды и follow-up после `GO_TO_SCENE`.
- `colorTemperature` нужен для `color_temperature`, включая DT8 Tc query и `DT8_SET_COLOUR_TEMP_TC`.
- `rgbw` нужен для каналов `red`, `green`, `blue` и `white`.

Для group и broadcast target прошивка разворачивает команду только в те сохранённые устройства, которые одновременно входят в target и имеют нужный capability.

## Примечания

- Brightness публикуется только для сохранённых устройств с capability `brightness`.
- Color temperature публикуется только для сохранённых устройств с capability `colorTemperature`.
- RGBW-каналы публикуются только для сохранённых устройств с capability `rgbw`.
- `QUERY_STATUS` можно позже добавить как diagnostic или health signal, но он не должен заменять value queries выше.

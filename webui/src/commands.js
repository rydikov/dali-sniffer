const TARGET_LINES = [
  '* `lamp <n>` - короткий адрес устройства `0..63`;',
  '* `group <n>` - группа `0..15`;',
  '* `all` - broadcast-команда;',
  '* `broadcast` - то же, что `all`;',
  '* `raw` - low-level режим для отправки сырого forward DALI-кадра.'
];

const DT8_LINES = [
  '* `lamp <n>`',
  '* `group <n>`'
];

const ACTION_LINES = [
  '* `off` - выключить светильник или группу;',
  '* `on` - включить на последний/текущий рабочий уровень;',
  '* `max` - перейти на максимальный уровень яркости;',
  '* `min` - перейти на минимальный уровень яркости;',
  '* `up` - начать плавное увеличение яркости;',
  '* `down` - начать плавное уменьшение яркости;',
  '* `step up` - увеличить яркость на один шаг;',
  '* `step down` - уменьшить яркость на один шаг;',
  '* `step up on` - включить и увеличить яркость на шаг;',
  '* `step down off` - уменьшить яркость на шаг с переходом к выключению;',
  '* `scene <0..15>` - вызвать сохранённую сцену;',
  '* `<percent>%` - установить яркость в процентах от `0` до `100`;',
  '* `query status` - запросить статус control gear;',
  '* `query present` - проверить наличие устройства на шине;',
  '* `query failure` - запросить флаг неисправности лампы;',
  '* `query lamp on` - узнать, включена ли лампа;',
  '* `query level` - запросить текущий уровень яркости;',
  '* `query max` - запросить сохранённый максимальный уровень;',
  '* `query min` - запросить сохранённый минимальный уровень;',
  '* `query power on` - запросить уровень при включении питания;',
  '* `query version` - запросить версию DALI-команд устройства;',
  '* `query device type` - запросить тип устройства;',
  '* `query groups` - запросить битовую маску групп устройства;',
  '* `query scene <0..15>` - запросить уровень, сохранённый в сцене;',
  '* `add to group <0..15>` - добавить устройство в группу;',
  '* `remove from group <0..15>` - удалить устройство из группы;',
  '* `remove scene <0..15>` - удалить сохранённую сцену;',
  '* `ct <kelvin>K` - установить цветовую температуру в Kelvin; только для DT8-совместимых control gear;',
  '* `rgb <r> <g> <b>` - установить RGB-цвет значениями `0..255`; только для DT8-совместимых control gear;',
  '* для `raw`: `<byte1> <byte2>` или `<byte1> <byte2> <byte3>` - отправить сырой 16- или 24-битный DALI forward frame в hex-формате.'
];

const EXAMPLE_LINES = [
  '`lamp 1 -> off`',
  '`lamp 1 -> on`',
  '`lamp 1 -> 50%`',
  '`lamp 3 -> scene 4`',
  '`group 2 -> max`',
  '`group 2 -> step down`',
  '`group 2 -> query groups`',
  '`all -> off`',
  '`broadcast -> query status`',
  '`lamp 5 -> query level`',
  '`lamp 5 -> query device type`',
  '`lamp 5 -> add to group 3`',
  '`lamp 5 -> remove from group 3`',
  '`lamp 5 -> remove scene 2`',
  '`lamp 1 -> ct 4000K`',
  '`group 2 -> ct 2700K`',
  '`lamp 5 -> rgb 255 120 0`',
  '`group 3 -> rgb 0 0 255`'
];

const RAW_EXAMPLE_LINES = [
  '* `raw -> 03 00` - команда `OFF` для `lamp 1`;',
  '* `raw -> 05 A0` - запрос `QUERY ACTUAL LEVEL` для `lamp 2`;',
  '* `raw -> 85 05` - команда `RECALL MAX LEVEL` для `group 2`;',
  '* `raw -> FF 00` - broadcast-команда `OFF`;',
  '* `raw -> FE 80` - broadcast DAPC с уровнем `128`;',
  '* `raw -> C1 08` - special-команда записи `DTR = 0x08`, полезно для DT8-последовательностей;',
  '* `raw -> C3 01 80` - пример 24-битного forward frame, где `0xC3` это special opcode, а дальше идут два байта параметра.'
];

const NOTE_LINES = [
  'После успешной отправки UI покажет подтверждение вида:',
  'Message: Sent: lamp 1 -> off',
  'Command "lamp 1 -> off" accepted',
  '',
  'Если строка не распознана или кадр не удалось отправить на шину, в чате появится сообщение об ошибке, а `command_ack` придёт с `accepted: false`.',
  '',
  'Для `ct` значение вводится в Kelvin, а внутри прошивки конвертируется в DALI DT8 `mired`.',
  'Перед отправкой DT8-команд прошивка не делает предварительный `query features`, поэтому несовместимые устройства могут не отреагировать.',
  '',
  'Команда `raw` отправляет байты в шину без проверки DALI-семантики адреса и opcode.',
  'Поддерживаются только forward-кадры длиной `16` и `24` бит, то есть ровно `2` или `3` байта.',
  'Байт можно указывать как `AA` или `0xAA`.'
];

const COMMON_ACTIONS = [
  'off',
  'on',
  'max',
  'min',
  'up',
  'down',
  'step up',
  'step down',
  'step up on',
  'step down off',
  'scene ',
  'scene 0',
  '10%',
  '50%',
  '100%',
  'query ',
  'query status',
  'query present',
  'query failure',
  'query lamp on',
  'query level',
  'query max',
  'query min',
  'query power on',
  'query version',
  'query device type',
  'query groups',
  'query scene ',
  'query scene 0',
  'add to group ',
  'add to group 0',
  'remove from group ',
  'remove from group 0',
  'remove scene ',
  'remove scene 0'
];

const DT8_ACTIONS = [
  'ct ',
  'ct 4000K',
  'rgb ',
  'rgb 255 255 255'
];

const ROOT_CANDIDATES = [
  'lamp ',
  'lamp 1 -> ',
  'group ',
  'group 1 -> ',
  'all',
  'all -> ',
  'broadcast',
  'broadcast -> ',
  'raw',
  'raw -> ',
  'raw -> FF 00',
  'help'
];

function normalizeValue(value) {
  return value.toLowerCase();
}

function findSuggestion(input, candidates) {
  const normalizedInput = normalizeValue(input);
  const exactIndex = candidates.findIndex((candidate) => normalizeValue(candidate) === normalizedInput);

  if (exactIndex >= 0) {
    for (let index = exactIndex + 1; index < candidates.length; index += 1) {
      if (normalizeValue(candidates[index]).startsWith(normalizedInput)) {
        return candidates[index];
      }
    }

    return null;
  }

  return candidates.find((candidate) => normalizeValue(candidate).startsWith(normalizedInput)) || null;
}

function buildActionCandidates(prefix, includeDt8) {
  const actions = includeDt8 ? [...COMMON_ACTIONS, ...DT8_ACTIONS] : COMMON_ACTIONS;
  return actions.map((action) => `${prefix}${action}`);
}

function buildHelpText() {
  return [
    'Управление из чата',
    '',
    'Формат команд:',
    '`<TARGET> -> <ACTION>`',
    '',
    'Поддерживаемые цели:',
    ...TARGET_LINES,
    '',
    'Для DT8-команд `ct` и `rgb` в первой версии поддерживаются только:',
    ...DT8_LINES,
    '',
    'Поддерживаемые действия:',
    ...ACTION_LINES,
    '',
    'Примеры:',
    ...EXAMPLE_LINES,
    '',
    'Примеры `raw` с расшифровкой:',
    ...RAW_EXAMPLE_LINES,
    '',
    ...NOTE_LINES,
    '',
    'Подсказка: нажмите `Tab`, чтобы автодополнить команду в поле ввода.'
  ].join('\n');
}

export function isHelpCommand(command) {
  return command.trim().toLowerCase() === 'help';
}

export function getHelpText() {
  return buildHelpText();
}

export function getAutocompleteSuggestion(input) {
  const value = input.trimStart();

  if (!value) {
    return ROOT_CANDIDATES[0];
  }

  const addressedTargetMatch = value.match(/^(lamp|group)\s+(\d+)$/i);
  if (addressedTargetMatch) {
    return `${addressedTargetMatch[1].toLowerCase()} ${addressedTargetMatch[2]} -> `;
  }

  const directSuggestion = findSuggestion(value, ROOT_CANDIDATES);
  if (directSuggestion) {
    return directSuggestion;
  }

  const rawMatch = value.match(/^raw(?:\s*->\s*)(.*)$/i);
  if (rawMatch) {
    return findSuggestion(value, ['raw -> ', 'raw -> FF 00', 'raw -> FE 80', 'raw -> C3 01 80']);
  }

  const targetMatch = value.match(/^((lamp|group)\s+(\d+)|all|broadcast)\s*->\s*(.*)$/i);
  if (!targetMatch) {
    return null;
  }

  const targetType = targetMatch[2] ? targetMatch[2].toLowerCase() : targetMatch[1].toLowerCase();
  const targetIndex = targetMatch[3];
  const actionText = targetMatch[4] || '';
  let prefix;

  if (targetType === 'lamp' || targetType === 'group') {
    prefix = `${targetType} ${targetIndex} -> `;
  } else {
    prefix = `${targetType} -> `;
  }

  const candidates = buildActionCandidates(prefix, targetType === 'lamp' || targetType === 'group');
  const actionSuggestion = findSuggestion(`${prefix}${actionText}`, candidates);

  if (actionSuggestion) {
    return actionSuggestion;
  }

  if (!actionText) {
    return `${prefix}off`;
  }

  return null;
}

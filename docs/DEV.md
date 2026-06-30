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
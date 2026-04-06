#include "dali_sniffer.h"

#include <cstddef>
#include <cinttypes>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "dali_sniffer";
// Таймер тикает заметно быстрее DALI-бита и даёт "сырые" сэмплы линии, из которых
// потом восстанавливается Manchester-код. Значение взято с запасом, чтобы можно
// было ловить небольшие отклонения таймингов на реальной шине.
constexpr uint32_t kTimerResolutionHz = 24000000;
// Период будильника GPTimer в микросекундах. Это шаг дискретизации, с которым мы
// опрашиваем линию DALI и складываем уровни в буфер до последующей декодировки.
constexpr uint32_t kTimerAlarmPeriodUs = 2500;
// Очередь нужна, чтобы WebSocket-слой мог забирать уже собранные кадры без работы
// в ISR и без блокировки sniffer-задачи.
constexpr int kEventQueueSize = 32;
// Буфер под "сырые" биты, считанные таймером. Этого хватает для стандартных
// DALI-кадров 8/16/24 бит вместе со стартом, стопом и технологическим запасом.
constexpr uint8_t kRxBufferSize = 40;

// Низкоуровневый драйвер занимается только времянкой: регулярно считывает состояние
// шины, собирает битовый поток и восстанавливает полезные DALI-биты.
class DaliSnifferDriver {
public:
    using RxCallback = void (*)(void *);

    void begin(uint8_t (*bus_is_high)(), void (*bus_set_low)(), void (*bus_set_high)())
    {
        bus_is_high_ = bus_is_high;
        bus_set_low_ = bus_set_low;
        bus_set_high_ = bus_set_high;
        set_busstate_idle();
        rxstate_ = RxState::Empty;
    }

    void set_rx_callback(RxCallback callback, void *arg)
    {
        rx_callback_ = callback;
        rx_callback_arg_ = arg;
    }

    void IRAM_ATTR timer()
    {
        // Таймер вызывается из ISR-контекста, поэтому здесь нельзя делать ничего
        // тяжёлого: только читать уровень линии и обновлять компактное состояние.
        const uint8_t bus_is_high = bus_is_high_() ? 1 : 0;

        switch (busstate_) {
        case BusState::Idle:
            // В Idle мы ждём ухода линии из состояния покоя. Для DALI это означает
            // начало нового кадра, после чего сбрасываем внутренние счётчики.
            if (bus_is_high != 0) {
                if (idle_count_ != 0xFF) {
                    idle_count_ = idle_count_ + 1;
                }
                break;
            }

            rx_pos_ = 0;
            rx_bit_count_ = 0;
            rx_idle_ = 0;
            rxstate_ = RxState::Receiving;
            busstate_ = BusState::Receiving;
            [[fallthrough]];
        case BusState::Receiving:
            // Складываем последовательные сэмплы линии в байтовый буфер. Это ещё не
            // декодированные DALI-биты, а лишь "фотография" линии во времени.
            rx_byte_ = (rx_byte_ << 1) | bus_is_high;
            rx_bit_count_ = rx_bit_count_ + 1;
            if (rx_bit_count_ == 8) {
                rx_data_[rx_pos_] = rx_byte_;
                rx_pos_ = rx_pos_ + 1;
                if (rx_pos_ > kRxBufferSize - 1) {
                    rx_pos_ = kRxBufferSize - 1;
                }
                rx_bit_count_ = 0;
            }

            if (bus_is_high != 0) {
                // Длинная серия единиц означает окончание передачи и переход шины в
                // idle. После этого помечаем кадр как завершённый и будим задачу.
                rx_idle_ = rx_idle_ + 1;
                if (rx_idle_ >= 16) {
                    rx_data_[rx_pos_] = 0xFF;
                    rx_pos_ = rx_pos_ + 1;
                    rxstate_ = RxState::Completed;
                    set_busstate_idle();

                    if (rx_callback_ != nullptr) {
                        rx_callback_(rx_callback_arg_);
                    }
                }
            } else {
                rx_idle_ = 0;
            }
            break;
        }
    }

    uint8_t rx(uint8_t *decoded_data, size_t decoded_data_size)
    {
        switch (rxstate_) {
        case RxState::Empty:
            // Кадра нет.
            return 0;
        case RxState::Receiving:
            // Кадр ещё не завершён.
            return 1;
        case RxState::Completed: {
            // После завершения кадра пробуем восстановить полезные биты из
            // Manchester-представления. Возвращаем длину уже декодированного кадра.
            rxstate_ = RxState::Empty;
            const uint16_t encoded_bit_len = static_cast<uint16_t>(rx_pos_) * 8U;
            const uint8_t decoded_length =
                manchester_decode(rx_data_, encoded_bit_len, decoded_data, decoded_data_size);
            if (decoded_length < 3) {
                // Меньше трёх бит считаем шумом/ошибкой синхронизации.
                return 2;
            }
            return decoded_length;
        }
        }

        return 0;
    }

private:
    enum class BusState : uint8_t {
        Idle = 0,
        Receiving = 1,
    };

    enum class RxState : uint8_t {
        Empty,
        Receiving,
        Completed,
    };

    void IRAM_ATTR set_busstate_idle()
    {
        // Передатчик здесь используется только для задания "отпущенного" состояния
        // линии, соответствующего покою шины для внешнего трансивера.
        bus_set_high_();
        idle_count_ = 0;
        busstate_ = BusState::Idle;
    }

    static uint8_t manchester_weight(uint8_t sample)
    {
        // Вес оценивает, насколько 8-битное окно похоже на корректный Manchester:
        // первая половина окна должна быть противоположна второй. Чем вес больше,
        // тем лучше окно совпадает с ожидаемым шаблоном перехода.
        int8_t weight = 0;
        weight += ((sample >> 7) & 1) ? 1 : -1;
        weight += ((sample >> 6) & 1) ? 2 : -2;
        weight += ((sample >> 5) & 1) ? 2 : -2;
        weight += ((sample >> 4) & 1) ? 1 : -1;
        weight -= ((sample >> 3) & 1) ? 1 : -1;
        weight -= ((sample >> 2) & 1) ? 2 : -2;
        weight -= ((sample >> 1) & 1) ? 2 : -2;
        weight -= ((sample >> 0) & 1) ? 1 : -1;

        weight *= 2;
        if (weight < 0) {
            weight = -weight + 1;
        }

        return static_cast<uint8_t>(weight);
    }

    static uint8_t sample_window(volatile uint8_t *encoded_data, uint16_t bit_pos, uint8_t *stop_or_collision)
    {
        // Собираем 8-битное окно из произвольной битовой позиции внутри сырого
        // буфера. Это позволяет чуть смещать окно влево/вправо и ловить лучший
        // центр бита при декодировании.
        const uint8_t pos = bit_pos >> 3;
        const uint8_t shift = bit_pos & 0x7;
        const uint8_t sample = (encoded_data[pos] << shift) | (encoded_data[pos + 1] >> (8 - shift));

        if (sample == 0xFF) {
            // Сплошные единицы трактуем как стоп/idle после завершения кадра.
            *stop_or_collision = 1;
        }

        if (sample == 0x00) {
            // Сплошные нули обычно означают коллизию или сильно повреждённый участок.
            *stop_or_collision = 2;
        }

        return sample;
    }

    static uint8_t manchester_decode(volatile uint8_t *encoded_data,
                                     uint16_t encoded_bit_len,
                                     uint8_t *decoded_data,
                                     size_t decoded_data_size)
    {
        // Декодер идёт по сырому битовому потоку и на каждом шаге пробует три
        // положения окна: текущее, на один сэмпл левее и на один правее. Так мы
        // подстраиваемся к реальному центру бита и лучше переносим джиттер.
        uint8_t decoded_bit_len = 0;
        uint16_t encoded_bit_pos = 1;

        while (encoded_bit_pos + 1 < encoded_bit_len) {
            uint8_t stop_or_collision = 0;
            uint8_t sample = sample_window(encoded_data, encoded_bit_pos, &stop_or_collision);
            uint8_t max_weight = manchester_weight(sample);
            uint8_t best_step = 8;

            sample = sample_window(encoded_data, encoded_bit_pos - 1, &stop_or_collision);
            uint8_t weight = manchester_weight(sample);
            if (max_weight < weight) {
                max_weight = weight;
                best_step = 7;
            }

            sample = sample_window(encoded_data, encoded_bit_pos + 1, &stop_or_collision);
            weight = manchester_weight(sample);
            if (max_weight < weight) {
                max_weight = weight;
                best_step = 9;
            }

            if (stop_or_collision == 1) {
                break;
            }

            if (stop_or_collision == 2) {
                return 0;
            }

            if (decoded_bit_len > 0) {
                // Первый стартовый бит не кладём в полезную нагрузку, а последующие
                // биты упаковываем в обычный MSB-first массив байтов.
                const size_t byte_pos = (decoded_bit_len - 1) >> 3;
                const uint8_t bit_pos = (decoded_bit_len - 1) & 0x7;
                if (byte_pos >= decoded_data_size) {
                    // На шумном сигнале декодер может увидеть слишком длинный поток.
                    // В этом случае останавливаемся до записи за пределы буфера.
                    return 0;
                }
                if (bit_pos == 0) {
                    decoded_data[byte_pos] = 0;
                }
                decoded_data[byte_pos] = (decoded_data[byte_pos] << 1) | (max_weight & 1);
            }

            decoded_bit_len++;
            encoded_bit_pos += best_step;
        }

        if (decoded_bit_len > 1) {
            // Убираем стоповый бит, чтобы снаружи осталась только длина полезного кадра:
            // 8, 16 или 24 бита.
            decoded_bit_len--;
        }

        return decoded_bit_len;
    }

    volatile BusState busstate_{BusState::Idle};
    volatile RxState rxstate_{RxState::Empty};
    volatile uint8_t idle_count_{0};
    volatile uint8_t rx_data_[kRxBufferSize]{};
    volatile uint8_t rx_pos_{0};
    volatile uint8_t rx_byte_{0};
    volatile uint8_t rx_bit_count_{0};
    volatile uint8_t rx_idle_{0};

    uint8_t (*bus_is_high_)() = nullptr;
    void (*bus_set_low_)() = nullptr;
    void (*bus_set_high_)() = nullptr;
    RxCallback rx_callback_{nullptr};
    void *rx_callback_arg_{nullptr};
};

class DaliSnifferService {
public:
    esp_err_t start()
    {
        if (started_) {
            return ESP_OK;
        }

        const gpio_num_t rx_pin = static_cast<gpio_num_t>(CONFIG_DALI_RX_PIN);
        const gpio_num_t tx_pin = static_cast<gpio_num_t>(CONFIG_DALI_TX_PIN);

        ESP_LOGI(kTag, "Configuring DALI sniffer GPIOs: RX=%d, TX=%d", rx_pin, tx_pin);

        // TX здесь не передаёт DALI-команды в обычном смысле. Он нужен, чтобы держать
        // связанный с трансивером выход в корректном логическом состоянии покоя.
        const gpio_config_t tx_config = {
            .pin_bit_mask = (1ULL << tx_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&tx_config), kTag, "Failed to configure TX pin");

        const gpio_config_t rx_config = {
            .pin_bit_mask = (1ULL << rx_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rx_config), kTag, "Failed to configure RX pin");

        // Инициализируем драйвер функциями доступа к линии и callback'ом, который
        // будет вызван, когда ISR обнаружит завершённый кадр.
        bus_set_high();
        driver_.begin(bus_is_high, bus_set_low, bus_set_high);
        driver_.set_rx_callback(rx_complete_isr, this);

        if (event_queue_ == nullptr) {
            event_queue_ = xQueueCreate(kEventQueueSize, sizeof(dali_frame_event_t));
            if (event_queue_ == nullptr) {
                ESP_LOGE(kTag, "Failed to create DALI event queue");
                return ESP_ERR_NO_MEM;
            }
        }

        // GPTimer используется как стабильный high-resolution источник опроса линии.
        gptimer_config_t timer_config = {};
        timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        timer_config.direction = GPTIMER_COUNT_UP;
        timer_config.resolution_hz = kTimerResolutionHz;
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_config, &timer_), kTag, "Failed to create GPTimer");

        const gptimer_event_callbacks_t callbacks = {
            .on_alarm = dali_timer_isr_callback,
        };
        ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(timer_, &callbacks, &driver_), kTag, "Failed to register timer callbacks");
        ESP_RETURN_ON_ERROR(gptimer_enable(timer_), kTag, "Failed to enable GPTimer");

        // Таймер работает в auto-reload режиме и периодически вызывает ISR, которая
        // скармливает очередной сэмпл линии в DaliSnifferDriver.
        const gptimer_alarm_config_t alarm_config = {
            .alarm_count = kTimerAlarmPeriodUs,
            .reload_count = 0,
            .flags = {
                .auto_reload_on_alarm = true,
            },
        };
        ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(timer_, &alarm_config), kTag, "Failed to configure GPTimer alarm");
        ESP_RETURN_ON_ERROR(gptimer_start(timer_), kTag, "Failed to start GPTimer");

        if (sniffer_task_handle_ == nullptr) {
            // Отдельная задача нужна, чтобы уже вне ISR декодировать кадр, логировать
            // его и класть в очередь для веб-сервера.
            const BaseType_t task_created = xTaskCreate(sniffer_task,
                                                        "dali_sniffer",
                                                        4096,
                                                        this,
                                                        5,
                                                        &sniffer_task_handle_);
            if (task_created != pdPASS) {
                ESP_LOGE(kTag, "Failed to create DALI sniffer task");
                return ESP_FAIL;
            }
        }

        started_ = true;
        ESP_LOGI(kTag, "DALI sniffer started");
        return ESP_OK;
    }

    QueueHandle_t event_queue() const
    {
        return event_queue_;
    }

private:
    static uint8_t IRAM_ATTR bus_is_high()
    {
        return gpio_get_level(static_cast<gpio_num_t>(CONFIG_DALI_RX_PIN));
    }

    static void IRAM_ATTR bus_set_low()
    {
        // Для конкретного интерфейса здесь логика инвертирована: "low" на шине
        // соответствует установке GPIO в 1. Это зависит от схемы трансивера.
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_DALI_TX_PIN), 1);
    }

    static void IRAM_ATTR bus_set_high()
    {
        // Аналогично, "high"/idle на шине даёт уровень 0 на локальном GPIO.
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_DALI_TX_PIN), 0);
    }

    static bool IRAM_ATTR dali_timer_isr_callback(gptimer_handle_t,
                                                  const gptimer_alarm_event_data_t *,
                                                  void *user_ctx)
    {
        auto *driver = static_cast<DaliSnifferDriver *>(user_ctx);
        driver->timer();
        return false;
    }

    static void rx_complete_isr(void *arg)
    {
        auto *self = static_cast<DaliSnifferService *>(arg);
        BaseType_t higher_priority_task_woken = pdFALSE;

        // ISR только будит задачу; вся дальнейшая обработка вынесена в task-контекст.
        if (self->sniffer_task_handle_ != nullptr) {
            vTaskNotifyGiveFromISR(self->sniffer_task_handle_, &higher_priority_task_woken);
        }

        portYIELD_FROM_ISR(higher_priority_task_woken);
    }

    static void sniffer_task(void *arg)
    {
        auto *self = static_cast<DaliSnifferService *>(arg);
        uint8_t decoded_data[4] = {};

        while (true) {
            // Ждём сигнала от ISR о том, что очередной кадр собран целиком.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            const uint8_t bit_length = self->driver_.rx(decoded_data, sizeof(decoded_data));
            if (bit_length <= 2) {
                // Шум, неполный кадр или ошибка декодирования.
                continue;
            }

            dali_frame_event_t frame = {};
            frame.length = bit_length;
            frame.is_backward_frame = (bit_length == 8);

            // Перекладываем декодированные байты в компактное представление события,
            // которое затем можно безопасно передавать между задачами.
            if (bit_length == 8) {
                frame.data = decoded_data[0];
            } else if (bit_length == 16) {
                frame.data = (static_cast<uint32_t>(decoded_data[0]) << 8) | decoded_data[1];
            } else if (bit_length == 24) {
                frame.data = (static_cast<uint32_t>(decoded_data[0]) << 16) |
                             (static_cast<uint32_t>(decoded_data[1]) << 8) |
                             decoded_data[2];
            } else {
                ESP_LOGW(kTag, "Ignoring DALI frame with unsupported length: %u", bit_length);
                continue;
            }

            ESP_LOGI(kTag,
                     "Captured DALI %s frame (%u bit): 0x%08" PRIX32,
                     frame.is_backward_frame ? "backward" : "forward",
                     frame.length,
                     frame.data);

            if (self->event_queue_ != nullptr) {
                // Не блокируемся, чтобы sniffer не зависал под нагрузкой. Если очередь
                // переполнится, кадр просто будет потерян, но приём продолжится.
                xQueueSend(self->event_queue_, &frame, 0);
            }
        }
    }

    DaliSnifferDriver driver_{};
    gptimer_handle_t timer_{nullptr};
    TaskHandle_t sniffer_task_handle_{nullptr};
    QueueHandle_t event_queue_{nullptr};
    bool started_{false};
};

DaliSnifferService &service()
{
    static DaliSnifferService instance;
    return instance;
}

}  // namespace

esp_err_t dali_sniffer_start(void)
{
    return service().start();
}

QueueHandle_t dali_sniffer_get_event_queue(void)
{
    return service().event_queue();
}

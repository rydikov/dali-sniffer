#include "dali_sniffer.h"

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
constexpr uint32_t kTimerResolutionHz = 24000000;
constexpr uint32_t kTimerAlarmPeriodUs = 2500;
constexpr int kEventQueueSize = 32;
constexpr uint8_t kRxBufferSize = 40;

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
        const uint8_t bus_is_high = bus_is_high_() ? 1 : 0;

        switch (busstate_) {
        case BusState::Idle:
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

    uint8_t rx(uint8_t *decoded_data)
    {
        switch (rxstate_) {
        case RxState::Empty:
            return 0;
        case RxState::Receiving:
            return 1;
        case RxState::Completed: {
            rxstate_ = RxState::Empty;
            const uint8_t decoded_length = manchester_decode(rx_data_, rx_pos_ * 8, decoded_data);
            if (decoded_length < 3) {
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
        bus_set_high_();
        idle_count_ = 0;
        busstate_ = BusState::Idle;
    }

    static uint8_t manchester_weight(uint8_t sample)
    {
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
        const uint8_t pos = bit_pos >> 3;
        const uint8_t shift = bit_pos & 0x7;
        const uint8_t sample = (encoded_data[pos] << shift) | (encoded_data[pos + 1] >> (8 - shift));

        if (sample == 0xFF) {
            *stop_or_collision = 1;
        }

        if (sample == 0x00) {
            *stop_or_collision = 2;
        }

        return sample;
    }

    static uint8_t manchester_decode(volatile uint8_t *encoded_data, uint8_t encoded_bit_len, uint8_t *decoded_data)
    {
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
                const uint8_t byte_pos = (decoded_bit_len - 1) >> 3;
                const uint8_t bit_pos = (decoded_bit_len - 1) & 0x7;
                if (bit_pos == 0) {
                    decoded_data[byte_pos] = 0;
                }
                decoded_data[byte_pos] = (decoded_data[byte_pos] << 1) | (max_weight & 1);
            }

            decoded_bit_len++;
            encoded_bit_pos += best_step;
        }

        if (decoded_bit_len > 1) {
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
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_DALI_TX_PIN), 1);
    }

    static void IRAM_ATTR bus_set_high()
    {
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
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            const uint8_t bit_length = self->driver_.rx(decoded_data);
            if (bit_length <= 2) {
                continue;
            }

            dali_frame_event_t frame = {};
            frame.length = bit_length;
            frame.is_backward_frame = (bit_length == 8);

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

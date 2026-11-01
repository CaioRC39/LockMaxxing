/* =============================================================================
 * main/actuators.c — Servo e buzzer (LED visual delegado ao WS2812)
 * ========================================================================== */

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "led_status.h"
#include "actuators.h"

static const char *TAG = CONFIG_LOG_TAG_ACTUATOR;

static bool g_initialized = false;

/* ============================================================================
 * Helpers
 * ========================================================================== */

static esp_err_t gpio_write(int pin, bool on, int active_level)
{
    return gpio_set_level((gpio_num_t)pin, on ? active_level : !active_level);
}

static uint32_t angle_to_duty(uint8_t degrees)
{
    if (degrees > 180) degrees = 180;
    uint32_t range = CONFIG_SERVO_DUTY_MAX - CONFIG_SERVO_DUTY_MIN;
    return CONFIG_SERVO_DUTY_MIN + ((uint32_t)degrees * range / 180U);
}

static esp_err_t servo_set_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                  CONFIG_SERVO_CHANNEL, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_SERVO_CHANNEL);
}

/* ============================================================================
 * Init / self-test
 * ========================================================================== */

esp_err_t actuators_init(void)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "actuators_init() chamada novamente — ignorando");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Inicializando atuadores (buzzer + servo; LED via WS2812)");

    /* -------------------------------------------------------------------------
     * Buzzer — saída digital
     * ---------------------------------------------------------------------- */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_GPIO_BUZZER),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(buzzer) falhou: %s", esp_err_to_name(err));
        return err;
    }
    gpio_write(CONFIG_GPIO_BUZZER, false, CONFIG_LEVEL_BUZZER_ON);

    /* -------------------------------------------------------------------------
     * Servo — timer e canal LEDC (canal 1, câmera usa canal 0)
     * ---------------------------------------------------------------------- */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = CONFIG_SERVO_TIMER_RES,
        .timer_num       = CONFIG_SERVO_TIMER,
        .freq_hz         = CONFIG_SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config(servo) falhou: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = CONFIG_GPIO_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = CONFIG_SERVO_CHANNEL,
        .timer_sel  = CONFIG_SERVO_TIMER,
        .duty       = angle_to_duty(0),
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config(servo) falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    servo_set_duty(angle_to_duty(0));
    vTaskDelay(pdMS_TO_TICKS(400));

    g_initialized = true;
    ESP_LOGI(TAG, "Atuadores prontos: buzzer=GPIO%d, servo=GPIO%d "
                  "(timer=%d, canal=%d)",
             CONFIG_GPIO_BUZZER, CONFIG_GPIO_SERVO,
             CONFIG_SERVO_TIMER, CONFIG_SERVO_CHANNEL);
    return ESP_OK;
}

esp_err_t actuators_self_test(void)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    /* Feedback visual via WS2812 (led_status task precisa estar ativa) */
    ESP_LOGI(TAG, "Self-test: verde (WS2812)");
    led_status_set_raw(0, 200, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Self-test: vermelho (WS2812)");
    led_status_set_raw(200, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    led_status_set_raw(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Self-test: buzzer");
    actuators_set_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    actuators_set_buzzer(false);

    ESP_LOGI(TAG, "Self-test: servo");
    servo_set_duty(angle_to_duty(180));
    vTaskDelay(pdMS_TO_TICKS(300));
    servo_set_duty(angle_to_duty(0));
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Self-test concluído");
    return ESP_OK;
}

/* ============================================================================
 * Sequências de resultado
 * ============================================================================
 * NOTA: o LED WS2812 é controlado por tasks.c em on_enter_state(APPLYING),
 * que já conhece o resultado (pending_apply_approved). Esta função não toca
 * no LED — só servo e buzzer.
 * ========================================================================== */

esp_err_t actuators_apply_result(actuator_result_t result)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    switch (result) {
    case ACTUATOR_RESULT_APPROVED:
        ESP_LOGI(TAG, "APROVADO: servo sweep");

        servo_set_duty(CONFIG_SERVO_DUTY_APPROVED);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));

        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_HOLD_MS));

        servo_set_duty(angle_to_duty(0));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));

        ESP_LOGI(TAG, "Aprovação concluída");
        return ESP_OK;

    case ACTUATOR_RESULT_REJECTED:
        ESP_LOGI(TAG, "REPROVADO: buzzer");

        actuators_set_buzzer(true);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BUZZER_DURATION_MS));
        actuators_set_buzzer(false);

        ESP_LOGI(TAG, "Reprovação concluída");
        return ESP_OK;

    default:
        ESP_LOGE(TAG, "Resultado inválido: %d", (int)result);
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t actuators_all_off(void)
{
    gpio_write(CONFIG_GPIO_BUZZER, false, CONFIG_LEVEL_BUZZER_ON);

    if (g_initialized) {
        servo_set_duty(angle_to_duty(0));
    }

    /* Reseta o WS2812 para preto. A SM deve transicionar para IDLE logo
     * em seguida (via sm_force_idle), que chamará led_status_set(IDLE)
     * e restaurará a cor de estado. */
    led_status_set_raw(0, 0, 0);

    return ESP_OK;
}

/* ============================================================================
 * Setters individuais
 * ========================================================================== */

esp_err_t actuators_set_buzzer(bool on)
{
    return gpio_write(CONFIG_GPIO_BUZZER, on, CONFIG_LEVEL_BUZZER_ON);
}

esp_err_t actuators_set_servo_angle(uint8_t degrees)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    return servo_set_duty(angle_to_duty(degrees));
}
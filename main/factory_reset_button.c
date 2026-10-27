/* =============================================================================
 * main/factory_reset_button.c — Botão físico de reset de fábrica
 * ========================================================================== */

#include <stdbool.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "app_config.h"
#include "factory_reset_button.h"

static const char *TAG = "BTN_RESET";

/* ============================================================================
 * Configuração
 * ========================================================================== */

/* Botão BOOT da N16R8 (GPIO0). Strapping durante reset, GPIO normal após. */
#define FACTORY_RESET_BUTTON_GPIO       0

/* Tempo para o sistema estabilizar antes de começar a monitorar.
 * Necessário para não confundir o estado de strapping do GPIO0 no boot. */
#define FACTORY_RESET_BOOT_DELAY_MS     5000

/* Intervalo de polling do botão (debounce natural) */
#define FACTORY_RESET_POLL_MS           50

/* Duração mínima de pressão para acionar o reset */
#ifndef CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS
#define CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS  5000
#endif

/* Stack e prioridade da task (baixa prioridade, ela é ociosa 99% do tempo) */
#define FACTORY_RESET_TASK_STACK        3072
#define FACTORY_RESET_TASK_PRIO         1

/* ============================================================================
 * Task principal
 * ========================================================================== */

static void factory_reset_button_task(void *arg)
{
    (void)arg;

    /* --- 1) Configura o GPIO como entrada com pull-up --- */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FACTORY_RESET_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* botão aterra quando pressionado */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config falhou: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* --- 2) Espera o sistema estabilizar ---
     * Durante o boot, GPIO0 é strapping pin. Se o usuário o segurou no
     * power-on, a placa teria entrado em modo download — mas como estamos
     * rodando app_main, sabemos que isso não aconteceu. Após o chip ter
     * bootado com sucesso, o GPIO0 é liberado para uso como entrada normal.
     *
     * Mesmo assim, esperamos 5 s para garantir que o Wi-Fi, MQTT e outras
     * tasks já tenham começado (contexto estável). */
    ESP_LOGI(TAG, "Botão de reset de fábrica ativo após %.1f s de boot",
             FACTORY_RESET_BOOT_DELAY_MS / 1000.0f);
    vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_BOOT_DELAY_MS));

    ESP_LOGI(TAG, "Monitorando GPIO%d — segure por %d s para reset de fábrica",
             FACTORY_RESET_BUTTON_GPIO,
             CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS / 1000);

    /* --- 3) Loop de monitoramento --- */
    bool       was_pressed          = false;
    TickType_t press_start          = 0;
    int        last_logged_threshold = 0;

    while (1) {
        int  level   = gpio_get_level(FACTORY_RESET_BUTTON_GPIO);
        bool pressed = (level == 0);   /* pull-up: pressionado = LOW */

        /* --- Borda de descida: botão acabou de ser pressionado --- */
        if (pressed && !was_pressed) {
            press_start           = xTaskGetTickCount();
            last_logged_threshold = 0;
            ESP_LOGI(TAG, "Botão pressionado — mantenha por %d s",
                     CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS / 1000);
        }

        /* --- Borda de subida: botão foi solto --- */
        if (!pressed && was_pressed) {
            TickType_t held = xTaskGetTickCount() - press_start;
            TickType_t needed = pdMS_TO_TICKS(CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS);
            if (held < needed) {
                uint32_t held_ms = (uint32_t)(held * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "Botão solto após %u ms — cancelado",
                         (unsigned)held_ms);
            }
            last_logged_threshold = 0;
        }

        /* --- Se está pressionado, verifica progresso --- */
        if (pressed) {
            TickType_t held   = xTaskGetTickCount() - press_start;
            TickType_t needed = pdMS_TO_TICKS(CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS);

            /* Logs de progresso em 25%, 50%, 75% */
            int pct = needed > 0 ? (int)((held * 100U) / needed) : 100;
            if (pct >= 25 && last_logged_threshold < 25) {
                ESP_LOGW(TAG, "  ... 25%% (%d s restantes)",
                         (int)((needed - held) * portTICK_PERIOD_MS / 1000));
                last_logged_threshold = 25;
            }
            if (pct >= 50 && last_logged_threshold < 50) {
                ESP_LOGW(TAG, "  ... 50%% (%d s restantes)",
                         (int)((needed - held) * portTICK_PERIOD_MS / 1000));
                last_logged_threshold = 50;
            }
            if (pct >= 75 && last_logged_threshold < 75) {
                ESP_LOGW(TAG, "  ... 75%% (%d s restantes)",
                         (int)((needed - held) * portTICK_PERIOD_MS / 1000));
                last_logged_threshold = 75;
            }

            /* --- Tempo atingido: dispara reset --- */
            if (held >= needed) {
                ESP_LOGW(TAG, "==========================================");
                ESP_LOGW(TAG, " RESET DE FÁBRICA ACIONADO");
                ESP_LOGW(TAG, " Apagando NVS e reiniciando...");
                ESP_LOGW(TAG, "==========================================");

                esp_err_t er = app_config_erase();
                if (er == ESP_OK) {
                    /* Sucesso: reboot em 500 ms (dá tempo de logar) */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                    /* não retorna */
                } else {
                    ESP_LOGE(TAG, "app_config_erase() falhou: %s — cancelando",
                             esp_err_to_name(er));

                    /* Espera o usuário soltar, para evitar retry imediato
                     * em loop enquanto o botão ainda está pressionado. */
                    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    last_logged_threshold = 0;
                }
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_POLL_MS));
    }
}

/* ============================================================================
 * API pública
 * ========================================================================== */

esp_err_t factory_reset_button_init(void)
{
    BaseType_t ok = xTaskCreate(
        factory_reset_button_task,
        "btn_reset",
        FACTORY_RESET_TASK_STACK / sizeof(StackType_t),
        NULL,
        FACTORY_RESET_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
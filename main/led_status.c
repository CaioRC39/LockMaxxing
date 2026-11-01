/* =============================================================================
 * main/led_status.c — LED de status WS2812 (GPIO 48)
 * ========================================================================== */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portmacro.h"

#include "sdkconfig.h"
#include "config.h"
#include "led_status.h"

static const char *TAG = "LED";

/* ============================================================================
 * Tipos internos
 * ========================================================================== */

typedef struct {
    uint8_t r, g, b;
} led_rgb_t;

/* Padrão de pisca: período total + fração do período com LED aceso.
 * Ex: {1000, 0.5} = 500 ms aceso, 500 ms apagado, ciclo de 1 s.
 * Fração 1.0 = aceso fixo. Fração 0.0 = apagado. */
typedef struct {
    led_rgb_t color;
    uint16_t  period_ms;
    float     on_fraction;
} led_pattern_t;

/* ============================================================================
 * Estado compartilhado (escrito por qualquer task, lido pela task LED)
 * ========================================================================== */

static portMUX_TYPE       s_led_lock = portMUX_INITIALIZER_UNLOCKED;
static sm_state_t         s_current_state = SM_STATE_INIT;
static bool               s_ota_active    = false;
static bool               s_raw_override  = false;
static led_rgb_t          s_raw_color     = {0, 0, 0};

static led_strip_handle_t s_strip = NULL;

/* ============================================================================
 * Inicialização do driver
 * ========================================================================== */

static esp_err_t strip_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num              = CONFIG_LED_WS2812_GPIO,
        .max_leds                    = CONFIG_LED_WS2812_COUNT,
        .led_model                   = LED_MODEL_WS2812,
        .color_component_format      = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,   /* 10 MHz = 100 ns/tick */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,                   /* 1 LED: DMA desnecessário */
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Estado inicial: apagado */
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "LED WS2812 inicializado (GPIO %d, RMT 10 MHz)",
             CONFIG_LED_WS2812_GPIO);
    return ESP_OK;
}

/* ============================================================================
 * Mapeamento estado → padrão
 * ========================================================================== */

static led_pattern_t pattern_for_state(sm_state_t state)
{
    switch (state) {
    case SM_STATE_INIT:
        /* Azul piscando devagar: aguardando rede */
        return (led_pattern_t){ {0, 0, 180}, 1000, 0.5f };

    case SM_STATE_IDLE:
        /* Verde fixo em brilho baixo: pronto, ocioso */
        return (led_pattern_t){ {0, 180, 0}, 1000, 1.0f };

    case SM_STATE_CAPTURING:
        /* Azul piscando rápido: câmera ativa */
        return (led_pattern_t){ {0, 0, 180}, 300, 0.5f };

    case SM_STATE_SENDING:
        /* Amarelo piscando médio: enviando dados */
        return (led_pattern_t){ {180, 180, 0}, 500, 0.6f };

    case SM_STATE_WAITING:
        /* Ciano pulsando lento: aguardando servidor */
        return (led_pattern_t){ {0, 180, 180}, 1200, 0.3f };

    case SM_STATE_APPLYING:
        /* Cor depende do resultado — aproximação: verde se aprovado
         * recente, vermelho se reprovado. O actuator_task já sinaliza
         * via LED físico; aqui usamos verde fixo como "processando". */
        return (led_pattern_t){ {0, 180, 0}, 1000, 1.0f };

    case SM_STATE_ERROR:
        /* Vermelho piscando: falha recuperável */
        return (led_pattern_t){ {180, 0, 0}, 500, 0.5f };

    default:
        return (led_pattern_t){ {0, 0, 0}, 1000, 0.0f };
    }
}

/* ============================================================================
 * Task de animação
 * ========================================================================== */

#define LED_TICK_MS  50

static void led_task(void *arg)
{
    (void)arg;
    TickType_t start = xTaskGetTickCount();

    while (1) {
        /* Snapshot do estado atual (protegido por spinlock) */
        portENTER_CRITICAL(&s_led_lock);
        sm_state_t  st        = s_current_state;
        bool        ota       = s_ota_active;
        bool        raw       = s_raw_override;
        led_rgb_t   raw_color = s_raw_color;
        portEXIT_CRITICAL(&s_led_lock);

        /* Escolha do padrão */
        led_pattern_t p;
        if (raw) {
            p = (led_pattern_t){ raw_color, 1000, 1.0f };
        } else if (ota) {
            p = (led_pattern_t){ {120, 0, 180}, 150, 0.5f };  /* roxo rápido */
        } else {
            p = pattern_for_state(st);
        }

        /* Calcula se o LED deve estar aceso neste instante */
        bool on;
        if (p.on_fraction >= 0.99f) {
            on = true;
        } else if (p.on_fraction <= 0.01f) {
            on = false;
        } else if (p.period_ms == 0) {
            on = true;
        } else {
            TickType_t now      = xTaskGetTickCount();
            uint32_t elapsed_ms = (uint32_t)((now - start) * portTICK_PERIOD_MS);
            uint32_t phase_ms   = elapsed_ms % p.period_ms;
            uint32_t on_ms      = (uint32_t)(p.period_ms * p.on_fraction);
            on = (phase_ms < on_ms);
        }

        /* Aplica no LED */
        if (on) {
            /* Brilho global escala cada componente */
            uint32_t scale = CONFIG_LOCKMAXXING_LED_BRIGHTNESS;
            uint32_t r = ((uint32_t)p.color.r * scale) / 255;
            uint32_t g = ((uint32_t)p.color.g * scale) / 255;
            uint32_t b = ((uint32_t)p.color.b * scale) / 255;

            esp_err_t err = led_strip_set_pixel(s_strip, 0, r, g, b);
            if (err == ESP_OK) {
                led_strip_refresh(s_strip);
            }
        } else {
            led_strip_clear(s_strip);
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

/* ============================================================================
 * API pública
 * ========================================================================== */

esp_err_t led_status_init(void)
{
#if !CONFIG_LOCKMAXXING_LED_ENABLED
    ESP_LOGW(TAG, "LED de status desabilitado via Kconfig");
    return ESP_OK;
#else
    esp_err_t err = strip_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(
        led_task,
        "led_status",
        2048 / sizeof(StackType_t),   /* stack pequena — só math e RMT */
        NULL,
        1,                             /* prioridade mínima (acima de IDLE) */
        NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#endif
}

void led_status_set(sm_state_t state)
{
    portENTER_CRITICAL(&s_led_lock);
    s_current_state = state;
    s_raw_override  = false;   /* limpa override ao voltar ao normal */
    portEXIT_CRITICAL(&s_led_lock);
}

void led_status_set_ota_active(bool active)
{
    portENTER_CRITICAL(&s_led_lock);
    s_ota_active = active;
    portEXIT_CRITICAL(&s_led_lock);
}

void led_status_set_raw(uint8_t r, uint8_t g, uint8_t b)
{
    portENTER_CRITICAL(&s_led_lock);
    s_raw_override = true;
    s_raw_color    = (led_rgb_t){ r, g, b };
    portEXIT_CRITICAL(&s_led_lock);
}
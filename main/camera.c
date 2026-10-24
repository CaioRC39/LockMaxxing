/* =============================================================================
 * main/camera.c — Driver da câmera OV5640
 * =============================================================================
 *
 * Sensor alvo: OV5640 (5 MP) na ESP32-S3 CAM N16R8.
 * A pinagem DVP é a mesma do OV3660 (config.h, bloco 4).
 *
 * Encapsula o componente esp32-camera. O resto do firmware não precisa
 * incluir esp_camera.h — só camera.h.
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * ORDEM IMPORTANTE: esp_camera.h antes de config.h, porque o config.h
 * usa macros definidas nesse header (FRAMESIZE_*, PIXFORMAT_*,
 * CAMERA_GRAB_*, CAMERA_FB_IN_PSRAM). Inverter causa "macro não definida".
 */
#include "esp_camera.h"

#include "config.h"
#include "camera.h"

static const char *TAG = CONFIG_LOG_TAG_CAMERA;

/* -----------------------------------------------------------------------------
 * Estado interno
 * -------------------------------------------------------------------------- */

static bool          g_initialized  = false;
static uint16_t      g_sensor_pid   = 0;
static const char   *g_sensor_name  = "desconhecido";

/* -----------------------------------------------------------------------------
 * Detecção de sensor
 * -------------------------------------------------------------------------- */

/*
 * Mapeia o PID lido do sensor para um nome legível. Os PIDs são os
 * "Product ID" dos sensores Omnivision, como aparecem no datasheet e
 * no source do esp32-camera.
 *
 * OV2640 é uma exceção: retorna PID 0x26 em algumas versões e 0x2640
 * em outras, dependendo do módulo. Aceitamos os dois.
 */
static void detect_sensor(sensor_t *s)
{
    g_sensor_pid = s->id.PID;

    switch (g_sensor_pid) {
        case 0x26:
        case 0x2640:
        case 0x2642:
            g_sensor_name = "OV2640";
            break;
        case 0x3660:
            g_sensor_name = "OV3660";
            break;
        case 0x5640:
            g_sensor_name = "OV5640";
            break;
        case 0x7670:
            g_sensor_name = "OV7670";
            break;
        case 0x7725:
            g_sensor_name = "OV7725";
            break;
        default:
            g_sensor_name = "desconhecido";
            break;
    }

    ESP_LOGI(TAG, "Sensor detectado: %s (PID=0x%04X, VER=0x%04X)",
             g_sensor_name, g_sensor_pid, s->id.VER);
}

/* -----------------------------------------------------------------------------
 * Ajustes específicos por sensor
 * -------------------------------------------------------------------------- */

/*
 * Aplicado UMA vez, logo após esp_camera_init(). Não bloqueia; só chama
 * os callbacks do sensor_t que escrevem nos registradores I2C.
 *
 * OV5640: na maioria dos módulos da N16R8, o sensor vem montado com a
 * imagem de cabeça para baixo — daí o set_vflip(1). Se a sua imagem
 * sair correta sem flip, troque para 0. Se sair espelhada, use também
 * set_hmirror(1).
 *
 * OV3660 e OV2640: em geral não precisam de flip. Se precisarem,
 * adicione um case aqui.
 */
static void apply_sensor_specific_config(sensor_t *s)
{
    if (g_sensor_pid == 0x5640) {
        /* OV5640 na N16R8 — flip vertical para orientação correta. */
        int ret_v = s->set_vflip(s, 1);
        ESP_LOGI(TAG, "OV5640: set_vflip(1) = %d", ret_v);

        /*
         * Neutraliza brilho/contraste/saturação. Os valores (0) são os
         * padrões do driver. Ajustar aqui se a imagem ficar muito clara,
         * escura ou dessaturada na sua iluminação.
         */
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
    }
}

/* -----------------------------------------------------------------------------
 * Init / deinit
 * -------------------------------------------------------------------------- */

esp_err_t camera_init(void)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "camera_init() chamada novamente — ignorando");
        return ESP_OK;
    }

    /* PSRAM é obrigatória. Sem ela, os framebuffers da câmera não cabem
     * no heap interno (~320 KB) e o init falha silenciosamente de um jeito
     * difícil de depurar. Falha antecipada com mensagem explícita. */
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM não inicializada — impossível alocar framebuffers");
        ESP_LOGE(TAG, "Verifique em sdkconfig.defaults:");
        ESP_LOGE(TAG, "  CONFIG_SPIRAM=y");
        ESP_LOGE(TAG, "  CONFIG_SPIRAM_MODE_OCT=y   (N16R8 usa PSRAM octal)");
        return ESP_ERR_NO_MEM;
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "Inicializando câmera (PSRAM total: %u bytes)",
             (unsigned)psram_size);

    /*
     * camera_config_t é definida em esp_camera.h. Os campos abaixo mapeiam
     * quase 1:1 para o que está em config.h. A única decisão local é o
     * canal LEDC (TIMER_0/CHANNEL_0) que precisa ser exclusivo do driver
     * da câmera — por isso o servo em config.h vai para o canal 1.
     */
    camera_config_t cfg = {
        .pin_pwdn       = CONFIG_CAM_PIN_PWDN,
        .pin_reset      = CONFIG_CAM_PIN_RESET,
        .pin_xclk       = CONFIG_CAM_PIN_XCLK,
        .pin_sccb_sda   = CONFIG_CAM_PIN_SIOD,
        .pin_sccb_scl   = CONFIG_CAM_PIN_SIOC,

        .pin_d7         = CONFIG_CAM_PIN_D7,
        .pin_d6         = CONFIG_CAM_PIN_D6,
        .pin_d5         = CONFIG_CAM_PIN_D5,
        .pin_d4         = CONFIG_CAM_PIN_D4,
        .pin_d3         = CONFIG_CAM_PIN_D3,
        .pin_d2         = CONFIG_CAM_PIN_D2,
        .pin_d1         = CONFIG_CAM_PIN_D1,
        .pin_d0         = CONFIG_CAM_PIN_D0,

        .pin_vsync      = CONFIG_CAM_PIN_VSYNC,
        .pin_href       = CONFIG_CAM_PIN_HREF,
        .pin_pclk       = CONFIG_CAM_PIN_PCLK,

        .xclk_freq_hz   = CONFIG_CAM_XCLK_FREQ_HZ,

        /* Canal LEDC exclusivo do XCLK da câmera. Servo usa o canal 1. */
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = CONFIG_CAM_PIXEL_FORMAT,
        .frame_size     = CONFIG_CAM_FRAMESIZE,
        .jpeg_quality   = CONFIG_CAM_JPEG_QUALITY,
        .fb_count       = CONFIG_CAM_FB_COUNT,

        /* Framebuffers vão para PSRAM. Sem isso, o driver falharia com
         * ESP_ERR_NO_MEM em SVGA. */
        .fb_location    = CAMERA_FB_IN_PSRAM,

        .grab_mode      = CONFIG_CAM_GRAB_MODE,

        /* -1 = cria um novo barramento I2C/SCCB nos pinos SIOD/SIOC.
         * Não usamos I2C_NUM_0 porque nada mais no firmware o controla,
         * e passar 0 faria o driver tentar reutilizá-lo sem estar
         * inicializado. */
        .sccb_i2c_port  = -1,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init() falhou: %s", esp_err_to_name(err));
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Nenhum sensor respondeu. Verifique, na ordem:");
            ESP_LOGE(TAG, "  1. Cabo flat bem encaixado (contatos p/ baixo)");
            ESP_LOGE(TAG, "  2. Alimentação estável em 3.3V e 5V");
            ESP_LOGE(TAG, "  3. Pinagem em config.h = placa real");
            ESP_LOGE(TAG, "  4. PSRAM detectada (log \"PSRAM total\" acima)");
        }
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "esp_camera_sensor_get() retornou NULL após init OK");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    detect_sensor(s);
    apply_sensor_specific_config(s);

    g_initialized = true;

    ESP_LOGI(TAG, "Câmera inicializada: %s, frame size %d, qualidade %d",
             g_sensor_name,
             (int)cfg.frame_size,
             cfg.jpeg_quality);

    return ESP_OK;
}

esp_err_t camera_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_camera_deinit();
    if (err == ESP_OK) {
        g_initialized = false;
        ESP_LOGI(TAG, "Câmera liberada");
    } else {
        ESP_LOGE(TAG, "esp_camera_deinit() falhou: %s", esp_err_to_name(err));
    }
    return err;
}

/* -----------------------------------------------------------------------------
 * Captura
 * -------------------------------------------------------------------------- */

esp_err_t camera_capture(camera_frame_t **out_frame)
{
    if (out_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_frame = NULL;

    /*
     * esp_camera_fb_get() bloqueia até um frame estar disponível. Não há
     * parâmetro de timeout no driver. Medimos o tempo aqui para fins de
     * diagnóstico (log); não conseguimos interromper a chamada.
     *
     * Se algo travar de verdade, o Task Watchdog do ESP-IDF reinicia a
     * placa em 10 s (configurado em sdkconfig.defaults).
     */
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb  = esp_camera_fb_get();
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    int elapsed_ms = (int)(elapsed_us / 1000);

    if (fb == NULL) {
        ESP_LOGE(TAG, "esp_camera_fb_get() retornou NULL (após %d ms)",
                 elapsed_ms);
        return ESP_FAIL;
    }

    /* Sanidade: se o sensor não tem engine JPEG, o driver teria entregue
     * um frame RGB/YUV mesmo com PIXFORMAT_JPEG configurado. Não queremos
     * passar isso adiante fingindo que é JPEG. */
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Frame não é JPEG (format=%d). Sensor sem engine JPEG?",
                 (int)fb->format);
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_STATE;
    }

    if (elapsed_ms > CONFIG_CAMERA_CAPTURE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Captura levou %d ms (esperado < %d ms), frame OK: "
                      "%u×%u, %u bytes",
                 elapsed_ms, CONFIG_CAMERA_CAPTURE_TIMEOUT_MS,
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
    } else {
        ESP_LOGI(TAG, "Frame: %u×%u, %u bytes, %d ms",
                 (unsigned)fb->width, (unsigned)fb->height,
                 (unsigned)fb->len, elapsed_ms);
    }

    *out_frame = (camera_frame_t *)fb;
    return ESP_OK;
}

void camera_release(camera_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    esp_camera_fb_return((camera_fb_t *)frame);
}

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

uint16_t camera_get_sensor_pid(void)
{
    return g_sensor_pid;
}

const char *camera_get_sensor_name(void)
{
    return g_sensor_name;
}

void *camera_get_sensor(void)
{
    if (!g_initialized) {
        return NULL;
    }
    return (void *)esp_camera_sensor_get();
}
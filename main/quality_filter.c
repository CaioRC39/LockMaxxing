/* =============================================================================
 * main/quality_filter.c — Análise de qualidade em JPEG
 * =============================================================================
 * Decodifica o JPEG efetivo (o que será enviado) em QQVGA RGB565 e calcula
 * brilho e nitidez. Sem troca de modo da câmera — o FB-OVF do RGB565 QQVGA
 * dinâmico não ocorre mais.
 * ========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "jpeg_decoder.h"

#include "sdkconfig.h"
#include "quality_filter.h"

static const char *TAG = "QUALITY";

/* Buffer de decodificação: QQVGA RGB565 (160x120x2 = 38400 bytes).
 *
 * Estático em .bss. Se o projeto passar a sofrer pressão de RAM interna,
 * mover para PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM) num init
 * one-shot. Por ora, 38 KB é aceitável na S3 (512 KB). */
#define MAX_DECODE_PIXELS   (160 * 120)
#define MAX_DECODE_BYTES    (MAX_DECODE_PIXELS * 2)
static uint8_t s_decode_buf[MAX_DECODE_BYTES];

const char *quality_result_name(quality_result_t r)
{
    switch (r) {
    case QUALITY_OK:         return "OK";
    case QUALITY_TOO_DARK:   return "TOO_DARK";
    case QUALITY_TOO_BRIGHT: return "TOO_BRIGHT";
    case QUALITY_TOO_BLURRY: return "TOO_BLURRY";
    case QUALITY_ERROR:      return "ERROR";
    default:                 return "UNKNOWN";
    }
}

/* Converte um pixel RGB565 (little-endian) em intensidade Y. */
static inline uint8_t rgb565_to_y(const uint8_t *p)
{
    uint16_t v = ((uint16_t)p[1] << 8) | p[0];
    uint8_t r5 = (v >> 11) & 0x1F;
    uint8_t g6 = (v >> 5)  & 0x3F;
    uint8_t b5 = v         & 0x1F;

    uint8_t r8 = (r5 << 3) | (r5 >> 2);
    uint8_t g8 = (g6 << 2) | (g6 >> 4);
    uint8_t b8 = (b5 << 3) | (b5 >> 2);

    return (uint8_t)(((77 * (uint32_t)r8) +
                      (150 * (uint32_t)g8) +
                      (29 * (uint32_t)b8)) >> 8);
}

quality_result_t quality_analyze_jpeg(const uint8_t *jpeg_data,
                                       size_t jpeg_len,
                                       quality_metrics_t *out)
{
    if (out == NULL) return QUALITY_ERROR;
    memset(out, 0, sizeof(*out));
    out->result = QUALITY_ERROR;

    if (jpeg_data == NULL || jpeg_len == 0) {
        ESP_LOGE(TAG, "JPEG inválido (data=NULL ou len=0)");
        return QUALITY_ERROR;
    }

    /* ---- Decodifica o JPEG em QQVGA RGB565 ---- */
    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpeg_data,
        .indata_size = (uint32_t)jpeg_len,
        .outbuf      = s_decode_buf,
        .outbuf_size = sizeof(s_decode_buf),
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_1_4,   /* VGA(640x480) → QQVGA(160x120) */
        .flags = {
            .swap_color_bytes = 0,   /* 0 = saída big-endian, 1 = little-endian */
        },
    };
    esp_jpeg_image_output_t info = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &info);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_jpeg_decode falhou: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return QUALITY_ERROR;
    }

    const uint16_t width  = info.width;
    const uint16_t height = info.height;

    if (width < 4 || height < 4) {
        ESP_LOGE(TAG, "Decodificação gerou imagem muito pequena: %ux%u",
                 width, height);
        return QUALITY_ERROR;
    }
    if ((uint32_t)width * height * 2 > sizeof(s_decode_buf)) {
        ESP_LOGE(TAG, "Buffer pequeno para %ux%u — aumente MAX_DECODE_PIXELS",
                 width, height);
        return QUALITY_ERROR;
    }

    const uint32_t total_pixels = (uint32_t)width * height;

    /* ---- Passo 1: brilho (média Y) ---- */
    uint64_t sum_y = 0;
    for (uint32_t i = 0; i < total_pixels; i++) {
        sum_y += rgb565_to_y(&s_decode_buf[i * 2]);
    }
    out->brightness = (float)sum_y / (float)total_pixels;

    /* ---- Passo 2: nitidez (variância do Laplaciano) ----
     *
     * Recalcula Y a cada acesso para economizar 19 KB de RAM (buffer Y).
     * Custo: ~2-3 ms adicionais por frame. Irrelevante com ciclo de 30 s. */
    int64_t  sum_lap    = 0;
    int64_t  sum_lap_sq = 0;
    uint32_t count      = 0;

    for (uint16_t y = 1; y < height - 1; y++) {
        const uint32_t row      = (uint32_t)y * width;
        const uint32_t row_up   = row - width;
        const uint32_t row_down = row + width;

        for (uint16_t x = 1; x < width - 1; x++) {
            const uint32_t i = row + x;
            int16_t lap = (int16_t)(4 * (int16_t)rgb565_to_y(&s_decode_buf[i * 2])
                                    - (int16_t)rgb565_to_y(&s_decode_buf[(i - 1) * 2])
                                    - (int16_t)rgb565_to_y(&s_decode_buf[(i + 1) * 2])
                                    - (int16_t)rgb565_to_y(&s_decode_buf[(row_up + x) * 2])
                                    - (int16_t)rgb565_to_y(&s_decode_buf[(row_down + x) * 2]));
            sum_lap    += lap;
            sum_lap_sq += (int64_t)lap * lap;
            count++;
        }
    }

    if (count == 0) {
        ESP_LOGE(TAG, "Sem pixels internos para Laplaciano");
        return QUALITY_ERROR;
    }

    float mean_lap = (float)sum_lap / (float)count;
    float var_lap  = ((float)sum_lap_sq / (float)count) - (mean_lap * mean_lap);
    if (var_lap < 0.0f) var_lap = 0.0f;
    out->sharpness = var_lap;

    /* Ordem de prioridade:
     *   1. TOO_BLURRY — ação: reajustar foco (acionável)
     *   2. TOO_DARK   — ação: iluminar cena (acionável mas fora do controle do SW)
     *   3. TOO_BRIGHT — ação: reduzir luz (idem)
     *
     * Um frame desfocado apontado para luz forte é mais provavelmente
     * "esqueci de focar" do que "a luz está intensa". Reportar TOO_BLURRY
     * dá ao usuário o diagnóstico certo.
     */
    if (out->sharpness < CONFIG_LOCKMAXXING_QUALITY_MIN_SHARPNESS) {
        out->result = QUALITY_TOO_BLURRY;
    } else if (out->brightness < CONFIG_LOCKMAXXING_QUALITY_MIN_BRIGHTNESS) {
        out->result = QUALITY_TOO_DARK;
    } else if (out->brightness > CONFIG_LOCKMAXXING_QUALITY_MAX_BRIGHTNESS) {
        out->result = QUALITY_TOO_BRIGHT;
    } else {
        out->result = QUALITY_OK;
    }

    /* Log adicional: reporta todos os problemas detectados,
     * não só o prioritário. Útil para calibrar. */
#if CONFIG_LOCKMAXXING_QUALITY_DEBUG_LOG
    if (out->sharpness < CONFIG_LOCKMAXXING_QUALITY_MIN_SHARPNESS &&
        (out->brightness < CONFIG_LOCKMAXXING_QUALITY_MIN_BRIGHTNESS ||
         out->brightness > CONFIG_LOCKMAXXING_QUALITY_MAX_BRIGHTNESS)) {
        ESP_LOGW(TAG, "Frame tem MÚLTIPLOS problemas: nitidez=%.1f brilho=%.1f "
                      "(reportado como prioridade: %s)",
                 out->sharpness, out->brightness,
                 quality_result_name(out->result));
    }
#else
    if (out->result != QUALITY_OK) {
        ESP_LOGW(TAG, "Rejeitado: %s (brilho=%.1f, nitidez=%.1f)",
                 quality_result_name(out->result),
                 out->brightness, out->sharpness);
    }
#endif

    return out->result;
}
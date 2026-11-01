#ifndef QUALITY_FILTER_H
#define QUALITY_FILTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    QUALITY_OK = 0,
    QUALITY_TOO_DARK,
    QUALITY_TOO_BRIGHT,
    QUALITY_TOO_BLURRY,
    QUALITY_ERROR,
} quality_result_t;

typedef struct {
    float brightness;
    float sharpness;
    quality_result_t result;
} quality_metrics_t;

/**
 * @brief Analisa um frame JPEG diretamente.
 *
 * Decodifica o JPEG em QQVGA RGB565 (via esp_jpeg), calcula:
 *   - brightness = média do canal Y (0-255)
 *   - sharpness  = variância do operador Laplaciano
 *
 * Aplica os thresholds do Kconfig.
 *
 * @param jpeg_data  Buffer JPEG (não NULL, jpeg_len > 0).
 * @param jpeg_len   Tamanho em bytes.
 * @param out        Struct de saída (não NULL).
 *
 * @return O resultado da classificação.
 */
quality_result_t quality_analyze_jpeg(const uint8_t *jpeg_data,
                                       size_t jpeg_len,
                                       quality_metrics_t *out);

const char *quality_result_name(quality_result_t result);

#endif
/* =============================================================================
 * main/hmac_auth.h — Assinatura HMAC-SHA256 de imagens
 * =============================================================================
 *
 * Assina (id || size || chunks || image_bytes) com um segredo hexadecimal
 * carregado da NVS. Usado para o servidor autenticar que a imagem veio
 * de um cliente autorizado.
 *
 * Sem segredo configurado: HMAC desabilitado, funções retornam erro
 * "not configured" e o firmware envia imagens sem assinatura.
 * ========================================================================== */

#ifndef HMAC_AUTH_H
#define HMAC_AUTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Inicializa PSA Crypto e carrega o segredo da NVS.
 *
 * Deve ser chamada uma vez, em app_main(), após app_config_load().
 *
 * @return ESP_OK em sucesso (HMAC ativo ou desabilitado).
 *         ESP_ERR_INVALID_STATE se PSA Crypto não inicializou.
 *         ESP_ERR_INVALID_SIZE se o segredo configurado não tem 64 chars hex.
 */
esp_err_t hmac_auth_init(void);

/**
 * @brief Retorna true se HMAC está configurado (segredo não vazio).
 */
bool hmac_auth_is_enabled(void);

/**
 * @brief Assina uma imagem e produz a assinatura em hex.
 *
 * Entrada assinada:
 *   id (4B big-endian) || size (4B BE) || chunks (2B BE) || image_bytes
 *
 * @param image_id     ID da imagem.
 * @param total_size   Tamanho total do JPEG em bytes.
 * @param total_chunks Número de chunks.
 * @param image_data   Buffer da imagem.
 * @param image_len    Tamanho da imagem.
 * @param out_hex      Buffer de saída com 64 hex chars + '\0' (65 bytes).
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_STATE se HMAC não está configurado.
 *         ESP_FAIL se o cálculo PSA falhou.
 */
esp_err_t hmac_sign_image(uint32_t image_id,
                          uint32_t total_size,
                          uint16_t total_chunks,
                          const uint8_t *image_data,
                          size_t image_len,
                          char out_hex[65]);

#endif
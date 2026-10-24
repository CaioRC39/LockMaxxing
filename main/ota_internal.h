/* =============================================================================
 * main/ota_internal.h — Contrato interno do subsistema OTA
 * =============================================================================
 *
 * Este header é incluído SOMENTE por ota_common.c, ota_client.c e ota_server.c.
 * Nenhum outro módulo do firmware deve incluir este arquivo — a API pública
 * está em ota.h.
 *
 * Aqui ficam:
 *   - O mutex global que serializa mudanças de estado entre as duas metades
 *   - A declaração do estado global de OTA (definido em ota_common.c)
 *   - Helpers de progresso e SHA-256 compartilhados
 * ========================================================================== */

#ifndef OTA_INTERNAL_H
#define OTA_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Estado compartilhado
 * -------------------------------------------------------------------------- */

/*
 * Mutex que protege o estado global de OTA e impede duas operações OTA
 * concorrentes (por exemplo, cliente HTTPS rodando enquanto um POST /update
 * chega no servidor local).
 *
 * Criado uma única vez em ota_init(). Nunca destruído (o firmware vive até
 * reboot).
 */
extern SemaphoreHandle_t g_ota_mutex;

/*
 * Atualiza o estado global de OTA.
 *
 * A função assume que g_ota_mutex está criado. Ela pega o mutex, grava o
 * estado e solta. Se o mutex não existir, é um bug de inicialização — o
 * código loga um erro mas não trava.
 */
void ota_set_state(ota_state_t state);

/*
 * Preenche um struct de progresso a partir dos dois contadores de bytes.
 * Se total == 0, percent permanece 0 (indica "desconhecido").
 */
void ota_fill_progress(ota_progress_t *p, size_t received, size_t total);

/* -----------------------------------------------------------------------------
 * SHA-256 — usado pelos dois modos (cliente e servidor)
 * -------------------------------------------------------------------------- */

/*
 * Calcula SHA-256 de um único buffer e escreve 64 caracteres hex lowercase
 * seguidos de '\0' (portanto out_hex precisa ter 65 bytes).
 */
esp_err_t ota_sha256_hex(const uint8_t *data, size_t len,
                         char out_hex[OTA_SHA256_HEX_LEN + 1]);

/*
 * Versão em streaming, para hashing de dados que chegam em chunks
 * (download HTTPS ou corpo de POST HTTP).
 *
 * Uso típico:
 *
 *   mbedtls_sha256_context ctx;
 *   ota_sha256_stream_init(&ctx);
 *   while (tem_dados) {
 *       ota_sha256_stream_update(&ctx, buf, n);
 *   }
 *   char hex[OTA_SHA256_HEX_LEN + 1];
 *   ota_sha256_stream_finish(&ctx, hex);
 *
 * O contexto é do tipo mbedtls (definido em mbedtls/sha256.h) — usamos o
 * tipo concreto para evitar esconder alocação de memória atrás de um void*.
 */
esp_err_t ota_sha256_stream_init(psa_hash_operation_t *ctx);
esp_err_t ota_sha256_stream_update(psa_hash_operation_t *ctx,
                                   const uint8_t *data,
                                   size_t len);
esp_err_t ota_sha256_stream_finish(psa_hash_operation_t *ctx,
                                   char hex_out[65]);

/*
 * Compara duas strings hex de SHA-256 (case-insensitive).
 * Retorna true se ambas forem não-NULL, tiverem 64 chars e forem iguais.
 */
bool ota_sha256_hex_equals(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* OTA_INTERNAL_H */
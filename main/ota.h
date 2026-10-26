/* =============================================================================
 * main/ota.h — Módulo de Over-The-Air update
 * =============================================================================
 *
 * Este módulo expõe duas formas de atualizar o firmware:
 *
 *   1. CLIENTE HTTPS (pull):
 *        O firmware baixa uma imagem de um servidor HTTPS remoto.
 *        Usa "esp_https_ota" e verifica o certificado do servidor.
 *
 *   2. SERVIDOR HTTPS LOCAL (push):
 *        A placa sobe um servidor HTTPS. Um cliente (curl, Python)
 *        faz POST /update com o binário. Útil em bancada.
 *
 * Ambos os modos usam o mesmo núcleo de escrita em partição OTA
 * (esp_ota_begin / esp_ota_write / esp_ota_end / esp_ota_set_boot_partition).
 *
 * ROLLBACK:
 *   Se o novo firmware não chamar ota_mark_app_valid() após o self-test,
 *   o bootloader considera a imagem como "pendente" e, após o número
 *   configurado de tentativas, reverte para a partição anterior.
 *
 * INTEGRIDADE (sem Secure Boot):
 *   Não há verificação criptográfica em hardware. A aplicação opcionalmente
 *   compara o SHA-256 informado pelo cliente com o SHA-256 calculado sobre
 *   os bytes gravados. Isso detecta corrupção de transporte, mas NÃO
 *   garante autenticidade. Para autenticidade real, habilite Secure Boot V2
 *   (exige gravação de eFuse — fora do escopo desta configuração).
 * ========================================================================== */

#ifndef OTA_H
#define OTA_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Constantes públicas
 * -------------------------------------------------------------------------- */

/* Tamanho máximo de uma mensagem de resultado (incluindo '\0'). */
#define OTA_RESULT_MSG_MAX      128

/* Tamanho de um SHA-256 em hexadecimal (sem '\0'). */
#define OTA_SHA256_HEX_LEN      64

/* Porta HTTPS padrão do servidor local de OTA. */
#define OTA_DEFAULT_LOCAL_PORT  8443

/* -----------------------------------------------------------------------------
 * Estados do subsistema OTA
 * -------------------------------------------------------------------------- */

typedef enum {
    OTA_STATE_IDLE = 0,      /* Nada em andamento.                          */
    OTA_STATE_DOWNLOADING,   /* Recebendo bytes (via cliente ou servidor).  */
    OTA_STATE_VERIFYING,     /* Verificando SHA-256 / integridade.          */
    OTA_STATE_APPLYING,      /* esp_ota_set_boot_partition em progresso.    */
    OTA_STATE_SUCCESS,       /* OTA concluído — reinício iminente.          */
    OTA_STATE_ERROR,         /* Falha — veja a mensagem no callback.        */
} ota_state_t;

/* -----------------------------------------------------------------------------
 * Progresso reportado ao chamador
 * -------------------------------------------------------------------------- */

typedef struct {
    size_t  bytes_received;
    size_t  bytes_total;     /* 0 quando desconhecido (ex: chunked). */
    uint8_t percent;         /* 0..100; 0 quando total desconhecido. */
} ota_progress_t;

/* -----------------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------------- */

/* Chamado periodicamente durante o download (não bloqueie neste callback). */
typedef void (*ota_progress_cb_t)(const ota_progress_t *progress);

/* Chamado UMA vez ao final do processo.
 *   result   : ESP_OK em sucesso, código de erro em falha.
 *   message  : string descritiva (sempre válida, sempre NUL-terminada).
 */
typedef void (*ota_result_cb_t)(esp_err_t result, const char *message);

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa o subsistema OTA.
 *
 * Deve ser chamada uma única vez, no início de app_main().
 * Apenas consulta o estado do bootloader (não grava nada).
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t ota_init(void);

/* -----------------------------------------------------------------------------
 * Confirmação de boot (para rollback)
 * -------------------------------------------------------------------------- */

/**
 * @brief Retorna true se o firmware atual foi bootado a partir de uma
 *        partição OTA e ainda NÃO foi confirmado como válido.
 *
 * Nesse estado, o bootloader está aguardando uma chamada a
 * ota_mark_app_valid(). Se o firmware travar antes, ele reverte no próximo
 * boot.
 */
bool ota_is_pending_verify(void);

/**
 * @brief Confirma o firmware atual como válido (cancela o rollback).
 *
 * Deve ser chamada DEPOIS que o self-test da aplicação passar:
 * câmera inicializada, Wi-Fi conectado, MQTT conectado, etc.
 *
 * @return ESP_OK em sucesso, ESP_ERR_INVALID_STATE se não havia
 *         confirmação pendente.
 */
esp_err_t ota_mark_app_valid(void);

/**
 * @brief Declara o firmware atual como inválido e reinicia imediatamente.
 *
 * Útil quando o self-test falha e você quer forçar o rollback agora,
 * em vez de esperar o bootloader desistir sozinho.
 *
 * Esta função NÃO retorna em caso de sucesso (faz esp_restart()).
 *
 * @return Código de erro caso o reboot não aconteça.
 */
esp_err_t ota_mark_app_invalid_and_reboot(void);

/* -----------------------------------------------------------------------------
 * OTA remoto (cliente HTTPS) — modelo "pull"
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicia uma atualização OTA via HTTPS em uma task interna.
 *
 * Retorna imediatamente. O progresso e o resultado são entregues pelos
 * callbacks informados.
 *
 * @param url                 URL do firmware (ex: "https://host/fw.bin").
 * @param expected_sha256_hex SHA-256 hex (64 chars) esperado do arquivo
 *                            baixado. Se NULL, a verificação extra é
 *                            ignorada (o esp_ota_end() ainda valida a
 *                            estrutura da imagem).
 * @param progress_cb         Callback de progresso (pode ser NULL).
 * @param result_cb           Callback de resultado (pode ser NULL, mas
 *                            altamente recomendável).
 *
 * @return ESP_OK se a task foi criada com sucesso.
 *         ESP_ERR_INVALID_STATE se já há uma operação OTA em andamento.
 *         ESP_ERR_INVALID_ARG se a URL for inválida.
 */
esp_err_t ota_https_update_start(const char *url,
                                 const char *expected_sha256_hex,
                                 ota_progress_cb_t progress_cb,
                                 ota_result_cb_t result_cb);

/* -----------------------------------------------------------------------------
 * OTA local (servidor HTTPS) — modelo "push"
 * -------------------------------------------------------------------------- */

/**
 * @brief Sobe o servidor HTTPS local para receber firmware via POST /update.
 *
 * O certificado do servidor é embutido no firmware (self-signed, gerado
 * no build). Clientes devem usar -k (curl) ou equivalente, ou confiar
 * no certificado manualmente.
 *
 * Fluxo do servidor:
 *   1. Cliente faz POST /update com o binário no corpo.
 *   2. Servidor grava bytes na partição OTA inativa.
 *   3. Ao final, chama esp_ota_end() + esp_ota_set_boot_partition().
 *   4. Responde HTTP 200 e reinicia em ~500 ms.
 *
 * @param port Porta TCP (use OTA_DEFAULT_LOCAL_PORT para 8443).
 *
 * @return ESP_OK se o servidor subiu.
 *         ESP_ERR_INVALID_STATE se já está rodando.
 */
esp_err_t ota_local_server_start(uint16_t port);

/**
 * @brief Para o servidor HTTPS local (se estiver rodando).
 *
 * Não é possível chamar enquanto um upload está em andamento —
 * aguarda a conexão atual terminar ou timeout.
 */
esp_err_t ota_local_server_stop(void);

/**
 * @brief Retorna true se o servidor HTTPS local está ativo.
 */
bool ota_local_server_is_running(void);

/* -----------------------------------------------------------------------------
 * Consulta de estado e diagnóstico
 * -------------------------------------------------------------------------- */

/**
 * @brief Retorna o estado atual do subsistema OTA.
 */
ota_state_t ota_get_state(void);

/**
 * @brief Obtém informações sobre a partição atualmente em execução.
 *
 * @param part_out  Recebe o ponteiro para a struct da partição (não liberar).
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t ota_get_running_partition_info(const esp_partition_t **part_out);

/**
 * @brief Obtém informações sobre a partição que será alvo da próxima OTA.
 *
 * @param part_out  Recebe o ponteiro para a struct da partição (não liberar).
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t ota_get_next_update_partition_info(const esp_partition_t **part_out);

/* Flag simples de "OTA em andamento". true = captura/envio pausados.
 * Escrita pelo módulo OTA (server e client), lida por qualquer task.
 * Volatile porque é acessada de múltiplos cores sem sincronização
 * (bool atômico em Xtensa — leitura/escrita de 1 byte é indivisível). */
extern volatile bool g_ota_busy;

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
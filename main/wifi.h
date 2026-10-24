/* =============================================================================
 * main/wifi.h — Conexão Wi-Fi (station mode) com auto-reconexão
 * =============================================================================
 *
 * Este módulo encapsula toda a interação com o driver Wi-Fi do ESP-IDF.
 * O resto do firmware NUNCA chama esp_wifi_* — apenas as funções abaixo.
 *
 * Responsabilidades:
 *   - Inicializar o driver, netif e event handlers
 *   - Conectar com as credenciais de config.h
 *   - Reconectar automaticamente após queda (backoff exponencial)
 *   - Expor estado de conexão via event group (para tasks bloquearem)
 *   - Reportar o IP obtido via DHCP
 *
 * Modelo de uso:
 *
 *   // Em app_main, uma única vez:
 *   wifi_init();
 *   wifi_wait_connected(30000);   // espera até 30s
 *
 *   // Em tasks que dependem de rede:
 *   EventBits_t bits = xEventGroupWaitBits(wifi_get_event_group(),
 *                                          WIFI_CONNECTED_BIT,
 *                                          pdFALSE, pdTRUE, portMAX_DELAY);
 *
 * Contexto de execução:
 *   - wifi_init()              : chamado em app_main, antes das tasks
 *   - wifi_wait_connected()    : chamado em app_main ou numa task
 *   - wifi_is_connected()      : seguro de qualquer task (leitura atômica)
 *   - wifi_get_ip()            : seguro de qualquer task (copia para buffer)
 *   - wifi_get_event_group()   : seguro, retorna handle estável após init
 *   - wifi_disconnect()        : pensado para testes; em uso normal,
 *                                o módulo gerencia a conexão sozinho
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Bits do event group
 * --------------------------------------------------------------------------
 *
 * Quem chamar wifi_get_event_group() pode aguardar por estes bits.
 * Usamos bits distintos para diferenciar "associado ao AP" (CONNECTED)
 * de "DHCP concluído" (GOT_IP). Alguns roteadores levam 1–2 s entre os
 * dois eventos. Para uso prático (enviar MQTT, servir HTTP), GOT_IP é
 * o que importa.
 * -------------------------------------------------------------------------- */

/* Wi-Fi associou ao AP (nível 2 do modelo OSI). Sem IP ainda. */
#define WIFI_CONNECTED_BIT      BIT0

/* DHCP entregou IP, máscara, gateway (nível 3). Aqui a rede está utilizável. */
#define WIFI_GOT_IP_BIT         BIT1

/* Desconectado. Útil para quem quer reagir à queda. */
#define WIFI_DISCONNECTED_BIT   BIT2

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa o driver Wi-Fi, netif e event handlers, e inicia a conexão.
 *
 * Não bloqueia esperando conexão — retorna após o driver aceitar a
 * configuração. Para esperar a conexão, use wifi_wait_connected().
 *
 * Idempotente: chamar duas vezes retorna ESP_OK na segunda.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_STATE se NVS não foi inicializada (chame
 *         nvs_flash_init() antes — main.c já faz).
 *         ESP_ERR_NO_MEM em falha de alocação.
 *         Outros erros propagados do driver esp_wifi.
 */
esp_err_t wifi_init(void);

/**
 * @brief Para o Wi-Fi e libera recursos.
 *
 * Normalmente não é chamada. Útil em testes ou em modos de baixo
 * consumo onde o rádio deve desligar por completo.
 */
esp_err_t wifi_teardown(void);

/* -----------------------------------------------------------------------------
 * Estado e espera
 * -------------------------------------------------------------------------- */

/**
 * @brief Espera até que o Wi-Fi esteja conectado E com IP, ou até o timeout.
 *
 * Bloqueia a task chamadora no event group WIFI_GOT_IP_BIT. Se o
 * Wi-Fi cair durante a espera, continua esperando até o timeout.
 *
 * Em caso de timeout, a conexão CONTINUA sendo tentada em background
 * pelo módulo — a task pode seguir e tentar de novo depois, ou consultar
 * wifi_is_connected() periodicamente.
 *
 * @param timeout_ms  Tempo máximo de espera, em ms.
 *                    Use 0 para apenas verificar sem esperar.
 *                    Use portMAX_DELAY para esperar para sempre.
 *
 * @return ESP_OK se conectado (com IP) antes do timeout.
 *         ESP_ERR_TIMEOUT se o timeout expirou sem conexão.
 *         ESP_ERR_INVALID_STATE se wifi_init() ainda não foi chamada.
 */
esp_err_t wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief Retorna true se o Wi-Fi está associado ao AP e tem IP.
 *
 * Não bloqueia. Seguro de qualquer task.
 */
bool wifi_is_connected(void);

/**
 * @brief Copia o IP atual (string dotted-quad) para o buffer.
 *
 * Se o Wi-Fi ainda não obteve IP, retorna ESP_ERR_INVALID_STATE e
 * preenche o buffer com "0.0.0.0". Buffer deve ter pelo menos
 * 16 bytes (tamanho de "255.255.255.255" + NUL).
 *
 * @param buf     Buffer de destino (não NULL).
 * @param buf_len Tamanho do buffer, em bytes (>= 16).
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_ARG se buf for NULL ou buf_len for pequeno.
 *         ESP_ERR_INVALID_STATE se não há IP ainda.
 */
esp_err_t wifi_get_ip(char *buf, size_t buf_len);

/* -----------------------------------------------------------------------------
 * Event group (para sincronização com outras tasks)
 * -------------------------------------------------------------------------- */

/**
 * @brief Retorna o event group de Wi-Fi.
 *
 * Válido após wifi_init() ser chamada. Retorna NULL antes disso.
 *
 * Uso típico em tasks que dependem de rede:
 *
 *   EventGroupHandle_t eg = wifi_get_event_group();
 *   if (eg != NULL) {
 *       xEventGroupWaitBits(eg, WIFI_GOT_IP_BIT,
 *                           pdFALSE, pdTRUE, portMAX_DELAY);
 *   }
 *
 * Não destrua o handle. O módulo é dono dele.
 */
EventGroupHandle_t wifi_get_event_group(void);

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

/**
 * @brief Contadores de reconexão desde o boot. Úteis em logs e diagnóstico.
 *
 * @param attempts_out  Recebe o número de tentativas de conexão feitas
 *                      (incluindo a primeira).
 * @param failures_out  Recebe o número de desconexões observadas.
 *
 * Ambos os parâmetros podem ser NULL se não interessarem.
 */
void wifi_get_stats(uint32_t *attempts_out, uint32_t *failures_out);

/**
 * @brief Desconecta manualmente (para testes de reconexão).
 *
 * Em uso normal, você não precisa chamar esta função — o módulo já
 * gerencia a conexão. Ela é útil para forçar uma queda e verificar que
 * a auto-reconexão funciona.
 *
 * A próxima reconexão automática acontece como se fosse uma queda real.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_STATE se não está conectado.
 */
esp_err_t wifi_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
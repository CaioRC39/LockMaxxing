/* =============================================================================
 * main/factory_reset_button.h — Botão físico de reset de fábrica
 * =============================================================================
 *
 * Usa o botão BOOT (GPIO0) da N16R8 para acionar reset de fábrica:
 *   - Usuário segura o botão por CONFIG_LOCKMAXXING_FACTORY_RESET_HOLD_MS
 *   - Placa apaga a NVS e reinicia em modo provisionamento
 *
 * Segurança: a leitura só começa após um delay de boot (evita conflito com
 * o strapping pin do GPIO0). Durante o boot, o botão continua funcionando
 * como "modo download" — a lógica de aplicação não interfere.
 *
 * Uso:
 *   Em app_main(), APÓS tasks_start_all():
 *       factory_reset_button_init();
 * ========================================================================== */

#ifndef FACTORY_RESET_BUTTON_H
#define FACTORY_RESET_BUTTON_H

#include "esp_err.h"

/**
 * @brief Inicia a task que monitora o botão de reset de fábrica.
 *
 * A task:
 *   1. Aguarda 5 s após boot (garante strapping pin liberado)
 *   2. Faz polling do GPIO0 a cada 50 ms
 *   3. Se pressionado, loga progresso em 25/50/75%
 *   4. Em 100%, chama app_config_erase() e reinicia
 *
 * Se o usuário soltar antes do tempo, o contador reseta (sem efeito).
 *
 * @return ESP_OK se a task foi criada.
 *         ESP_ERR_NO_MEM se não houver memória para a task.
 *         ESP_FAIL se o GPIO não pôde ser configurado.
 */
esp_err_t factory_reset_button_init(void);

#endif /* FACTORY_RESET_BUTTON_H */
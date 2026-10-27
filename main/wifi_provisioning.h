/* =============================================================================
 * main/wifi_provisioning.h — Portal de provisionamento via SoftAP
 * =============================================================================
 *
 * Quando chamado, este módulo:
 *   1. Sobe um SoftAP "LockMaxxing-Setup" (senha opcional)
 *   2. Sobe um DNS captive portal (responde qualquer query com o IP do ESP32)
 *   3. Sobe um HTTP server com o formulário de configuração
 *   4. Bloqueia a task chamadora até configuração ou timeout
 *   5. Salva na NVS e reinicia a placa
 *
 * NÃO chame esta função em uso normal — só no primeiro boot (NVS vazia)
 * ou após reset de fábrica.
 *
 * Broker NÃO é configurado aqui — vem de CONFIG_LOCKMAXXING_FACTORY_BROKER_URI.
 * ========================================================================== */

#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"

/**
 * @brief Inicia o modo provisionamento e bloqueia até o usuário configurar.
 *
 * Ao final (por configuração ou timeout), a placa reinicia via esp_restart().
 *
 * Se o usuário submeter o formulário: salva na NVS e reinicia → próximo
 * boot entra em modo normal (wifi_init + mqtt_init).
 *
 * Se o timeout expirar sem configuração: reinicia sem salvar → próximo
 * boot reentra em modo provisionamento.
 *
 * Esta função NÃO retorna em uso normal (reboot é iminente). Retorna erro
 * apenas se algum recurso (httpd, SoftAP, DNS) não puder ser criado.
 *
 * @return Código de erro se o portal não conseguir iniciar.
 */
esp_err_t wifi_provisioning_run(void);

#endif /* WIFI_PROVISIONING_H */
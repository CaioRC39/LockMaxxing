/* =============================================================================
 * main/main.c — Ponto de entrada do firmware
 * =============================================================================
 *
 * Responsabilidade: inicialização, self-test e criação das tasks.
 * NÃO contém lógica de negócio — isso vive em tasks.c, state_machine.c,
 * camera.c, wifi.c, mqtt.c, actuators.c.
 *
 * Fluxo:
 *   1. NVS init
 *   2. OTA init (detecta PENDING_VERIFY pós-OTA)
 *   3. Boot info (chip, PSRAM, reset reason)
 *   4. Atuadores init + self-test rápido (LEDs, buzzer, servo)
 *   5. Câmera init + captura de teste
 *   6. Wi-Fi init + wait connected (timeout)
 *   7. MQTT init + wait connected (timeout)
 *   8. Confirmação de boot (cancela rollback se crítico OK)
 *   9. Start das tasks FreeRTOS
 *  10. Servidor HTTPS local de OTA (opcional)
 *  11. app_main retorna — tasks assumem o controle
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_camera.h" 

#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "camera.h"
#include "wifi.h"
#include "mqtt.h"
#include "actuators.h"
#include "tasks.h"
#include "ota.h"

static const char *TAG = CONFIG_LOG_TAG_MAIN;

/* -----------------------------------------------------------------------------
 * Boot info
 * -------------------------------------------------------------------------- */

static void log_boot_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *model_name;
    switch (chip.model) {
        case CHIP_ESP32:    model_name = "ESP32";       break;
        case CHIP_ESP32S2:  model_name = "ESP32-S2";    break;
        case CHIP_ESP32S3:  model_name = "ESP32-S3";    break;
        case CHIP_ESP32C3:  model_name = "ESP32-C3";    break;
        case CHIP_ESP32C6:  model_name = "ESP32-C6";    break;
        case CHIP_ESP32H2:  model_name = "ESP32-H2";    break;
        default:            model_name = "desconhecido"; break;
    }

    ESP_LOGI(TAG, "Chip: %s, %d core(s), rev v%d.%d",
             model_name, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    const char *reason_str;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reason_str = "power-on";   break;
        case ESP_RST_SW:        reason_str = "software";   break;
        case ESP_RST_PANIC:     reason_str = "panic";      break;
        case ESP_RST_WDT:       reason_str = "watchdog";   break;
        case ESP_RST_BROWNOUT:  reason_str = "brownout";   break;
        case ESP_RST_EXT:       reason_str = "external";   break;
        case ESP_RST_TASK_WDT:  reason_str = "task-wdt";   break;
        case ESP_RST_INT_WDT:   reason_str = "int-wdt";    break;
        case ESP_RST_DEEPSLEEP: reason_str = "deep-sleep"; break;
        default:                reason_str = "unknown";    break;
    }
    ESP_LOGI(TAG, "Motivo do reset: %s", reason_str);

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM: %u bytes livres inicialmente",
                 (unsigned)psram_size);
    } else {
        ESP_LOGW(TAG, "PSRAM NÃO detectada — verifique sdkconfig.defaults "
                      "(CONFIG_SPIRAM=y, CONFIG_SPIRAM_MODE_OCT=y)");
    }

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Heap livre no boot: %u B internal, %u B PSRAM",
             (unsigned)free_internal, (unsigned)free_psram);
}

/* -----------------------------------------------------------------------------
 * NVS
 * -------------------------------------------------------------------------- */

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS com problema (%s) — apagando e reinicializando",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase() falhou: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

/* -----------------------------------------------------------------------------
 * Reboot com delay (permite logs saírem pela UART antes)
 * -------------------------------------------------------------------------- */

static void force_reboot(const char *reason, uint32_t delay_ms)
{
    ESP_LOGE(TAG, "Reboot em %u ms — motivo: %s",
             (unsigned)delay_ms, reason);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
    /* Não retorna. */
}

/* -----------------------------------------------------------------------------
 * Verificação de placeholders de credenciais
 * -------------------------------------------------------------------------- */

static void warn_placeholder_credentials(void)
{
    if (strcmp(CONFIG_WIFI_SSID, "SUA_REDE_AQUI") == 0) {
        ESP_LOGW(TAG, "=================================================");
        ESP_LOGW(TAG, " CONFIG_WIFI_SSID ainda é o placeholder!");
        ESP_LOGW(TAG, " Edite main/config.h antes de usar o Wi-Fi.");
        ESP_LOGW(TAG, "=================================================");
    }
    if (strcmp(CONFIG_MQTT_BROKER_URI, "mqtt://192.168.1.100:1883") == 0) {
        ESP_LOGW(TAG, "CONFIG_MQTT_BROKER_URI ainda é o placeholder");
    }
}

/* -----------------------------------------------------------------------------
 * app_main
 * -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t err;
    bool critical_ok = true;
    bool network_ok  = false;

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, " ESP32-CAM MQTT + OTA — boot");
    ESP_LOGI(TAG, "===============================================");

    warn_placeholder_credentials();

    /* -------------------------------------------------------------------------
     * 1) NVS
     * ---------------------------------------------------------------------- */

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar NVS: %s", esp_err_to_name(err));
        force_reboot("NVS init failed", 3000);
    }
    ESP_LOGI(TAG, "NVS inicializado");

    /* -------------------------------------------------------------------------
     * 2) Subsistema OTA
     * ---------------------------------------------------------------------- */

    err = ota_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar OTA: %s", esp_err_to_name(err));
        /* Não é crítico para o funcionamento normal — sem OTA, mas com câmera,
         * LEDs, etc. o firmware ainda faz o que precisa. */
    }

    /* -------------------------------------------------------------------------
     * 3) Diagnóstico de boot
     * ---------------------------------------------------------------------- */

    log_boot_info();

    /* -------------------------------------------------------------------------
     * 4) Atuadores + self-test rápido
     * ---------------------------------------------------------------------- */

    err = actuators_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar atuadores: %s",
                 esp_err_to_name(err));
        critical_ok = false;
    } else {
        /* Self-test rápido: acende verde, vermelho, beep, sweep do servo.
         * Dura ~1 segundo. Prova em hardware que GPIO/PWM funcionam. */
        err = actuators_self_test();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Self-test dos atuadores falhou: %s",
                     esp_err_to_name(err));
            critical_ok = false;
        } else {
            ESP_LOGI(TAG, "Self-test dos atuadores: OK");
        }
    }

    /* -------------------------------------------------------------------------
     * 5) Câmera + captura de teste
     * ---------------------------------------------------------------------- */

    err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar câmera: %s", esp_err_to_name(err));
        critical_ok = false;
    } else {
        camera_frame_t *test_fb = NULL;
        err = camera_capture(&test_fb);
        if (err != ESP_OK || test_fb == NULL) {
            ESP_LOGE(TAG, "Self-test da câmera falhou: %s",
                     esp_err_to_name(err));
            critical_ok = false;
        } else {
            ESP_LOGI(TAG, "Self-test da câmera: OK (%u bytes)",
                     (unsigned)test_fb->len);
            camera_release(test_fb);
        }
    }

    /* -------------------------------------------------------------------------
     * 6) Wi-Fi
     * ---------------------------------------------------------------------- */

    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar Wi-Fi: %s", esp_err_to_name(err));
    } else {
        err = wifi_wait_connected(CONFIG_WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi não conectou em %d ms: %s",
                     CONFIG_WIFI_CONNECT_TIMEOUT_MS, esp_err_to_name(err));
            ESP_LOGW(TAG, "Tasks continuarão tentando em background");
        } else {
            network_ok = true;
            ESP_LOGI(TAG, "Wi-Fi conectado");
        }
    }

    /* -------------------------------------------------------------------------
     * 7) MQTT (só se Wi-Fi está up)
     * ---------------------------------------------------------------------- */

    if (network_ok) {
        err = mqtt_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao inicializar MQTT: %s",
                     esp_err_to_name(err));
            network_ok = false;
        } else {
            err = mqtt_wait_connected(CONFIG_MQTT_CONNECT_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT não conectou em %d ms: %s",
                         CONFIG_MQTT_CONNECT_TIMEOUT_MS,
                         esp_err_to_name(err));
                ESP_LOGW(TAG, "Task de comunicação continuará tentando");
                network_ok = false;
            } else {
                ESP_LOGI(TAG, "MQTT conectado");
            }
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi não conectado — init do MQTT adiado");
    }

    /* -------------------------------------------------------------------------
     * 8) Confirmação de boot (rollback)
     * ---------------------------------------------------------------------- */

    if (critical_ok) {
        /*
         * Um firmware novo confirmou: câmera e atuadores funcionam.
         * Rede pode estar down — é problema transitório, não do firmware.
         */
        if (ota_is_pending_verify()) {
            err = ota_mark_app_valid();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Firmware confirmado — rollback cancelado");
            } else {
                ESP_LOGW(TAG, "Não foi possível confirmar firmware: %s",
                         esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Firmware já confirmado (não é boot pós-OTA)");
        }

        if (!network_ok) {
            ESP_LOGW(TAG, "Rede indisponível — tasks vão tentar reconectar");
        }
    } else {
        /*
         * Falha crítica (câmera ou atuadores). Se estamos em boot pós-OTA,
         * a nova imagem é considerada ruim → rollback.
         */
        if (ota_is_pending_verify()) {
            ESP_LOGE(TAG, "Self-test crítico falhou E PENDING_VERIFY ativo");
            ESP_LOGE(TAG, "Disparando rollback para a partição anterior...");
            ota_mark_app_invalid_and_reboot();   /* não retorna em sucesso */
            /* Só chegamos aqui se o rollback falhou. */
            force_reboot("rollback falhou", 3000);
        } else {
            ESP_LOGE(TAG, "Self-test crítico falhou (boot não é OTA)");
            force_reboot("self-test falhou", 5000);
        }
    }

    /* -------------------------------------------------------------------------
     * 9) Start das tasks
     * ---------------------------------------------------------------------- */

    err = tasks_start_all();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar tasks: %s", esp_err_to_name(err));
        force_reboot("tasks_start_all falhou", 3000);
    }
    ESP_LOGI(TAG, "Tasks iniciadas — app_main retornando");

    /* -------------------------------------------------------------------------
     * 10) Servidor HTTPS local de OTA (opcional, não-crítico)
     * ---------------------------------------------------------------------- */

    err = ota_local_server_start(OTA_DEFAULT_LOCAL_PORT);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Servidor HTTPS local de OTA ativo (porta %u)",
                 (unsigned)OTA_DEFAULT_LOCAL_PORT);
    } else {
        ESP_LOGW(TAG, "Servidor HTTPS local não iniciou: %s "
                      "(OTA remoto continua disponível)",
                 esp_err_to_name(err));
    }

    /* -------------------------------------------------------------------------
     * 11) Fim
     * -------------------------------------------------------------------------
     * Nenhum loop aqui. As tasks assumem. A task do FreeRTOS que executa
     * app_main se auto-deleta após retornar.
     */
    ESP_LOGI(TAG, "app_main concluído — sistema em regime");
}
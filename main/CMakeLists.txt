# =============================================================================
# main/CMakeLists.txt — Registro do componente "main"
# =============================================================================
#
# Este arquivo faz três coisas:
#
#   1. Registra o componente "main" no sistema de build do ESP-IDF,
#      listando os arquivos .c e os componentes dos quais dependemos.
#
#   2. Gera main/certs.h (certificado auto-assinado do servidor HTTPS
#      de OTA local) na primeira compilação, chamando tools/gen_cert.sh.
#      Builds subsequentes reusam o arquivo existente.
#
#   3. Liga a geração de certs.h à compilação do componente, de modo
#      que ota_server.c sempre encontre o header pronto.
# =============================================================================

idf_component_register(

    # -------------------------------------------------------------------------
    # SRCS — Arquivos .c compilados para o firmware
    # -------------------------------------------------------------------------
    SRCS
        "main.c"
        "camera.c"
        "wifi.c"
        "mqtt.c"
        "actuators.c"
        "state_machine.c"
        "tasks.c"
        "ota_common.c"
        "ota_client.c"
        "ota_server.c"
        "app_config.c"
        "wifi_provisioning.c"
        "factory_reset_button.c"

    # -------------------------------------------------------------------------
    # INCLUDE_DIRS — Diretórios de headers públicos
    # -------------------------------------------------------------------------
    # "." = a própria pasta "main/". Permite #include "camera.h" de qualquer
    # outro componente, e também #include "certs.h" (gerado pelo build).
    INCLUDE_DIRS
        "."

    # -------------------------------------------------------------------------
    # REQUIRES — Componentes do ESP-IDF dos quais dependemos
    # -------------------------------------------------------------------------
    REQUIRES
        freertos            # Tasks, queues, event groups, semáforos, mutex
        driver              # gpio_config, ledc_*  (LEDs, buzzer, servo)
        esp_wifi            # esp_wifi_init, esp_wifi_connect
        esp_event           # esp_event_loop_*, event handlers
        esp_netif           # esp_netif_init, DHCP, IP_EVENT_STA_GOT_IP
        nvs_flash           # Armazenamento das credenciais Wi-Fi
        esp_psram
        esp_driver_gpio
        mqtt                # esp_mqtt_client_*
        log                 # esp_log.h
        cjson
        esp_timer           # esp_timer_get_time
        esp32-camera        # esp_camera_init, esp_camera_fb_get
                            # (componente externo — ver idf_component.yml)

        # --- Módulo de OTA ---
        app_update
        esp_http_client     # Cliente HTTPS para download (ota_client.c)
        esp_https_server    # Servidor HTTPS local (ota_server.c)
        esp_http_server     # Requerido transitivamente por esp_https_server
        mbedtls             # SHA-256 (ota_common.c)
)

# =============================================================================
# Geração de main/certs.h
# =============================================================================
#
# O servidor HTTPS local (ota_server.c) precisa de certificado e chave
# privada embutidos no firmware. Eles são gerados por tools/gen_cert.sh
# em formato de array C, dentro de main/certs.h.
#
# Esse header NÃO deve ser versionado no Git (cada placa / dev gera o seu).
# Está no .gitignore. O build cuida de criá-lo na primeira vez.
#
# Regra de idempotência: add_custom_command só executa se o arquivo de
# OUTPUT não existir. Ou seja, depois do primeiro build, certs.h é
# reusado indefinidamente (a menos que você rode o script com --force,
# ou apague o arquivo).
#
# O script detecta paths relativos ao próprio diretório ($BASH_SOURCE),
# portanto WORKING_DIRECTORY não é estritamente necessário — mas deixamos
# explícito para clareza.

# Localiza explicitamente o Git Bash no Windows, evitando o bash.exe do WSL.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/certs.h"
    COMMAND "C:/Program Files/Git/bin/bash.exe"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_cert.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Gerando main/certs.h (certificado auto-assinado para OTA local)"
    VERBATIM
)

add_custom_target(gen_certs
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/certs.h"
)

# Garante que o componente "main" (e, portanto, ota_server.c) só seja
# compilado depois que certs.h existir. Sem esta linha, o build pode
# falhar com "fatal error: certs.h: No such file or directory" no
# primeiro build, mesmo com o add_custom_command correto.
add_dependencies(${COMPONENT_LIB} gen_certs)
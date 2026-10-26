/* =============================================================================
 * main/config.h — Configuração centralizada do firmware
 * =============================================================================
 *
 * Nenhum outro arquivo deve conter números mágicos ou credenciais.
 * Tudo o que é ajustável vive aqui.
 *
 * Organização:
 *   1. Credenciais e broker MQTT (EDITE ANTES DE COMPILAR)
 *   2. Tópicos MQTT
 *   3. Pinagem dos atuadores
 *   4. Pinagem da câmera OV5640 (ESP32-S3 CAM N16R8)
 *   5. Parâmetros da câmera
 *   6. Parâmetros dos atuadores (servo, buzzer)
 *   7. Protocolo de fragmentação MQTT
 *   8. Timeouts e parâmetros de rede
 *   9. Tamanhos de stack e prioridades das tasks
 *  10. Constantes de diagnóstico
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"   // garante que as macros CONFIG_* estejam visíveis

/* =============================================================================
 * 1. CREDENCIAIS E BROKER MQTT — EDITE ESTES VALORES ANTES DE COMPILAR
 * =============================================================================
 *
 * Os valores abaixo são PLACEHOLDERS. Substitua pelos da sua rede.
 * Se você esquecer de trocar, o compilador emite #warning (ver abaixo).
 *
 * Em produção, migrar para Kconfig (menuconfig): ver README, seção
 * "Melhorias futuras".
 * ========================================================================== */

 // As credenciais vêm do Kconfig (menuconfig).
// Os fallbacks abaixo só são usados se a opção não existir (proteção extra).
/* --- Wi-Fi --- */
#ifndef CONFIG_LOCKMAXXING_WIFI_SSID
#define CONFIG_LOCKMAXXING_WIFI_SSID       "myssid"
#endif

#ifndef CONFIG_LOCKMAXXING_WIFI_PASSWORD
#define CONFIG_LOCKMAXXING_WIFI_PASSWORD   "mypassword"
#endif

#define CONFIG_WIFI_MAX_RETRY       10
#define CONFIG_WIFI_RETRY_DELAY_MS  3000


/* --- Broker MQTT --- */
#ifndef CONFIG_LOCKMAXXING_MQTT_BROKER_URI
#define CONFIG_LOCKMAXXING_MQTT_BROKER_URI "mqtt://192.168.1.100:1883"
#endif

#ifndef CONFIG_LOCKMAXXING_MQTT_USERNAME
#define CONFIG_LOCKMAXXING_MQTT_USERNAME ""
#endif

#ifndef CONFIG_LOCKMAXXING_MQTT_PASSWORD
#define CONFIG_LOCKMAXXING_MQTT_PASSWORD ""
#endif

#ifndef CONFIG_LOCKMAXXING_MQTT_CLIENT_ID
#define CONFIG_LOCKMAXXING_MQTT_CLIENT_ID "esp32-camera-01"
#endif


/* =============================================================================
 * 2. TÓPICOS MQTT
 * =============================================================================
 *
 * Protocolo de três tópicos:
 *
 *   <base>/image    — ESP32 publica cada chunk da imagem (binário + header)
 *   <base>/result   — servidor publica o resultado em JSON
 *   <base>/request  — ESP32 publica "vou enviar" (metadados antes dos chunks)
 *
 * O sufixo "request" carrega JSON com {id, total_chunks, sha256_prefix}.
 * O servidor usa esses dados para alocar buffer e conferir integridade.
 * ========================================================================== */

#define CONFIG_MQTT_TOPIC_BASE      "lockmaxxing/camera"
#define CONFIG_MQTT_TOPIC_IMAGE     CONFIG_MQTT_TOPIC_BASE "/image"
#define CONFIG_MQTT_TOPIC_RESULT    CONFIG_MQTT_TOPIC_BASE "/result"
#define CONFIG_MQTT_TOPIC_REQUEST   CONFIG_MQTT_TOPIC_BASE "/request"

/* QoS escolhido: 1 (at-least-once). Cada chunk é pequeno; perder um
 * significa refazer a imagem inteira. QoS 1 garante entrega, e o servidor
 * detecta duplicatas pelo índice do chunk. */
#define CONFIG_MQTT_QOS             1

/* Keep-alive do MQTT. 60s é o padrão do esp-mqtt e cobre NAT timeout. */
#define CONFIG_MQTT_KEEPALIVE_S     60

/* Reconexão automática do esp-mqtt quando a conexão cai. */
#define CONFIG_MQTT_RECONNECT_MS    5000

/* Timeout para aguardar a resposta do servidor após o envio da imagem.
 * Se estourar, o ciclo volta para IDLE sem acionar atuadores. */
#define CONFIG_MQTT_RESULT_TIMEOUT_MS  30000

/* =============================================================================
 * 3. PINAGEM DOS ATUADORES
 * =============================================================================
 *
 * Escolha baseada na ESP32-S3 CAM N16R8. Critérios:
 *   - Evitar GPIOs de strapping (0, 45, 46)
 *   - Evitar GPIOs de USB (19, 20)
 *   - Evitar GPIOs da PSRAM octal (33–37) e da câmera
 *   - Evitar GPIOs da flash (26–32)
 *
 * Pinos escolhidos: 1, 2, 3, 21 — todos livres na N16R8.
 * ========================================================================== */

/* LEDs */
#define CONFIG_GPIO_LED_GREEN       1
#define CONFIG_GPIO_LED_RED         2

/* Buzzer (ativo — liga/desliga simples) */
#define CONFIG_GPIO_BUZZER          3

/* Servomotor (via LEDC — PWM em canal dedicado) */
#define CONFIG_GPIO_SERVO           21

/* Nível lógico que ACENDE cada componente (placa pode inverter). */
#define CONFIG_LEVEL_LED_ON         1
#define CONFIG_LEVEL_BUZZER_ON      1

/* =============================================================================
 * 4. PINAGEM DA CÂMERA OV5640 (ESP32-S3 CAM N16R8)
 * =============================================================================
 *
 * A N16R8 usa o barramento DVP padrão CAMERA_MODEL_ESP32S3_EYE, compatível
 * com OV5640 e OV3660. A pinagem abaixo é a mesma para os dois sensores.
 * ========================================================================== */

#define CONFIG_CAM_PIN_PWDN         -1
#define CONFIG_CAM_PIN_RESET        -1
#define CONFIG_CAM_PIN_XCLK         15
#define CONFIG_CAM_PIN_SIOD         4
#define CONFIG_CAM_PIN_SIOC         5

#define CONFIG_CAM_PIN_D7           16
#define CONFIG_CAM_PIN_D6           17
#define CONFIG_CAM_PIN_D5           18
#define CONFIG_CAM_PIN_D4           12
#define CONFIG_CAM_PIN_D3           10
#define CONFIG_CAM_PIN_D2           8
#define CONFIG_CAM_PIN_D1           9
#define CONFIG_CAM_PIN_D0           11

#define CONFIG_CAM_PIN_VSYNC        6
#define CONFIG_CAM_PIN_HREF         7
#define CONFIG_CAM_PIN_PCLK         13

/* =============================================================================
 * 5. PARÂMETROS DA CÂMERA (OV5640)
 * =============================================================================
 *
 * OV5640 suporta até QSXGA (2560×1920), mas para transmissão MQTT
 * mantemos SVGA (800×600) como padrão. Aumente para UXGA ou QSXGA
 * apenas se o tempo de transmissão for aceitável.
 *
 * OV5640 pode precisar de XCLK 20 MHz ou 24 MHz dependendo do módulo.
 * 20 MHz é o mais comum e funciona na maioria dos casos.
 * ========================================================================== */

#define CONFIG_CAM_XCLK_FREQ_HZ     10000000
#define CONFIG_CAM_FRAMESIZE        FRAMESIZE_SVGA
#define CONFIG_CAM_JPEG_QUALITY     12
#define CONFIG_CAM_FB_COUNT         1
#define CONFIG_CAM_GRAB_MODE        CAMERA_GRAB_LATEST
#define CONFIG_CAM_PIXEL_FORMAT     PIXFORMAT_JPEG

/* =============================================================================
 * 6. PARÂMETROS DOS ATUADORES
 * ========================================================================== */

/* --- Servo (SG90) --- */
/* SG90 aceita pulsos entre 500 µs (0°) e 2400 µs (180°), com período
 * de 20 ms (50 Hz). O LEDC do S3 gera isso com resolução de 13 bits
 * em frequência de 50 Hz.
 *
 *   duty = (pulse_us / period_us) * (2^13 - 1)
 *         = (pulse_us / 20000)   * 8191
 *
 * 500  µs → duty ~205
 * 1500 µs → duty ~614  (centro)
 * 2400 µs → duty ~983  (180°)
 */
#define CONFIG_SERVO_FREQ_HZ        50
#define CONFIG_SERVO_TIMER_RES      13          /* bits de resolução */
#define CONFIG_SERVO_TIMER          1           /* NOVO */
#define CONFIG_SERVO_CHANNEL        1           /* canal LEDC */
#define CONFIG_SERVO_DUTY_MIN       205         /* 0°   */
#define CONFIG_SERVO_DUTY_MAX       983         /* 180° */
/* Estado de repouso e estado de aprovação — devem ser DIFERENTES
 * para que o movimento seja visível. */
#define CONFIG_SERVO_DUTY_HOME       CONFIG_SERVO_DUTY_MIN     /* ~0°, repouso */
#define CONFIG_SERVO_DUTY_APPROVED   CONFIG_SERVO_DUTY_MAX     /* ~180°, aprovado */
#define CONFIG_SERVO_HOLD_MS        1500        /* tempo parado na posição */
#define CONFIG_SERVO_RETURN_MS      800         /* tempo de retorno suave */

/* --- Buzzer --- */
/* Buzzer ativo (tem oscilador interno): só precisa ligar/desligar o GPIO.
 * Se o seu for passivo, troque o módulo para gerar tom via LEDC. */
#define CONFIG_BUZZER_DURATION_MS   3000        /* 3 s contínuos no "reprovado" */

/* --- LEDs --- */
#define CONFIG_LED_APPROVED_MS      2000        /* LED verde ligado após aprovado */
#define CONFIG_LED_REJECTED_MS      3000        /* LED vermelho piscando */

/* =============================================================================
 * 7. PROTOCOLO DE FRAGMENTAÇÃO MQTT
 * =============================================================================
 *
 * Formato de cada chunk publicado em CONFIG_MQTT_TOPIC_IMAGE:
 *
 *   [0..3]  image_id      uint32 big-endian   (identifica esta captura)
 *   [4..5]  chunk_index   uint16 big-endian   (0..total_chunks-1)
 *   [6..7]  total_chunks  uint16 big-endian
 *   [8..N]  payload       bytes do JPEG
 *
 * Tamanho total da mensagem: 8 + CONFIG_MQTT_CHUNK_SIZE.
 *
 * Escolha de 2048 bytes por chunk: cabe folgado no CONFIG_MQTT_BUFFER_SIZE
 * (4096) definido em sdkconfig.defaults, com espaço para tópico e overhead.
 * ========================================================================== */

#define CONFIG_MQTT_CHUNK_SIZE          2048
#define CONFIG_MQTT_CHUNK_HEADER_SIZE   8

/* ID da imagem é gerado a partir de esp_timer_get_time() truncado. */
#define CONFIG_IMAGE_MAX_SIZE           (200 * 1024)    /* 200 KB hard limit */

/* =============================================================================
 * 8. TIMEOUTS E PARÂMETROS DE REDE
 * ========================================================================== */

#define CONFIG_WIFI_CONNECT_TIMEOUT_MS  20000
#define CONFIG_MQTT_CONNECT_TIMEOUT_MS  15000
#define CONFIG_CAMERA_CAPTURE_TIMEOUT_MS 5000

/* Intervalo entre capturas automáticas quando em IDLE. Se você quiser
 * disparar manualmente (botão, MQTT), mude para 0 e implemente o trigger. */
#ifndef CONFIG_LOCKMAXXING_CAPTURE_INTERVAL_MS
#define CONFIG_LOCKMAXXING_CAPTURE_INTERVAL_MS 30000
#endif

/* =============================================================================
 * 9. STACKS E PRIORIDADES DAS TASKS
 * =============================================================================
 *
 * Ordem de prioridade (maior = mais urgente):
 *   5: wifi_mqtt_task    — manter a rede viva sempre
 *   4: camera_task       — captura é latência-sensível
 *   3: comm_task         — envio/recepção MQTT
 *   2: actuator_task     — controle dos periféricos
 *   1: idle do FreeRTOS  — reservada pelo sistema
 *
 * Stacks em bytes. 4 KB para camera_task porque o driver esp_camera usa
 * bastante stack (configuração de sensor, JPEG encode).
 * ========================================================================== */

#define CONFIG_TASK_WIFI_MQTT_PRIO      5
#define CONFIG_TASK_WIFI_MQTT_STACK     4096

#define CONFIG_TASK_CAMERA_PRIO         4
#define CONFIG_TASK_CAMERA_STACK        6144

#define CONFIG_TASK_COMM_PRIO           3
#define CONFIG_TASK_COMM_STACK          6144

#define CONFIG_TASK_ACTUATOR_PRIO       2
#define CONFIG_TASK_ACTUATOR_STACK      3072

/* Núcleos (S3 é dual-core). Distribuímos para não competir por cache:
 * rede no Core 0 (junto com a pilha TCP/IP), periféricos no Core 1. */
#define CONFIG_TASK_WIFI_MQTT_CORE      0
#define CONFIG_TASK_COMM_CORE           0
#define CONFIG_TASK_CAMERA_CORE         1
#define CONFIG_TASK_ACTUATOR_CORE       1

/* =============================================================================
 * 10. CONSTANTES DE DIAGNÓSTICO
 * ========================================================================== */

/* Tags de log por módulo. Centralizadas aqui para facilitar filtro no monitor. */
#define CONFIG_LOG_TAG_MAIN             "APP"
#define CONFIG_LOG_TAG_CAMERA           "CAMERA"
#define CONFIG_LOG_TAG_WIFI             "WIFI"
#define CONFIG_LOG_TAG_MQTT             "MQTT"
#define CONFIG_LOG_TAG_ACTUATOR         "ACTUATOR"
#define CONFIG_LOG_TAG_STATE            "STATE"
#define CONFIG_LOG_TAG_TASKS            "TASKS"

#endif /* CONFIG_H */
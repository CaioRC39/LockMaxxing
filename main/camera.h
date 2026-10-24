/* =============================================================================
 * main/camera.h — API pública do módulo de câmera (OV5640)
 * =============================================================================
 *
 * Encapsula o driver esp32-camera. O resto do firmware NÃO inclui
 * "esp_camera.h" — apenas "camera.h".
 *
 * Sensor alvo: OV5640 (5 MP), na ESP32-S3 CAM N16R8.
 * A pinagem DVP usada é a padrão CAMERA_MODEL_ESP32S3_EYE (mesma do
 * OV3660), definida em config.h.
 *
 * Responsabilidades do módulo:
 *   - Inicializar o sensor com a configuração de config.h
 *   - Aplicar ajustes específicos do OV5640 (flip vertical, clock)
 *   - Capturar um frame JPEG e devolver um handle opaco
 *   - Liberar o framebuffer (devolver à PSRAM)
 *
 * O frame NÃO é copiado. camera_capture() devolve um ponteiro para o
 * buffer interno do driver (em PSRAM). O chamador deve chamar
 * camera_release() assim que terminar de usar os bytes.
 * ========================================================================== */

#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Handle opaco para um frame capturado
 * -------------------------------------------------------------------------- */

/*
 * O tipo real é camera_fb_t (definido em esp_camera.h). Aqui usamos um
 * typedef opaco para que quem inclui camera.h não precise incluir
 * esp_camera.h.
 *
 * Acesso aos bytes:
 *   frame->buf   → ponteiro para o JPEG (uint8_t *)
 *   frame->len   → tamanho em bytes (size_t)
 *   frame->width / frame->height → dimensões
 *
 * Esses campos são públicos no esp_camera.h; o typedef abaixo só renomeia
 * o tipo. Se um dia você trocar de driver, mantém os mesmos campos e o
 * resto do firmware não muda.
 */
typedef camera_fb_t camera_frame_t;

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa a câmera e o sensor OV5640.
 *
 * Lê a configuração de config.h (pinos, resolução, qualidade JPEG),
 * chama esp_camera_init() e, em seguida, aplica ajustes específicos
 * do OV5640 (flip vertical, etc.).
 *
 * Deve ser chamada uma única vez, em app_main().
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_NOT_FOUND se nenhum sensor respondeu (cabo flat solto,
 *         pinagem errada, ou alimentação ausente).
 *         ESP_ERR_NO_MEM se a PSRAM não estiver disponível.
 */
esp_err_t camera_init(void);

/**
 * @brief Libera o driver da câmera.
 *
 * Normalmente não é chamada (o firmware reinicia a placa). Útil em
 * testes ou se você quiser alternar entre modos que precisam liberar
 * a PSRAM.
 */
esp_err_t camera_deinit(void);

/* -----------------------------------------------------------------------------
 * Captura
 * -------------------------------------------------------------------------- */

/**
 * @brief Captura um frame JPEG.
 *
 * Bloqueia até um frame estar disponível ou até o timeout configurado
 * em config.h (CONFIG_CAMERA_CAPTURE_TIMEOUT_MS).
 *
 * O frame retornado aponta para memória em PSRAM gerenciada pelo driver.
 * O chamador NÃO deve free()-lo nem modificá-lo. Use camera_release()
 * quando terminar.
 *
 * @param out_frame  Recebe o ponteiro para o frame. Não pode ser NULL.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_ARG se out_frame for NULL.
 *         ESP_ERR_TIMEOUT se nenhum frame chegou no prazo.
 *         ESP_FAIL em erro do driver.
 */
esp_err_t camera_capture(camera_frame_t **out_frame);

/**
 * @brief Devolve o frame ao driver.
 *
 * Deve ser chamado exatamente uma vez para cada frame retornado por
 * camera_capture(). Após esta chamada, o ponteiro fica inválido.
 *
 * @param frame  Frame a liberar. Se NULL, não faz nada.
 */
void camera_release(camera_frame_t *frame);

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

/**
 * @brief Retorna o ID do sensor detectado (ex: OV5640_PID).
 *
 * Útil para logs e para decidir ajustes específicos de sensor
 * sem hardcodar.
 *
 * @return PID do sensor, ou 0 se a câmera não foi inicializada.
 */
uint16_t camera_get_sensor_pid(void);

/**
 * @brief Retorna uma string legível do modelo do sensor.
 *
 * Ex: "OV5640", "OV3660", "OV2640". Retorna "desconhecido" se a
 * câmera não foi inicializada.
 */
const char *camera_get_sensor_name(void);

/* -----------------------------------------------------------------------------
 * Ajustes pós-init (opcional)
 * -------------------------------------------------------------------------- */

/**
 * @brief Acessa o handle do sensor para ajustes finos.
 *
 * Retorna o ponteiro `sensor_t *` do driver (definido em esp_camera.h).
 * Declarado como void* para não vazar esp_camera.h para o resto do firmware.
 *
 * Use apenas se souber o que está fazendo. Exemplos:
 *
 *   sensor_t *s = (sensor_t *)camera_get_sensor();
 *   s->set_brightness(s, 1);
 *   s->set_contrast(s, 1);
 *   s->set_saturation(s, 0);
 *
 * @return Ponteiro para o sensor, ou NULL se não inicializado.
 */
void *camera_get_sensor(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
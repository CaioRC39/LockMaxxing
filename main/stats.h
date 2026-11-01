/* =============================================================================
 * main/stats.h — Contadores e métricas do sistema
 * =============================================================================
 * Thread-safe via spinlock. Incrementado pelos módulos (camera_task,
 * do_send_chunked, process_comm_msg, check_state_timeouts), lido pelo
 * /status handler em ota_server.c.
 *
 * Todos os contadores resetam em reboot. Sem persistência em NVS
 * (evita desgaste de flash por contadores que mudam a cada 30 s).
 * ========================================================================== */

#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    STATS_LAST_RESULT_NONE = 0,
    STATS_LAST_RESULT_APPROVED,
    STATS_LAST_RESULT_REJECTED,
} stats_last_result_t;

typedef struct {
    /* Ciclo de imagens */
    uint32_t images_captured;           /* frames válidos da câmera */
    uint32_t images_sent;               /* imagens publicadas com sucesso */
    uint32_t images_rejected_quality;   /* rejeitadas pelo filtro */
    uint32_t images_rejected_network;   /* falha ao enviar (MQTT/rede) */

    /* Métricas do filtro de qualidade (running average) */
    float    avg_brightness;            /* 0-255 */
    float    avg_sharpness;             /* variância do Laplaciano */

    /* Último resultado recebido */
    stats_last_result_t last_result;

    /* Última mensagem de erro (string curta) */
    char     last_error[64];
} stats_snapshot_t;

void app_stats_init(void);

/* ---- Incrementos (chamados pelos módulos) ---- */
void stats_inc_captured(void);
void stats_inc_sent(void);
void stats_inc_rejected_quality(void);
void stats_inc_rejected_network(void);
void stats_record_quality(float brightness, float sharpness);
void stats_set_last_result(bool approved);
void stats_set_last_error(const char *msg);

/* ---- Snapshot ---- */
void stats_snapshot(stats_snapshot_t *out);

#endif /* STATS_H */
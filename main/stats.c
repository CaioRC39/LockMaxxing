/* =============================================================================
 * main/stats.c — Implementação dos contadores
 * ========================================================================== */

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "stats.h"

static const char *TAG = "STATS";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Estado interno */
static uint32_t s_captured          = 0;
static uint32_t s_sent              = 0;
static uint32_t s_rejected_quality  = 0;
static uint32_t s_rejected_network  = 0;

static double   s_sum_brightness    = 0.0;
static double   s_sum_sharpness     = 0.0;
static uint32_t s_quality_samples   = 0;

static stats_last_result_t s_last_result = STATS_LAST_RESULT_NONE;
static char s_last_error[64] = {0};

void app_stats_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_captured         = 0;
    s_sent             = 0;
    s_rejected_quality = 0;
    s_rejected_network = 0;
    s_sum_brightness   = 0.0;
    s_sum_sharpness    = 0.0;
    s_quality_samples  = 0;
    s_last_result      = STATS_LAST_RESULT_NONE;
    memset(s_last_error, 0, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "Módulo de estatísticas inicializado");
}

void stats_inc_captured(void)
{
    portENTER_CRITICAL(&s_lock);
    s_captured++;
    portEXIT_CRITICAL(&s_lock);
}

void stats_inc_sent(void)
{
    portENTER_CRITICAL(&s_lock);
    s_sent++;
    portEXIT_CRITICAL(&s_lock);
}

void stats_inc_rejected_quality(void)
{
    portENTER_CRITICAL(&s_lock);
    s_rejected_quality++;
    portEXIT_CRITICAL(&s_lock);
}

void stats_inc_rejected_network(void)
{
    portENTER_CRITICAL(&s_lock);
    s_rejected_network++;
    portEXIT_CRITICAL(&s_lock);
}

void stats_record_quality(float brightness, float sharpness)
{
    portENTER_CRITICAL(&s_lock);
    s_sum_brightness += (double)brightness;
    s_sum_sharpness  += (double)sharpness;
    s_quality_samples++;
    portEXIT_CRITICAL(&s_lock);
}

void stats_set_last_result(bool approved)
{
    portENTER_CRITICAL(&s_lock);
    s_last_result = approved ? STATS_LAST_RESULT_APPROVED
                             : STATS_LAST_RESULT_REJECTED;
    portEXIT_CRITICAL(&s_lock);
}

void stats_set_last_error(const char *msg)
{
    if (msg == NULL) msg = "";
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_last_error, msg, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGD(TAG, "last_error = '%s'", msg);
}

void stats_snapshot(stats_snapshot_t *out)
{
    if (out == NULL) return;

    portENTER_CRITICAL(&s_lock);
    out->images_captured         = s_captured;
    out->images_sent             = s_sent;
    out->images_rejected_quality = s_rejected_quality;
    out->images_rejected_network = s_rejected_network;
    out->last_result             = s_last_result;

    if (s_quality_samples > 0) {
        out->avg_brightness = (float)(s_sum_brightness / s_quality_samples);
        out->avg_sharpness  = (float)(s_sum_sharpness  / s_quality_samples);
    } else {
        out->avg_brightness = 0.0f;
        out->avg_sharpness  = 0.0f;
    }

    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    portEXIT_CRITICAL(&s_lock);
}
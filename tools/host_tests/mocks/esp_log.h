/* =============================================================================
 * tools/host_tests/mocks/esp_log.h — Mock de esp_log.h (imprime no terminal)
 * ============================================================================= */

#ifndef ESP_LOG_H_MOCK
#define ESP_LOG_H_MOCK

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) \
    fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
    fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) \
    fprintf(stdout, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) \
    fprintf(stdout, "[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) \
    fprintf(stdout, "[V][%s] " fmt "\n", tag, ##__VA_ARGS__)

#endif /* ESP_LOG_H_MOCK */
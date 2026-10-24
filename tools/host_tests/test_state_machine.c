/* =============================================================================
 * tools/host_tests/test_state_machine.c — Testes de main/state_machine.c
 * =============================================================================
 * Compila o .c REAL (via include) e exercita todas as transições.
 * Se a lógica de state_machine.c mudar, este teste reflete a mudança.
 * ========================================================================== */

#include <stdio.h>
#include <stddef.h>

/* Inclui o arquivo real. Os mocks vêm do include path. */
#include "state_machine.c"

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ(actual, expected, msg) do {                                   \
    long _a = (long)(actual);                                                   \
    long _e = (long)(expected);                                                 \
    if (_a == _e) {                                                             \
        printf("  [PASS] %s\n", msg);                                           \
        g_pass++;                                                               \
    } else {                                                                    \
        printf("  [FAIL] %s (got %ld, expected %ld)\n", msg, _a, _e);          \
        g_fail++;                                                               \
    }                                                                           \
} while (0)

/* ----------------------------------------- Caminho feliz ------------------- */
static void test_happy_path(void)
{
    printf("[CASE] happy path (aprovado)\n");
    sm_init();
    ASSERT_EQ(sm_get_state(), SM_STATE_INIT, "init → INIT");
    sm_process_event(SM_EVENT_NETWORK_READY);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "NETWORK_READY → IDLE");
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    ASSERT_EQ(sm_get_state(), SM_STATE_CAPTURING, "TRIGGER_CAPTURE → CAPTURING");
    sm_process_event(SM_EVENT_CAPTURE_OK);
    ASSERT_EQ(sm_get_state(), SM_STATE_SENDING, "CAPTURE_OK → SENDING");
    sm_process_event(SM_EVENT_SEND_OK);
    ASSERT_EQ(sm_get_state(), SM_STATE_WAITING, "SEND_OK → WAITING");
    sm_process_event(SM_EVENT_RESULT_APPROVED);
    ASSERT_EQ(sm_get_state(), SM_STATE_APPLYING, "RESULT_APPROVED → APPLYING");
    sm_process_event(SM_EVENT_APPLY_DONE);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "APPLY_DONE → IDLE");
}

/* ----------------------------------------- Caminho reprovado -------------- */
static void test_rejected_path(void)
{
    printf("[CASE] caminho reprovado\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_RESULT_REJECTED);
    ASSERT_EQ(sm_get_state(), SM_STATE_APPLYING, "RESULT_REJECTED → APPLYING");
    sm_process_event(SM_EVENT_APPLY_DONE);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "APPLY_DONE → IDLE");
}

/* ----------------------------------------- Todos os caminhos de erro ------ */
static void test_error_paths(void)
{
    printf("[CASE] falha de captura\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_FAILED);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "CAPTURE_FAILED → ERROR");
    sm_process_event(SM_EVENT_RESET);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "RESET → IDLE");

    printf("[CASE] timeout de captura\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_TIMEOUT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "CAPTURE_TIMEOUT → ERROR");

    printf("[CASE] falha de envio\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_FAILED);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "SEND_FAILED → ERROR");

    printf("[CASE] timeout de resultado\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_RESULT_TIMEOUT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "RESULT_TIMEOUT → ERROR");

    printf("[CASE] resultado inválido\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_RESULT_INVALID);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "RESULT_INVALID → ERROR");

    printf("[CASE] network down em SENDING\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_NETWORK_DOWN);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "NETWORK_DOWN em SENDING → ERROR");

    printf("[CASE] network down em WAITING\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_NETWORK_DOWN);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "NETWORK_DOWN em WAITING → ERROR");
}

/* ----------------------------------------- Eventos inválidos são ignorados */
static void test_invalid_events_ignored(void)
{
    printf("[CASE] eventos inválidos em IDLE são ignorados\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "setup: em IDLE");

    sm_process_event(SM_EVENT_CAPTURE_OK);        /* inválido em IDLE */
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "CAPTURE_OK em IDLE ignorado");
    sm_process_event(SM_EVENT_SEND_OK);           /* inválido em IDLE */
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "SEND_OK em IDLE ignorado");
    sm_process_event(SM_EVENT_RESULT_APPROVED);   /* inválido em IDLE */
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "RESULT_APPROVED em IDLE ignorado");
    sm_process_event(SM_EVENT_APPLY_DONE);        /* inválido em IDLE */
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "APPLY_DONE em IDLE ignorado");
    sm_process_event(SM_EVENT_RESET);             /* inválido em IDLE */
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "RESET em IDLE ignorado");
}

/* ----------------------------------------- NETWORK_DOWN em IDLE não aborta */
static void test_network_down_in_idle(void)
{
    printf("[CASE] NETWORK_DOWN em IDLE não aborta\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_NETWORK_DOWN);
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "NETWORK_DOWN em IDLE mantém IDLE");
}

/* ----------------------------------------- ABORT de qualquer estado ------- */
static void test_abort_from_any_state(void)
{
    printf("[CASE] ABORT de qualquer estado → ERROR\n");
    /* Aborta de INIT */
    sm_init();
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em INIT → ERROR");

    /* Aborta de IDLE */
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em IDLE → ERROR");

    /* Aborta de CAPTURING */
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em CAPTURING → ERROR");

    /* Aborta de SENDING */
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em SENDING → ERROR");

    /* Aborta de WAITING */
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em WAITING → ERROR");

    /* Aborta de APPLYING */
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    sm_process_event(SM_EVENT_CAPTURE_OK);
    sm_process_event(SM_EVENT_SEND_OK);
    sm_process_event(SM_EVENT_RESULT_APPROVED);
    sm_process_event(SM_EVENT_ABORT);
    ASSERT_EQ(sm_get_state(), SM_STATE_ERROR, "ABORT em APPLYING → ERROR");
}

/* ----------------------------------------- força idle --------------------- */
static void test_force_idle(void)
{
    printf("[CASE] sm_force_idle de qualquer estado\n");
    sm_init();
    sm_process_event(SM_EVENT_NETWORK_READY);
    sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
    ASSERT_EQ(sm_get_state(), SM_STATE_CAPTURING, "setup: em CAPTURING");
    sm_force_idle();
    ASSERT_EQ(sm_get_state(), SM_STATE_IDLE, "force_idle de CAPTURING → IDLE");
}

/* ----------------------------------------- nomes dos estados/eventos ------ */
static void test_names(void)
{
    printf("[CASE] nomes legíveis\n");
    ASSERT_EQ((long)sm_get_state_name(SM_STATE_IDLE)[0], (long)'I',
              "nome de IDLE começa com 'I'");
    ASSERT_EQ((long)sm_get_event_name(SM_EVENT_ABORT)[0], (long)'A',
              "nome de ABORT começa com 'A'");
    ASSERT_EQ((long)sm_get_state_name((sm_state_t)999)[0], (long)'U',
              "estado inválido retorna 'UNKNOWN'");
    ASSERT_EQ((long)sm_get_event_name((sm_event_t)999)[0], (long)'U',
              "evento inválido retorna 'UNKNOWN'");
}

int main(void)
{
    printf("=== Testes de state_machine ===\n");
    test_happy_path();
    test_rejected_path();
    test_error_paths();
    test_invalid_events_ignored();
    test_network_down_in_idle();
    test_abort_from_any_state();
    test_force_idle();
    test_names();

    printf("\n=== Resultado: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
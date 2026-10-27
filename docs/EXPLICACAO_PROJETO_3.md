# Explicação Consolidada — Parte 3/6: Arquitetura do Firmware

Esta parte responde a três perguntas: *por que o projeto está dividido nesses arquivos*, *como o build realmente funciona* e *como a máquina de estados orquestra tudo sem bloquear*. Tudo referenciado ao seu código real.

---

## 1. Por que modularizar (e o que acontece se você não modularizar)

Você tem **28 arquivos**. Se você tivesse escrito o firmware em um único `main.c`, ele teria ~5.000 linhas. Consequências concretas:

| Aspecto | Arquivo único | 28 arquivos |
|---------|---------------|-------------|
| **Navegação** | `Ctrl+F` para tudo | Cada coisa no seu arquivo |
| **Compilação** | Toda mudança recompila 5.000 linhas | `ninja` recompila só o `.o` que mudou (~30 s) |
| **Teste unitário** | Impossível sem hardware | Testável no PC (`tools/host_tests/`) |
| **Reuso** | Copiar 5.000 linhas para outro projeto | Copiar `camera.c`/`camera.h` |
| **Colaboração** | Conflito de merge em cada PR | Cada dev numa pasta |
| **Debug** | Stack traces longos, difícil saber de onde veio o bug | `ESP_LOGE(TAG_CAMERA, ...)` diz o módulo |

O mais importante: **modularização é o que tornou os 78 host tests possíveis**. Sem `state_machine.c` isolado, você não conseguiria testar transições no PC. Sem `camera.h` opaco, o `main.c` dependeria do `esp_camera.h` inteiro e seria impossível testá-lo sem hardware.

---

## 2. Os 28 arquivos, por responsabilidade

### Raiz — configuração de build (6 arquivos)

| Arquivo | Responsabilidade |
|---------|-----------------|
| `CMakeLists.txt` | Define o projeto, inclui `project.cmake` do IDF. Só isso. |
| `sdkconfig.defaults` | Configurações **do IDF** (target, PSRAM, watchdog, buffers MQTT). Editado no `menuconfig`, não no código. |
| `partitions.csv` | Tabela de partições (nvs, otadata, ota_0, ota_1). |
| `.gitignore` | Ignora `build/`, `sdkconfig`, `main/certs.h`, etc. |
| `dependencies.lock` | Versões exatas dos componentes baixados. |
| `README.md` | Documentação. |

### `main/` — configuração e entrada (3 arquivos)

| Arquivo | Responsabilidade |
|---------|-----------------|
| `CMakeLists.txt` | Lista todos os `.c`, `REQUIRES` (componentes), e a **regra que gera `certs.h`** via `gen_cert.sh`. |
| `idf_component.yml` | Declara dependências externas: `esp32-camera`, `cjson`. |
| `Kconfig.projbuild` | Opções do `menuconfig` (SSID, senha, broker, warmup). |

### `main/` — código de aplicação (13 arquivos)

| Arquivo | Responsabilidade | Depende de |
|---------|-----------------|-----------|
| `main.c` | `app_main`: init sequencial + self-test + start tasks | Todos os módulos |
| `config.h` | Constantes (pinos, timeouts, protocolo) + referências a `CONFIG_*` do Kconfig | — |
| `camera.h/.c` | Wrapper do `esp_camera.h`: `camera_init`, `camera_capture`, `camera_release` | `esp32-camera` |
| `wifi.h/.c` | Station com backoff exponencial, event groups | `esp_wifi`, `esp_event` |
| `mqtt.h/.c` | Cliente MQTT: publish de chunks, parse de resultado (cJSON) | `mqtt`, `cjson` |
| `actuators.h/.c` | LEDs, buzzer, servo via LEDC | `driver/ledc` |
| `state_machine.h/.c` | Autômato puro: `(estado, evento) → novo estado` | Nada (só `esp_log`) |
| `tasks.h/.c` | 4 tasks, filas, orquestração, callback MQTT | Todos os módulos |

### `main/` — OTA (5 arquivos)

| Arquivo | Responsabilidade |
|---------|-----------------|
| `ota.h` | API pública (vista por `tasks.c`, `main.c`) |
| `ota_internal.h` | Contrato interno (só os três `.c` abaixo veem) |
| `ota_common.c` | Init, confirmação/rollback, partições, SHA-256 (PSA Crypto) |
| `ota_client.c` | Cliente HTTPS (pull) com retry, SHA-256 streaming, `common_name` para CN match |
| `ota_server.c` | Servidor HTTPS (push) + `/status` + `/pull-ota` + warmup |

### `tools/` — ferramentas de PC (6 arquivos)

| Arquivo | Responsabilidade |
|---------|-----------------|
| `gen_cert.sh` | Gera `certs.h` (cert auto-assinado) no build, se não existir |
| `mqtt_test_server.py` | Servidor MQTT de análise simulada: reconstrói JPEGs, decide resultado |
| `ota_test_server.py` | Servidor HTTPS push + pull + utilitário `sha256` |
| `host_tests/mocks/*` | Mocks de `esp_err.h`, `esp_log.h` para compilar no PC |
| `host_tests/test_*.c` | 4 suítes, 78 testes |
| `host_tests/run_all.{sh,ps1}` | Compila e roda os testes no PC |

---

## 3. CMake em detalhe

Há **três** `CMakeLists.txt` no seu projeto. Vou explicar cada um.

### `CMakeLists.txt` (raiz) — o porteiro

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-camera-mqtt)
```

Cinco linhas. Não lista `.c`, não define pinos, não configura Wi-Fi. Ele só:

1. Exige CMake 3.16+
2. Importa `project.cmake` do IDF (que traz toolchain, componentes, bootloader, partition table)
3. Declara o nome do projeto (vira nome do `.bin`)

**Por que ele não muda?** Porque é **agnóstico ao target**. `esp32` vs `esp32s3` é escolhido via `idf.py set-target` ou `CONFIG_IDF_TARGET` no `sdkconfig`, não aqui. É uma decisão de design do IDF: separar "quais arquivos compilar" de "para qual chip compilar".

### `main/CMakeLists.txt` — o mapa do firmware

Este tem conteúdo concreto. Três blocos:

**Bloco `SRCS`:** lista os 10 `.c` compilados. Nenhum header aparece aqui — headers são incluídos por `#include`, não compilados diretamente.

**Bloco `REQUIRES`:** lista os componentes do IDF dos quais você depende. Cada item garante que os headers daquele componente estejam no `-I` path:

```cmake
    REQUIRES
        freertos        # xTaskCreate, xQueueSend, xEventGroup*
        driver          # gpio_config, ledc_*
        esp_wifi        # esp_wifi_init, esp_wifi_connect
        esp_event       # esp_event_loop_create_default
        esp_netif       # esp_netif_init, IP_EVENT_STA_GOT_IP
        nvs_flash       # nvs_flash_init
        mqtt            # esp_mqtt_client_init
        esp_timer       # esp_timer_get_time
        esp32-camera    # esp_camera_init (externo, via idf_component.yml)
        app_update      # esp_ota_ops.h (OTA)
        log             # esp_log.h
        esp_http_client # cliente HTTPS (pull)
        esp_https_server# servidor HTTPS (push)
        esp_http_server # transitivo de esp_https_server
        esp_crt_bundle  # esp_crt_bundle_attach
        mbedtls         # SHA-256 (via PSA, mas o componente é necessário)
        espressif__cjson# parser JSON (externo)
```

Se você esquecer `mqtt`, o compilador diz `mqtt_client.h: No such file or directory`. Se esquecer `app_update`, o linker falha com `undefined reference to esp_ota_begin` (foi exatamente o erro que você teve no início).

**Bloco `add_custom_command` — geração de `certs.h`:**

```cmake
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_SOURCE_DIR}/certs.h
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_cert.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Gerando main/certs.h (certificado auto-assinado para OTA local)"
    VERBATIM
)
add_custom_target(gen_certs DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/certs.h)
add_dependencies(${COMPONENT_LIB} gen_certs)
```

Isso é o que faz o `certs.h` existir na primeira compilação, sem você rodar nada manualmente:

- **`add_custom_command`**: "para produzir `certs.h`, execute `bash gen_cert.sh`". O CMake só executa se `certs.h` **não existir** — é por isso que `--force` é necessário para regenerar.
- **`add_custom_target`**: cria um alvo (nome `gen_certs`) que depende de `certs.h`.
- **`add_dependencies`**: força o componente `main` a compilar **depois** que `gen_certs` terminar. Sem isso, `ota_server.c` falha com `certs.h: No such file or directory` no primeiro build.

**Nota Windows:** o `bash` no comando é o Git Bash (`C:\Program Files\Git\bin\bash.exe`), não o WSL. Foi por isso que você precisou ajustar o `PATH` — se o `bash.exe` do WindowsApps/WSL fosse chamado, ele interpretaria os caminhos com `/c/Users/...` e o script falharia.

### `managed_components/*/CMakeLists.txt` — não mexa

Cada componente externo (`esp32-camera`, `cjson`) tem seu próprio `CMakeLists.txt`, gerenciado pelo Component Manager. Se você editar, o próximo `idf.py fullclean` sobrescreve. Se precisar customizar um componente, use `EXTRA_COMPONENT_DIRS` no `CMakeLists.txt` raiz apontando para uma cópia sua.

---

## 4. Kconfig: por que credenciais migraram de `#define` para `menuconfig`

Você teve a experiência concreta: no início, `config.h` tinha:

```c
#define CONFIG_WIFI_SSID "MinhaRedeReal"
#define CONFIG_WIFI_PASSWORD "MinhaSenha123"
```

Isso quebra o "out-of-the-box" do repositório. Dois problemas:

1. **Segurança**: commitar credenciais no GitHub é um dos erros mais comuns em projetos IoT. Bots varrem o GitHub procurando exatamente isso.
2. **Portabilidade**: quem clona o repositório precisa editar o `config.h` antes de compilar. Isso não é "out-of-the-box".

### Solução: `main/Kconfig.projbuild`

```kconfig
menu "LockMaxxing Configuration"
    config LOCKMAXXING_WIFI_SSID
        string "Wi-Fi SSID"
        default "myssid"
    config LOCKMAXXING_WIFI_PASSWORD
        string "Wi-Fi Password"
        default "mypassword"
    config LOCKMAXXING_MQTT_BROKER_URI
        string "MQTT Broker URI"
        default "mqtt://192.168.1.100:1883"
    # ...
endmenu
```

O `menuconfig` gera automaticamente `sdkconfig` com:

```ini
CONFIG_LOCKMAXXING_WIFI_SSID="MinhaRedeReal"
CONFIG_LOCKMAXXING_WIFI_PASSWORD="MinhaSenha123"
CONFIG_LOCKMAXXING_MQTT_BROKER_URI="mqtt://192.168.1.50:1883"
```

E o `sdkconfig.h` gerado contém as macros:

```c
#define CONFIG_LOCKMAXXING_WIFI_SSID "MinhaRedeReal"
#define CONFIG_LOCKMAXXING_WIFI_PASSWORD "MinhaSenha123"
```

### Como o `config.h` consome isso agora

```c
#include "sdkconfig.h"

/* Credenciais vêm do Kconfig */
#define CONFIG_WIFI_SSID            CONFIG_LOCKMAXXING_WIFI_SSID
#define CONFIG_WIFI_PASSWORD        CONFIG_LOCKMAXXING_WIFI_PASSWORD
#define CONFIG_MQTT_BROKER_URI      CONFIG_LOCKMAXXING_MQTT_BROKER_URI

/* Constantes de hardware — ficam hardcoded (não são segredo) */
#define CONFIG_GPIO_LED_GREEN       1
#define CONFIG_MQTT_CHUNK_SIZE      2048
```

O resto do código continua usando `CONFIG_WIFI_SSID` como antes — **zero mudança** em `wifi.c`, `mqtt.c`, etc. A migração foi transparente.

### O que **não** migrar para Kconfig

- **Pinos GPIO** — são hardware, não config de usuário. Ficam em `config.h`.
- **Timeouts do protocolo** (30 s de captura) — são design, não preferência.
- **Tamanho do chunk** (2048) — é determinístico do protocolo, não ajustável por usuário.

**Regra prática:** se o valor varia entre instalações do mesmo projeto (SSID, IP do broker), vai para Kconfig. Se é o mesmo para todos que rodam o projeto (pinos do hardware, protocolo), fica em `config.h`.

### E o `sdkconfig.defaults`?

Continua com **configurações do IDF**, não do projeto:

- `CONFIG_IDF_TARGET="esp32s3"`
- `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`
- `CONFIG_MQTT_BUFFER_SIZE=4096`
- `CONFIG_NEWLIB_NANO_FORMAT=y`

Esses são valores que você quer que **todo mundo que clone** tenha por padrão. As opções `LOCKMAXXING_*` ficam no Kconfig, com defaults; o usuário sobrescreve via `menuconfig` se quiser.

---

## 5. Máquina de estados em profundidade

Esta é a peça mais importante do firmware — e a mais bem projetada. Tudo o que acontece no ciclo passa por ela.

### Design: autômato puro

O `state_machine.c` é **deliberadamente burro**. Ele não:

- Não bloqueia
- Não aloca
- Não chama hardware, rede, MQTT, câmera
- Não tem task própria
- Não tem mutex

Ele só faz uma coisa: dado um estado atual e um evento, decide o próximo estado. **`(estado, evento) → novo_estado`.** É uma função pura.

Isso tem três consequências enormes:

1. **Testável no PC.** Você compilou `state_machine.c` com `gcc` e rodou 36 testes. Zero hardware, zero mock, zero ESP-IDF.
2. **Sem deadlock.** Autômato puro não tem mutex. Não trava. Nunca.
3. **Previsível.** Se você tem os logs (`TRANSITION: X --[E]--> Y`), você reconstitui exatamente o que aconteceu.

### Os 7 estados

| Estado | Significado | Como se sai |
|--------|------------|-------------|
| `INIT` | Antes da rede estar pronta | `NETWORK_READY` → IDLE |
| `IDLE` | Ocioso, aguardando timer | `TRIGGER_CAPTURE` → CAPTURING |
| `CAPTURING` | Esperando frame da câmera | `CAPTURE_OK`/`FAILED`/`TIMEOUT` |
| `SENDING` | Enviando chunks MQTT | `SEND_OK`/`FAILED` |
| `WAITING` | Esperando resultado do servidor | `RESULT_APPROVED`/`REJECTED`/`TIMEOUT`/`INVALID` |
| `APPLYING` | Atuador executando sequência | `APPLY_DONE` |
| `ERROR` | Estado transitório de falha | `RESET` → IDLE |

**Por que `APPROVED`/`REJECTED` não são estados?** Porque são **eventos**, não estados. Depois do resultado (aprovado ou rejeitado), o sistema entra em `APPLYING` — o mesmo estado para os dois. A decisão "LED verde + servo" vs "LED vermelho + buzzer" é feita **dentro** do `APPLYING`, não em estados separados. Isso reduz a máquina de 9 estados para 7.

### `sm_process_event`: o coração

```c
sm_state_t sm_process_event(sm_event_t event)
{
    sm_state_t old_state = s_current_state;
    sm_state_t new_state = old_state;

    switch (old_state) {
    case SM_STATE_IDLE:
        switch (event) {
        case SM_EVENT_TRIGGER_CAPTURE:
            new_state = SM_STATE_CAPTURING;
            break;
        case SM_EVENT_NETWORK_DOWN:
            /* Warning, mas mantém IDLE */
            break;
        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;
        default:
            ESP_LOGW(TAG, "Evento %s inválido em IDLE — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;
    /* ... outros estados ... */
    }

    if (new_state != old_state) {
        ESP_LOGI(TAG, "TRANSITION: %s --[%s]--> %s",
                 sm_get_state_name(old_state),
                 sm_get_event_name(event),
                 sm_get_state_name(new_state));
        s_current_state = new_state;
    }
    return s_current_state;
}
```

**Cinco características:**

1. **`switch` aninhado** — cada transição válida é **explícita**. Você pode ler o código e saber todas as transições sem tabela externa.
2. **`default` loga e ignora** — evento inválido não causa transição indesejada. Se chegar `TRIGGER_CAPTURE` em `APPLYING`, não aborta o ciclo; só avisa.
3. **Log de transição** — `TRANSITION: WAITING --[RESULT_APPROVED]--> APPLYING`. Você viu isso no log.
4. **Retorno é o novo estado** — o chamador não precisa reconsultar.
5. **Nada além de estado é tocado.** Sem I/O, sem delay, sem fila.

### `handle_transitions`: o loop de segurança

O `sm_process_event` só **decide** o próximo estado. Mas quem **executa as ações de entrada** de cada estado? O `handle_transitions`:

```c
static void handle_transitions(comm_task_ctx_t *ctx)
{
    int safety = 5;
    while (sm_get_state() != ctx->last_state) {
        if (safety-- <= 0) {
            ESP_LOGE(TAG, "Loop de transições não convergiu — forçando ERROR");
            sm_process_event(SM_EVENT_ABORT);
            return;
        }
        sm_state_t new_state = sm_get_state();
        ctx->last_state = new_state;
        ctx->state_start_tick = xTaskGetTickCount();
        on_enter_state(ctx, new_state);
    }
}
```

**Por que o loop:** transições podem ser encadeadas. Exemplo: `CAPTURING --[CAPTURE_OK]--> SENDING`, e o `on_enter_state(SENDING)` chama `do_send_chunked()`, que em sucesso emite `SEND_OK`, que leva a `WAITING`. Três transições em uma iteração.

**`safety = 5`:** proteção contra bug. Se `on_enter_state()` emitir eventos em loop (ex: `on_enter_state(ERROR)` chama `sm_process_event(ABORT)` em `ERROR`, que emite `ABORT` novamente), o `safety` aborta depois de 5 iterações. É a defesa contra loop infinito dentro do autômato.

**`state_start_tick` é atualizado a cada iteração:** crítico para os timeouts. Sem isso, o timeout de `WAITING` calcularia `elapsed` desde a entrada em `SENDING` (errado) e dispararia timeout no instante em que entrasse em `WAITING`.

### `compute_state_timeout` e `check_state_timeouts`: os timeouts

Cada estado tem um timeout:

| Estado | Timeout | Ação no timeout |
|--------|---------|-----------------|
| `IDLE` | 30 s | `TRIGGER_CAPTURE` (se MQTT ok) |
| `CAPTURING` | 5 s | `CAPTURE_TIMEOUT` → ERROR |
| `WAITING` | 30 s | `RESULT_TIMEOUT` → ERROR |
| `APPLYING` | 8 s | `ABORT` → ERROR |
| `ERROR` | 3 s | `RESET` → IDLE |

`compute_state_timeout` calcula quanto tempo falta até o timeout do estado atual — é passado como `timeout` no `xQueueReceive`. Se uma mensagem chega antes, a task acorda; senão, o `xQueueReceive` retorna sem mensagem e `check_state_timeouts` dispara o timeout.

**Isso é a integração elegante** entre máquina de estados e FreeRTOS: o timeout não precisa de timer separado, é o `xQueueReceive` que já tem.

### `on_enter_state`: as ações

```c
static void on_enter_state(comm_task_ctx_t *ctx, sm_state_t new_state)
{
    switch (new_state) {
    case SM_STATE_CAPTURING: {
        camera_cmd_t cmd = { .reserved = 0 };
        xQueueSend(s_camera_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        break;
    }
    case SM_STATE_SENDING:
        do_send_chunked(ctx);   // síncrono — vai para WAITING ou ERROR
        break;
    case SM_STATE_APPLYING: {
        actuator_cmd_t cmd = {
            .result = ctx->pending_apply_approved
                      ? ACTUATOR_RESULT_APPROVED
                      : ACTUATOR_RESULT_REJECTED,
        };
        xQueueSend(s_actuator_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        break;
    }
    case SM_STATE_ERROR:
        if (ctx->pending_frame != NULL) {
            camera_release(ctx->pending_frame);
            ctx->pending_frame = NULL;
        }
        break;
    default:
        break;
    }
}
```

Ações curtas — enfileirar comandos para outras tasks, ou sincronamente enviar chunks. **`comm_task` não executa a captura nem a sequência de atuadores** — só enfileira os comandos. Quem executa são `camera_task` e `actuator_task` (prioridades 4 e 2).

### `process_comm_msg`: traduzindo mensagens em eventos

```c
static void process_comm_msg(comm_task_ctx_t *ctx, const comm_msg_t *msg)
{
    switch (msg->type) {
    case COMM_MSG_NETWORK_READY:
        sm_process_event(SM_EVENT_NETWORK_READY);
        break;
    case COMM_MSG_CAMERA_FRAME:
        if (state == SM_STATE_CAPTURING && ctx->pending_frame == NULL) {
            ctx->pending_frame = msg->data.frame;
            sm_process_event(SM_EVENT_CAPTURE_OK);
        } else {
            camera_release(msg->data.frame);   // ownership!
        }
        break;
    case COMM_MSG_RESULT:
        if (state == SM_STATE_WAITING &&
            msg->data.result.image_id == ctx->pending_image_id) {
            ctx->pending_apply_approved = msg->data.result.approved;
            sm_process_event(approved
                             ? SM_EVENT_RESULT_APPROVED
                             : SM_EVENT_RESULT_REJECTED);
        } else {
            sm_process_event(SM_EVENT_RESULT_INVALID);
        }
        break;
    /* ... APPLY_DONE, TRIGGER_CAPTURE ... */
    }
}
```

Aqui está a **validação crítica**: o `image_id` do resultado precisa casar com o `pending_image_id` (a imagem que você enviou). Se não casar, é `RESULT_INVALID` → `ERROR`. Isso protege contra respostas atrasadas ou cruzadas.

---

## 6. O fluxo integrado: `comm_task ↔ SM ↔ outras tasks`

Agora que você conhece as peças, aqui está o ciclo completo **em uma iteração de sucesso**:

```
[T=0s]  comm_task (IDLE):
          compute_state_timeout() → 30 s
          xQueueReceive(..., 30s) — bloqueia

[T=30s] xQueueReceive retorna (timeout, sem msg)
          check_state_timeouts() → TRIGGER_CAPTURE
          handle_transitions():
            on_enter_state(CAPTURING):
              xQueueSend(s_camera_cmd_queue, "capture")
            [transição IDLE → CAPTURING]

[camera_task] (bloqueada em s_camera_cmd_queue):
          recebe comando
          camera_capture() — bloqueia ~100 ms
          enfileira COMM_MSG_CAMERA_FRAME em s_comm_msg_queue
          volta a bloquear

[T=30.1s] comm_task (CAPTURING):
          compute_state_timeout() → 5 s
          xQueueReceive(..., 5s)
          recebe COMM_MSG_CAMERA_FRAME
          process_comm_msg():
            pending_frame = frame
            sm_process_event(CAPTURE_OK) → SENDING
          handle_transitions():
            on_enter_state(SENDING):
              do_send_chunked():
                mqtt_publish_request()
                24 × mqtt_publish_chunk()   [~10 s]
                camera_release(frame)
                sm_process_event(SEND_OK) → WAITING
            [transição CAPTURING → SENDING → WAITING]

[T=40s]  comm_task (WAITING):
          compute_state_timeout() → 30 s
          xQueueReceive(..., 30s)

[T=40.2s] MQTT callback (na task do esp-mqtt):
          recebe {"id":1,"resultado":"aprovado"}
          on_mqtt_result() enfileira COMM_MSG_RESULT
          retorna

[T=40.3s] comm_task (WAITING):
          xQueueReceive retorna (mensagem)
          process_comm_msg():
            image_id match ✓
            pending_apply_approved = true
            sm_process_event(RESULT_APPROVED) → APPLYING
          handle_transitions():
            on_enter_state(APPLYING):
              xQueueSend(s_actuator_cmd_queue, APPROVED)
            [transição WAITING → APPLYING]

[actuator_task]:
          recebe comando
          actuators_apply_result(APPROVED):
            LED verde on
            servo → 180° (800 ms)
            hold (1500 ms)
            servo → 0° (800 ms)
            LED verde off          [total ~3 s]
          enfileira COMM_MSG_APPLY_DONE

[T=43.3s] comm_task (APPLYING):
          recebe COMM_MSG_APPLY_DONE
          process_comm_msg():
            sm_process_event(APPLY_DONE) → IDLE
          handle_transitions():
            on_enter_state(IDLE): (nada)
          [transição APPLYING → IDLE]

[T=43.3s] estado_start_tick atualizado. Próximo timeout em 30 s.
          ciclo se repete.
```

Tempo total de uma iteração: **~43 s** (30 s timer + 10 s envio + 3 s atuador). Correto — é o design.

**Concorrência real:**
- `comm_task` **nunca** espera `camera_task` ou `actuator_task` — usa filas e mensagens.
- `camera_task` e `actuator_task` **nunca** bloqueiam `comm_task` — só enfileiram.
- O MQTT callback **nunca** chama `actuators_apply_result()` direto — só enfileira para `actuator_task` executar.

---

## 7. O ponto de integração com OTA: `g_ota_busy`

Durante um OTA (push ou pull), `g_ota_busy` é `true` e a `comm_task` **para o ciclo**:

```c
while (true) {
    if (is_ota_busy()) {        // lê g_ota_busy
        // aborta ciclo em andamento, se houver
        if (state != INIT && state != IDLE && state != ERROR) {
            camera_release(pending_frame);
            sm_process_event(ABORT);
            handle_transitions();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
    }
    // ciclo normal
}
```

**O que acontece na prática:**

1. Cliente faz `POST /update` com 1 MB.
2. `ota_server.c` recebe primeiro chunk, seta `g_ota_busy = true`.
3. Na próxima iteração de `comm_task`, `is_ota_busy()` retorna `true`.
4. `comm_task` **não dispara captura**, **não envia chunks MQTT**.
5. `esp_ota_write` tem toda a rede e a flash para si.
6. Upload termina em ~10 s (era 66 s sem a pausa).
7. Reboot acontece; `g_ota_busy` não precisa ser resetado (a placa reinicia).

**Por que flag volatile e não mutex:** como expliquei na Parte 2, `ota_get_state()` (que pega mutex) travou o `app_main` por interação com prioridade. `volatile bool` elimina o deadlock — leitura atômica, sem bloqueio.

**Quando `g_ota_busy` é setado:**

| Local | Quando |
|-------|--------|
| `ota_server.c` — `update_post_handler` | Após adquirir `g_upload_mutex` (push) |
| `ota_client.c` — `ota_https_update_start` | Antes do `xTaskCreate` (pull) |

**Quando é resetado:**

| Local | Quando |
|-------|--------|
| `ota_server.c` — cleanup | Se o upload falhar (`err != ESP_OK`) |
| `ota_client.c` — final de `ota_client_task` | Sempre ao terminar |

**Em caso de sucesso no push**, `g_ota_busy` **fica `true`** e o reboot acontece — o que é correto, porque a placa vai reiniciar de qualquer forma.

---

## 8. O que levar desta Parte 3

1. **28 arquivos, cada um com uma responsabilidade.** Nenhum arquivo é tão grande que exija rolagem constante, nenhum é tão pequeno que disperse lógica.

2. **CMake raiz é o porteiro.** `main/CMakeLists.txt` é o mapa. `add_custom_command` gera `certs.h`. `REQUIRES` errado gera `undefined reference`.

3. **Kconfig resolveu o "out-of-the-box".** Credenciais no `menuconfig`, constantes em `config.h`. Quem clona compila sem editar nada. Quem usa configura sem vazar segredo.

4. **Máquina de estados é função pura.** Sem task, sem mutex, sem I/O. Por isso é testável no PC com 36 testes em 0,1 s.

5. **`handle_transitions` fecha transições encadeadas.** `on_enter_state` pode emitir eventos; o loop com `safety=5` as consome em sequência com `state_start_tick` correto.

6. **A integração SM ↔ FreeRTOS é elegante:** o timeout de cada estado é o timeout do `xQueueReceive`. Sem timer separado, sem polling.

7. **OTA pausa o ciclo via `g_ota_busy`.** Flag `volatile bool`, sem mutex. Reduziu o tempo de upload de 66 s para 10 s.
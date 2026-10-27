# Explicação Consolidada — Parte 6/6: Como Entender e Modificar o Projeto

Esta é a parte final. Ela responde: *como funcionam as funções principais*, *quais conceitos de C aparecem*, *quais boas práticas foram aplicadas* e *como você pode estender o projeto sem quebrá-lo*. Tudo referenciado ao seu código real.

---

## 1. Funções principais — o mapa completo

Formato padrão para cada uma: **o que faz · parâmetros · retorno · quem chama · o que acontece internamente · erros possíveis**.

### `app_main()` — `main.c`

| Item | Descrição |
|------|-----------|
| **O que faz** | Ponto de entrada do firmware. Inicializa todos os módulos em ordem e retorna (as tasks assumem). |
| **Parâmetros** | Nenhum (assinatura imposta pelo IDF). |
| **Retorno** | Nenhum (`void`). O IDF considera a task principal concluída. |
| **Quem chama** | O FreeRTOS, após o bootloader terminar. |
| **O que acontece** | 11 etapas: NVS → OTA init → boot info → atuadores + self-test → câmera + captura de teste → Wi-Fi init → MQTT init → decisão de rollback → `tasks_start_all()` → servidor HTTPS local → return. |
| **Erros** | Falha crítica (câmera/atuadores) → `force_reboot()` ou `ota_mark_app_invalid_and_reboot()`. |

**Ponto crítico:** depois que `tasks_start_all()` retorna, o firmware entra em "regime". Se algo falhar depois disso, as tasks precisam lidar — `app_main` não pode mais intervir.

### `camera_init()` — `camera.c`

| Item | Descrição |
|------|-----------|
| **O que faz** | Inicializa o driver `esp32-camera`, detecta o sensor e aplica config específica (OV5640 → flip vertical). |
| **Parâmetros** | Nenhum (config vem de `config.h`). |
| **Retorno** | `ESP_OK`, `ESP_ERR_NO_MEM` (PSRAM não init), `ESP_ERR_NOT_FOUND` (sensor não respondeu), `ESP_FAIL` (outros). |
| **Quem chama** | `app_main()` (uma vez). |
| **O que acontece** | (1) Verifica `esp_psram_is_initialized()`, (2) monta `camera_config_t` de `config.h`, (3) `esp_camera_init()`, (4) `esp_camera_sensor_get()`, (5) lê PID, aplica `set_vflip(1)` se OV5640. |
| **Erros** | PSRAM ausente, cabo flat solto, pinagem errada, sensor queimado. Logs específicos guiam o diagnóstico. |

**Idempotência:** chamar duas vezes retorna `ESP_OK` na segunda e loga warning. `g_initialized` protege.

### `camera_capture()` / `camera_release()` — `camera.c`

`camera_capture()` bloqueia até um frame estar disponível. Com `CAMERA_GRAB_LATEST`, é ~0 ms. Retorna um `camera_frame_t*` que aponta para PSRAM **gerenciada pelo driver**.

`camera_release()` devolve o framebuffer ao driver. **Obrigatória** — sem ela, após 2 capturas o driver fica sem framebuffer e `fb_get()` trava **para sempre**.

**Política de ownership:** o frame passa para `comm_task` via fila. `comm_task` é o único responsável pela liberação. Se a fila encher, `camera_task` libera antes de descartar.

### `wifi_init()` / `wifi_wait_connected()` — `wifi.c`

`wifi_init()` configura `esp_netif`, `esp_event`, driver Wi-Fi, registra handlers, **desativa power save**, e chama `esp_wifi_start()`. Não bloqueia.

`wifi_wait_connected(30000)` bloqueia até 30 s no `WIFI_GOT_IP_BIT`. Usado em `app_main` para decidir se o MQTT vai tentar conectar.

**Backoff exponencial no handler de desconexão:** 1 s, 2 s, 4 s, 8 s, 16 s, 30 s (teto). Reset em `GOT_IP`.

### `mqtt_init()` / `mqtt_publish_chunk()` — `mqtt.c`

`mqtt_init()` configura o cliente (URI, client_id, keepalive, reconexão automática), registra o event handler. Assina `/result` no `MQTT_EVENT_CONNECTED`.

`mqtt_publish_chunk()` monta o header binário, copia para `s_chunk_buf` (estático, protegido por mutex), chama `esp_mqtt_client_publish()`. Retorna `msg_id` ou `ESP_FAIL`.

**Ponto crítico:** `esp_mqtt_client_publish()` **copia** o payload antes de retornar. O `s_chunk_buf` pode ser reusado imediatamente.

### `sm_process_event()` — `state_machine.c`

| Item | Descrição |
|------|-----------|
| **O que faz** | Recebe um evento, decide o próximo estado, loga transição. |
| **Parâmetros** | `sm_event_t event` (14 valores possíveis). |
| **Retorno** | `sm_state_t` (novo estado). |
| **Quem chama** | **Apenas `comm_task`**, através de `process_comm_msg()`, `check_state_timeouts()`, `on_enter_state()`. |
| **O que acontece** | `switch (estado_atual) → switch (evento) → novo_estado`. Se `novo != atual`, loga `TRANSITION: X --[E]--> Y` e atualiza `s_current_state`. |
| **Erros** | Nunca retorna erro. Evento inválido → warning + estado inalterado. |

**Função pura:** sem I/O, sem alocação, sem mutex. Testável no PC (36 testes).

### `comm_task()` — `tasks.c`

O orquestrador. Loop:

```
if (is_ota_busy()) { dorme 500ms; continue; }   // pausa durante OTA
compute_state_timeout()                          // quanto falta pro timeout do estado
xQueueReceive(..., wait)                         // bloqueia até msg ou timeout
if (msg) process_comm_msg()                      // traduz em evento
check_state_timeouts()                           // timeouts → eventos
handle_transitions()                             // executa on_enter_state de cada transição
```

**Quem chama `comm_task`:** o FreeRTOS, uma vez criada. Nunca retorna (while true).

### `do_send_chunked()` — `tasks.c`

Chamada dentro de `on_enter_state(SENDING)`. Executa o envio completo de uma imagem:

1. Gera `image_id` (`s_next_image_id++`)
2. Calcula `total_chunks = ceil(len / 2048)`
3. `mqtt_publish_request()` com JSON de metadados
4. Loop de `mqtt_publish_chunk()` por chunk
5. **Sempre** libera o frame (`camera_release`) antes de retornar
6. Emite `SEND_OK` ou `SEND_FAILED`

**Síncrona** — quando retorna, a máquina de estados já está em `WAITING` ou `ERROR`.

### `update_post_handler()` — `ota_server.c`

Recebe `POST /update` com o `.bin` no corpo.

1. Verifica warmup (uptime < 300 s → `503`)
2. Adquire `g_upload_mutex` (timeout 0 → `409` se já há upload)
3. Seta `g_ota_busy = true`
4. Loop de `httpd_req_recv()` + `esp_ota_write()` + `mbedtls_sha256_update()` em streaming
5. Compara SHA-256 com o header `X-SHA256`
6. `esp_ota_end()` valida estrutura
7. `esp_ota_set_boot_partition()`
8. Responde `200 OK` com JSON
9. `vTaskDelay(2000)` + `esp_restart()` (com mutex mantido até o reboot)

**Ponto crítico:** o mutex **não é liberado antes do reboot** — evita que um segundo POST comece enquanto o primeiro está prestes a reiniciar.

### `ota_client_perform_download()` — `ota_client.c`

Uma tentativa completa de pull OTA:

1. Configura `esp_http_client_config_t` (com `cert_pem` + `common_name`)
2. Abre conexão HTTPS
3. Lê Content-Length, valida status 200
4. `esp_ota_begin()`
5. Loop de `esp_http_client_read()` + `esp_ota_write()` + `mbedtls_sha256_update()`
6. Callback de progresso com throttle (500 ms)
7. `mbedtls_sha256_finish()` + compara com SHA-256 esperado
8. `esp_ota_end()` + `esp_ota_set_boot_partition()`

**Retry é feito pela task** (`ota_client_task`): 2 tentativas com 2 s entre elas. **Exceção:** `ESP_ERR_INVALID_CRC` **não retenta** — se o arquivo está corrompido no servidor, baixar de novo não resolve.

---

## 2. Conceitos de C no seu código

### `struct` e `typedef`

```c
typedef struct {
    TickType_t       state_start_tick;
    sm_state_t       last_state;
    camera_frame_t  *pending_frame;
    uint32_t         pending_image_id;
    bool             pending_apply_approved;
} comm_task_ctx_t;
```

**`typedef struct { ... } Nome;`** cria um tipo `Nome` (sem precisar de `struct Nome` toda vez). É usado para todos os tipos de contexto/estado do projeto.

**`struct` anônima** (sem nome após `struct`) é comum quando o tipo só vai ser usado pelo `typedef`. Se você precisar de forward declaration (ponteiro opaco), dá nome:

```c
typedef struct camera_fb_t camera_frame_t;   // forward — incompleto
```

### Ponteiros

Aparecem em três padrões principais:

**1. Parâmetro de saída (`**`):**

```c
esp_err_t camera_capture(camera_frame_t **out_frame)
```

Por que `**` e não `*`: a função precisa **escrever** o ponteiro na variável do chamador. Se fosse `camera_frame_t *out_frame`, a atribuição ficaria local.

**Uso:**

```c
camera_frame_t *frame = NULL;
camera_capture(&frame);   // frame agora aponta para o frame real
```

**2. Callback (ponteiro para função):**

```c
typedef void (*ota_progress_cb_t)(const ota_progress_t *progress);
typedef void (*mqtt_result_cb_t)(uint32_t image_id, mqtt_result_t result);
```

**3. Ponteiro opaco (`void *`):**

```c
void *camera_get_sensor(void);   // retorna sensor_t* sem expor o tipo
```

### `enum`

```c
typedef enum {
    SM_STATE_INIT     = 0,
    SM_STATE_IDLE     = 1,
    ...
} sm_state_t;
```

**Vantagens:** nomes legíveis, checagem do compilador no `switch`, tamanho pequeno (geralmente `int`, mas o `-fshort-enums` do GCC pode reduzir para `uint8_t`).

**Uso no log:**

```c
static const char *k_state_names[] = {
    [SM_STATE_INIT] = "INIT",
    [SM_STATE_IDLE] = "IDLE",
    ...
};
sm_get_state_name(SM_STATE_IDLE);   // "IDLE"
```

Designated initializers garantem que o índice coincide com o valor do enum.

### `const`

Três usos:

```c
static const char *TAG = "CAMERA";                 // string literal, não pode ser modificada
esp_err_t wifi_get_ip(char *buf, size_t buf_len);  // char * (buf será escrito)
bool ota_sha256_hex_equals(const char *a, const char *b);  // const char * (só leitura)
```

**Regra prática:** parâmetros que a função **lê** mas não **escreve** vão como `const`. Isso documenta intenção e permite otimização.

### `static`

Três significados em C:

**1. Função estática (escopo do arquivo):**

```c
static void wifi_event_handler(...)   // só visível neste .c
```

**2. Variável global estática (escopo do arquivo):**

```c
static bool g_initialized = false;    // só visível neste .c
```

**3. Variável local estática (persiste entre chamadas):**

```c
void f(void) {
    static int counter = 0;   // inicializa uma vez, persiste
    counter++;
}
```

**No seu projeto, predominam as duas primeiras** — encapsular a implementação e evitar vazamento de símbolos.

### `volatile`

```c
extern volatile bool g_ota_busy;
```

**Significado:** "esta variável pode mudar fora desta função, não otimize".

**Sem `volatile`:** o compilador pode otimizar um loop:

```c
while (g_ota_busy) { }   // compilador pode transformar em while(1), lendo só uma vez
```

**Com `volatile`:** cada iteração relê da memória.

**Uso correto:** flags de estado lidas/escritas por tasks diferentes (`g_ota_busy`). **Uso incorreto:** proteção de estruturas maiores (não é atômico além de 1 byte).

### Arrays e buffers

```c
static uint8_t s_chunk_buf[CONFIG_MQTT_CHUNK_HEADER_SIZE + CONFIG_MQTT_CHUNK_SIZE];
```

**Tamanho em compile-time:** o array vai para `.bss` (variável global estática) ou stack (variável local). No seu caso, `.bss` — reserva ~2 KB permanentes.

**Arrays grandes na stack:** cuidado. 4 KB em uma task com stack de 3 KB = **stack overflow**. Por isso os buffers são globais estáticos, não locais.

### Passagem por valor vs referência

C não tem referência — tem **passagem por valor** e **passagem por ponteiro**.

```c
void f(sm_event_t e);              // por valor (cópia de 4 bytes)
void g(camera_frame_t *fb);        // por ponteiro (cópia do ponteiro, objeto compartilhado)
void h(camera_frame_t **fb);       // por ponteiro para ponteiro (escreve o ponteiro)
```

**No seu código:**

- `sm_process_event(SM_EVENT_X)` — valor (enum é pequeno, cópia barata)
- `xQueueSend(queue, &msg, ...)` — ponteiro (cópia real do conteúdo para a fila)
- `camera_capture(&frame)` — ponteiro para ponteiro (a função escreve)

### Escopo

```c
void f(void) {
    int x = 10;          // escopo: dentro de f()
    {
        int y = 20;      // escopo: dentro do bloco { }
        x = y;           // x visível, y visível
    }
    // y NÃO visível aqui
}
```

**No seu código, escopo de blocos é usado em `switch`:**

```c
case SM_STATE_CAPTURING: {
    camera_cmd_t cmd = { .reserved = 0 };   // ← bloco próprio
    xQueueSend(s_camera_cmd_queue, &cmd, ...);
    break;
}
```

**Por que o bloco:** sem ele, `camera_cmd_t cmd` seria declarado no escopo do `switch` inteiro, e outros `case` não poderiam reusar o nome `cmd` (mesmo declarando outro tipo). O bloco `{ }` cria escopo próprio — cada `case` pode ter variáveis com o mesmo nome.

### Gerenciamento de memória

Três padrões no seu firmware:

**1. Estática (padrão):**

```c
static uint8_t s_chunk_buf[2056];   // .bss, reserva permanente
```

**2. Dinâmica transacional (cJSON, IDF):**

```c
cJSON *root = cJSON_Parse(...);
// usa
cJSON_Delete(root);   // libera tudo
```

**3. Nunca no código de aplicação:** você não usa `malloc`/`free` diretamente. Se um dia precisar, `heap_caps_malloc` com capability explícita.

---

## 3. Boas práticas aplicadas

| Prática | Como aparece no código | Por que importa |
|---------|----------------------|-----------------|
| **Modularização** | 28 arquivos com responsabilidade única | Testável, substituível, navegável |
| **Separação de responsabilidades** | Máquina de estados pura; `comm_task` só orquestra; atuadores só em `actuators.c` | Debug isolado |
| **Nomes claros** | `camera_capture`, `mqtt_publish_chunk`, `sm_process_event` | Código autoexplicativo |
| **Tratamento de erro** | Todo `esp_err_t` verificado; propagação ou decisão | Sem falhas silenciosas |
| **Logs estruturados** | TAG por módulo, parâmetros nos logs | Reconstruir execução do log |
| **Timeouts** | Todos os `xQueueReceive`, `esp_timer`, `xSemaphoreTake` | Nenhum bloqueio infinito |
| **Reconexão** | Wi-Fi backoff, MQTT auto-reconnect | Resiliente a queda de rede |
| **Sincronização** | 3 queues, 3 mutex, 1 spinlock, 1 volatile bool | Escolha correta por contexto |
| **Controle de memória** | Buffers estáticos; cJSON transacional | Sem fragmentação |
| **Prevenção de leak** | Ownership de frames explícito | PSRAM não esgota |
| **Prevenção de deadlock** | Flag volatile para `g_ota_busy` (não mutex) | `app_main` não trava |
| **Prevenção de race** | Mutex em buffers compartilhados; spinlock em contadores | Estado consistente |
| **Prioridades adequadas** | 5→4→3→2 (rede→câmera→orquestração→atuador) | Ordem correta de urgência |
| **Stack adequada** | 6 KB para `camera_task`, 3 KB para `actuator_task` | Sem overflow |
| **Configuração centralizada** | `config.h` (constantes) + Kconfig (credenciais) | Out-of-the-box |

---

## 4. Como modificar o projeto — 5 receitas concretas

### Receita 1 — Trocar para OV3660 (reverter)

Se você trocar o módulo da câmera:

1. **`config.h`** — nenhuma mudança (mesma pinagem).
2. **`camera.c`** — em `apply_sensor_specific_config`:

```c
static void apply_sensor_specific_config(sensor_t *s)
{
    if (g_sensor_pid == 0x5640) {
        s->set_vflip(s, 1);   // só para OV5640
    }
    // OV3660: nada a fazer — já está na orientação correta
}
```

3. **Rebuild + flash + verificar no log:** `Sensor detectado: OV3660`.

### Receita 2 — Adicionar sensor I2C (BME280 de temperatura)

Novo módulo `main/bme280.c`:

```c
#include "driver/i2c_master.h"
#include "esp_log.h"

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t bme280_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 8,      // pino livre
        .scl_io_num = 9,      // pino livre
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_new_master_bus(&bus_cfg, &s_bus);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x76,   // BME280 default
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    return ESP_OK;
}
```

**Integração:** chame `bme280_init()` em `app_main` (após `camera_init`). Se quiser publicar temperatura junto com a imagem, adicione no `mqtt_publish_request`:

```c
// Em do_send_chunked:
float temp = bme280_read_temperature();
char payload[256];
snprintf(payload, sizeof(payload),
         "{\"id\":%u,\"size\":%u,\"chunks\":%u,\"temp_c\":%.1f}",
         image_id, frame->len, total_chunks, temp);
```

**Atenção:** `%f` exige formatação float. Se você tem `CONFIG_NEWLIB_NANO_FORMAT=y`, `%f` não funciona. Use `%d` com multiplicação: `(int)(temp * 10)` → `25.3` vira `253`, e o Python divide por 10.

### Receita 3 — Botão físico que dispara captura

Hardware: botão entre GPIO 14 e GND (pull-up interno).

**Novo módulo `main/button.c`:**

```c
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tasks.h"

#define BUTTON_GPIO 14

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,   // polling simples
    };
    gpio_config(&io);

    int last = 1;
    while (1) {
        int now = gpio_get_level(BUTTON_GPIO);
        if (last == 1 && now == 0) {   // borda de descida (pressionou)
            tasks_trigger_capture();
        }
        last = now;
        vTaskDelay(pdMS_TO_TICKS(50));   // debounce 50 ms
    }
}

esp_err_t button_init(void) {
    return xTaskCreate(button_task, "button", 2048, NULL, 1, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
```

**Integração:**

1. Chame `button_init()` em `app_main` (após `tasks_start_all()` — a função `tasks_trigger_capture` precisa da fila criada).
2. `tasks_trigger_capture()` já existe e enfileira `COMM_MSG_TRIGGER_CAPTURE`.
3. Adicione `button.c` no `SRCS` do `main/CMakeLists.txt`.

**O que acontece:** apertar o botão gera `SM_EVENT_TRIGGER_CAPTURE` na `comm_task`. Se estiver em `IDLE`, dispara ciclo. Se estiver em outro estado, evento é ignorado (comportamento correto — não interrompe ciclo em andamento).

### Receita 4 — Aumentar resolução para UXGA (1600×1200)

`config.h`:

```c
#define CONFIG_CAM_FRAMESIZE    FRAMESIZE_UXGA   // era SVGA
#define CONFIG_IMAGE_MAX_SIZE   (200 * 1024)     // 200 KB continua suficiente
```

**Consequências:**

| Métrica | SVGA | UXGA |
|---------|------|------|
| Tamanho típico JPEG | ~40 KB | ~100 KB |
| Chunks (2048 B) | ~20 | ~50 |
| Tempo de envio (MQTT) | ~2 s | ~5 s |
| Framebuffers (2×) em PSRAM | ~192 KB | ~920 KB |
| Latência de captura | ~0 ms | ~100 ms (mais dados) |

**Se o tempo de envio importar:** aumente `CONFIG_MQTT_CHUNK_SIZE` para 4096. Ajuste `CONFIG_MQTT_BUFFER_SIZE` no `sdkconfig.defaults` para 8192 (senão o publish falha com truncamento).

**Se PSRAM ficar apertada:** reduza `fb_count` para 1 (perde o double buffering, mas economiza ~460 KB).

### Receita 5 — OTA via botão físico

Reutilize o botão da receita 3, mas com **long-press** para OTA:

```c
// Em button_task:
int press_start_ms = 0;
while (1) {
    int now = gpio_get_level(BUTTON_GPIO);
    if (last == 1 && now == 0) {
        press_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    if (last == 0 && now == 1) {   // soltou
        int held = xTaskGetTickCount() * portTICK_PERIOD_MS - press_start_ms;
        if (held < 500) {
            tasks_trigger_capture();   // clique curto: captura
        } else if (held > 3000) {
            // clique longo: dispara OTA pull de URL fixa
            ota_https_update_start("https://192.168.15.5:8443/LockMaxxing.bin",
                                   NULL, NULL, ota_reboot_cb);
        }
    }
    last = now;
    vTaskDelay(pdMS_TO_TICKS(50));
}
```

**Callback:**

```c
static void ota_reboot_cb(esp_err_t result, const char *msg) {
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}
```

**Integração:** chame `button_init()` em `app_main` **após** `ota_init` (a função `ota_https_update_start` já está disponível).

---

## 5. Melhorias futuras

Divididas por **esforço × impacto**.

### Curto prazo (horas de trabalho, impacto imediato)

| Melhoria | Ganho | Como |
|----------|-------|------|
| **Reduzir `CONFIG_MQTT_RESULT_TIMEOUT_MS`** de 30 s para 15 s | Feedback mais rápido em falha de servidor | Uma linha em `config.h` |
| **`/status` expor métricas** | Diagnóstico remoto sem UART | Adicionar `wifi_get_stats()`, `mqtt_get_stats()` no JSON |
| **`/capture` endpoint** (HTTP POST) | Disparar captura sem botão físico | Nova rota em `ota_server.c` chamando `tasks_trigger_capture()` |
| **Reset via HTTP** (`/reboot`) | Recuperação remota | Nova rota chamando `esp_restart()` |
| **Senha no `/update` e `/pull-ota`** | Impede firmware malicioso em rede compartilhada | Header `X-OTA-Token` comparado com `CONFIG_*` |

### Médio prazo (dias, ganho estrutural)

| Melhoria | Ganho | Desafio |
|----------|-------|---------|
| **Credenciais em NVS (provisionamento sem recompilar)** | Instalar em campo sem IDE | Implementar portal captivo ou BLE GATT server |
| **Chunk adaptativo** | 4 KB em Wi-Fi bom, 1 KB em ruim | Medir throughput via RTT ou ACKs e ajustar |
| **Qualidade JPEG adaptativa** | Imagem menor em rede lenta | Reduzir de 12 para 20 se throughput <X KB/s |
| **Deep-sleep entre capturas** | Bateria: dias em vez de horas | Requer wakeup por timer + reconexão (perde ~5 s por ciclo) |
| **Task WDT explícito** | Detecta travamento de task específica mais rápido | `esp_task_wdt_add(comm_task_handle)` |
| **Secure Boot V2 + Flash Encryption** | Firmware autenticado por hardware | Exige eFuse (**irreversível** — decisão séria) |
| **MQTT com TLS** | Proteção contra broker malicioso | Requer CA raiz e mTLS opcional |
| **Compressão de imagem dupla** (qualidade dinâmica) | Mais detalhes em boa rede, menos em ruim | Lógica no `camera.c` |

### Longo prazo (semanas, para produto)

| Melhoria | Ganho | Custo |
|----------|-------|-------|
| **Redesenho de hardware** | Regulador adequado (AMS1117 → MP1584), RTC externo, watchdog externo | Nova PCB |
| **Certificação (ANATEL/FCC)** | Produto comercializável | Ensaio em câmara anecoica |
| **OTA via BLE** | Provisionamento inicial sem Wi-Fi | Stack BLE + DFU |
| **Análise embarcada (TinyML)** | Decisão local sem nuvem | `esp-tflite-micro` + modelo treinado |
| **Fleet management** | Monitorar N dispositivos | Backend MQTT + dashboard |

---

## 6. Sobre aprender com este projeto

Este projeto é **representativo** de 90% do que se faz em IoT profissional:

- **Wi-Fi + MQTT + TLS** — é o stack padrão de dispositivos conectados.
- **OTA com rollback** — obrigatório em produto.
- **FreeRTOS com tasks e filas** — arquitetura canônica de ESP32.
- **Câmera com PSRAM** — específico deste projeto, mas o padrão de "buffer grande em PSRAM" é geral.
- **Host tests** — prática de engenharia que separa protótipo de produto.

**O que você aprendeu vai se transferir** para qualquer projeto ESP32:

- Todo projeto com Wi-Fi vai precisar de `WIFI_PS_NONE` (se for plugado).
- Todo projeto com TLS vai precisar de `cert_pem`/`crt_bundle_attach` (não `NULL`).
- Todo projeto com OTA vai precisar de partição dupla + rollback.
- Todo projeto com tasks vai precisar de filas/mutexes com contexto certo.
- Todo projeto com cJSON vai precisar de `ParseWithLength` (não `Parse`).

**O que é específico deste projeto** (não se transfere direto):

- Pinagem da câmera N16R8.
- `set_vflip(1)` para OV5640.
- Conflito `LEDC_TIMER_0` da câmera com servo.
- Tamanho de chunk 2048 (depende do seu payload).

**O que você faria diferente no próximo projeto:**

1. **Host tests desde o início** — pegou 2 bugs em 30 min. Se você tivesse começado sem, seriam 2 dias de debug.
2. **`WIFI_PS_NONE` desde a primeira linha** — não deixaria para descobrir na otimização.
3. **Kconfig desde o começo** — não teria o problema do "out-of-the-box" no final.
4. **Conexão física em protoboard** antes do código — servo, LEDs, buzzer montados teriam pegado o conflito LEDC na hora.
5. **Medir cada otimização isoladamente** — o push foi de 66 s → 9.85 s por 3 razões. Sem medir cada uma, você não saberia qual foi a principal.

---

## 7. Considerações finais

O firmware está **funcional e validado**:

- 78 host tests passando
- Push OTA em **9.85 s** (era 66 s)
- Pull OTA funcionando sem skip TLS
- Rollback testado
- Warmup de 300 s
- Ciclo completo: captura → envio → resultado → atuador → IDLE
- Logs estruturados por módulo
- Watchdog em três camadas
- Sem `malloc` no código de aplicação

**O que ainda pode ser feito** (mas não é obrigatório):

- Hardware real: LEDs, buzzer, servo (o firmware tenta acionar, mas você não tem conectado).
- Análise de imagem real (o Python simula).
- TLS mútuo (broker autenticando dispositivo).
- Provisionamento sem recompilar (credenciais em NVS).

**O que este projeto provou:**

1. **RTOS não é gargalo.** O gargalo era Wi-Fi power save — uma linha resolveu.
2. **Bare-metal não é solução.** Você herdaria todos os problemas de rede sem a abstração do RTOS.
3. **Host tests valem o investimento.** 150 linhas de teste economizaram horas de debug no hardware.
4. **Otimização é iterativa e medida.** Cada mudança isolada, medida antes da próxima.
5. **A engenharia está no processo, não no resultado.** O resultado é 28 arquivos de código. O processo é como você chegou neles — e o processo se repete em qualquer projeto futuro.
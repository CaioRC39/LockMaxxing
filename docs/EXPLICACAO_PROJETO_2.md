# Explicação Consolidada — Parte 2/6: FreeRTOS em profundidade

Esta parte responde a três perguntas: *o que é um RTOS*, *o que são as primitivas que você usou* e *por que cada uma foi escolhida*. Tudo referenciado ao seu código real — `tasks.c`, `state_machine.c`, `wifi.c`, `ota_*.c`.

---

## 1. O que é um RTOS (e por que você **não** usaria um superloop)

**RTOS** = *Real-Time Operating System*. "Real-time" aqui não significa "rápido" — significa **determinístico**: uma tarefa de alta prioridade pronta para rodar executa **em tempo limitado e previsível**, independente do que as outras estão fazendo.

### O superloop clássico

```c
while (1) {
    wifi_poll();        // 100 µs
    mqtt_poll();        // 200 µs
    if (time_to_capture()) capture();  // 200 ms ← problema
    handle_mqtt_messages();            // 50 µs
    control_actuators();               // 100 µs
}
```

Enquanto `capture()` roda por 200 ms, o Wi-Fi **não é servido**. Se um pacote chega, o stack TCP/IP acumula. Se um beacon falha, a associação cai. Se o broker aguarda PINGRESP por 60 s, mas o loop demora 200 ms por iteração, tudo funciona — mas **em condição ideal**. Quando o JPEG fica maior (imagem mais detalhada), o loop vai para 400 ms, e a rede começa a sofrer *starvation*. Você não descobre o motivo — só vê "desconectou do broker".

### O RTOS resolve isso com preempção

Em FreeRTOS, cada função vira uma **task** independente, com sua própria stack. O **scheduler** alterna entre elas a cada tick (1 ms no seu projeto) ou quando uma bloqueia (espera fila, semáforo, delay). Se `camera_task` está capturando por 200 ms **sem chamar `vTaskDelay`**, o scheduler **a interrompe** e dá CPU para `wifi_mqtt_task` — porque a prioridade dele é maior.

No seu projeto, o ganho é concreto:

- Captura JPEG leva ~100 ms bloqueando `camera_task`.
- Wi-Fi **continua** funcionando porque `wifi_mqtt_task` tem prioridade 5 e é **preemptiva**.
- Se fosse superloop, o Wi-Fi ficaria suspenso 100 ms por captura — exatamente o que causava `NO-SOI` e timeouts nos seus testes iniciais.

### Onde o RTOS **não** ajuda

Sua pergunta anterior ("por que a rede é tão lenta se escolhi RTOS?") veio desse mal-entendido. **RTOS não acelera rede.** Ele garante que **a task certa rode na hora certa**, mas se o Wi-Fi físico está a 250 ms por round-trip, o RTOS não muda isso. RTOS é **determinismo**, não **velocidade**. Essas são coisas independentes.

---

## 2. Task: o conceito, e como aparece no seu código

Uma **task** é uma função C com assinatura `void fn(void *arg)` que o scheduler executa como se fosse um programa independente. Ela tem:

- **Stack própria** (alocada na criação)
- **Prioridade** (0–25 no FreeRTOS; seu projeto usa 2–5)
- **Estado** (Running, Ready, Blocked, Suspended — veja adiante)
- **Contexto de CPU** salvo/restaurado pelo scheduler

### Criação no seu `tasks.c`

```c
h = xTaskCreateStaticPinnedToCore(
    camera_task, "camera",
    CONFIG_TASK_CAMERA_STACK / sizeof(StackType_t),
    NULL, CONFIG_TASK_CAMERA_PRIO,
    s_camera_stack, &s_camera_task_static,
    CONFIG_TASK_CAMERA_CORE);
```

Cinco campos importantes:

| Campo | Valor no seu projeto | Significado |
|-------|---------------------|-------------|
| `camera_task` | função | O corpo da task |
| `"camera"` | string | Nome (aparece no `idf.py monitor`, útil para debug) |
| `CONFIG_TASK_CAMERA_STACK / sizeof(StackType_t)` | 6144 bytes / 4 = 1536 palavras | Tamanho da stack em **palavras**, não em bytes |
| `CONFIG_TASK_CAMERA_PRIO` | 4 | Prioridade (maior = mais urgente) |
| `CONFIG_TASK_CAMERA_CORE` | 1 | Core fixo (dual-core no S3) |

### `xTaskCreateStatic` vs `xTaskCreate`

Você usou **`xTaskCreateStatic`** — a variante que **não aloca dinamicamente**. Você passa o buffer da stack (`s_camera_stack[]`) e a estrutura da task (`s_camera_task_static`), ambos **estáticos** (em `.bss`).

Por que: consistente com a política "sem malloc em código de aplicação". As tasks não fragmentam heap porque nunca tocam no heap.

### Estados de uma task

O FreeRTOS alterna entre quatro estados:

```
           ┌─────────────────────────────────────┐
           │                                     │
           ▼                                     │
    ┌────────────┐   evento    ┌────────────┐    │
    │  Blocked   │◄──espera────│   Ready    │    │
    └──────┬─────┘             └─────┬──────┘    │
           │                          │           │
           │ evento                   │ escalada  │
           │ pronto                   │ pelo      │
           │                          │ scheduler │
           ▼                          ▼           │
        ┌──────────────────────────────────────┐  │
        │             Running                  │──┘
        └──────────────────────────────────────┘  (preempção)
              │                                  
              │ vTaskSuspend()                   
              ▼                                  
        ┌────────────┐                           
        │ Suspended  │  (não é usado no projeto) 
        └────────────┘                           
```

**No seu projeto:**

- **Running**: a task está executando agora (apenas uma por core).
- **Ready**: pronta, aguardando o scheduler escalar.
- **Blocked**: esperando `xQueueReceive`, `vTaskDelay`, semáforo — o caso mais comum no firmware. **Uma task bloqueada não gasta CPU.**
- **Suspended**: parada manualmente. **Nunca usada no seu código** — o `Suspended` seria necessário em casos como "parar a captura de câmera temporariamente", que você resolveu via máquina de estados.

**No seu log:** quando você vê `I (35474) TASKS: Timer de captura expirou — disparando ciclo`, essa linha vem de `check_state_timeouts()`, chamada por `comm_task` que estava **Blocked** em `xQueueReceive(s_comm_msg_queue, &msg, wait)` por até 30 s. O timeout do `xQueueReceive` a acordou.

### Prioridade

Prioridade **maior** = mais urgente. No FreeRTOS:

| Prioridade | Nível | Uso típico |
|-----------|-------|------------|
| 0 | idle | Task idle (o que roda quando ninguém faz nada) |
| 1–2 | baixa | Atuadores, lógica não-crítica |
| 3–4 | média | Orquestração, câmera |
| 5–10 | alta | Rede, drivers de rádio |
| 20+ | muito alta | Wi-Fi do IDF (`prio:23` no seu log!) |

**No seu projeto:**

```
wifi_mqtt_task  prio 5  →  rede, saúde do sistema
camera_task     prio 4  →  câmera, mas não bloqueia rede
comm_task       prio 3  →  orquestração (nunca urgente por si)
actuator_task   prio 2  →  LEDs/servo/buzzer (aceita latência de ~ms)
```

**Regra do FreeRTOS:** tasks com mesma prioridade se alternam por round-robin (com time slice). Tasks com prioridade diferente, a mais alta **sempre** vence. Se `wifi_mqtt_task` está `Ready`, ela **preempta** `camera_task` imediatamente — o que é exatamente o que você quer.

**Por que `actuator_task` tem a menor prioridade:** a sequência APROVADO/REPROVADO leva ~3 s (LED, servo sweep, buzzer). Nesse tempo, **o mundo físico não é urgente** — mas a rede **é**. Se o servo gira 100 ms mais devagar, ninguém nota. Se o Wi-Fi cai 100 ms, o broker desconecta.

### Stack

Cada task tem sua própria stack **separada do heap principal**. O tamanho é fixado na criação e não pode crescer. Escolhas do seu projeto:

| Task | Stack | Por quê |
|------|-------|---------|
| `wifi_mqtt_task` | 4096 B | Só chama `wifi_is_connected()` e `mqtt_is_connected()` — leves |
| `camera_task` | 6144 B | `camera_capture()` usa o driver `esp32-camera`, que tem variáveis locais grandes |
| `comm_task` | 6144 B | Chama `do_send_chunked()`, `sm_process_msg()` — mas buffers são estáticos, então cabe |
| `actuator_task` | 3072 B | `actuators_apply_result()` só chama `ledc_set_duty` e `vTaskDelay` |

**Stack overflow**: se uma task escreve além do limite, corrompe a stack da vizinha ou trava o sistema. Defesa no seu firmware: `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` no `sdkconfig.defaults`. Se acontecer, o FreeRTOS dispara panic **na hora**, não depois.

**Como diagnosticar overflow em runtime:**

```c
UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
ESP_LOGI(TAG, "stack sobra: %u words", high_water);
```

Se `high_water` estiver <200 palavras, aumente a stack da task.

---

## 3. Os quatro mecanismos de sincronização que você **realmente** usou

Vou percorrer cada um, mostrando onde aparece no código e **por que aquele** (e não outro).

### Queue (fila)

**O que é:** canal FIFO de tamanho fixo, com cada item do tamanho especificado na criação. `xQueueSend` bloqueia se cheia (ou timeout), `xQueueReceive` bloqueia se vazia (ou timeout).

**Onde você usa:** três filas em `tasks.c`:

```c
s_camera_cmd_queue    — comm_task → camera_task     (comandos "captura")
s_comm_msg_queue      — todos → comm_task           (mensagens heterogêneas)
s_actuator_cmd_queue  — comm_task → actuator_task   (APROVADO/REPROVADO)
```

**Por que Queue e não Event Group aqui:** porque as mensagens carregam **dados** (`comm_msg_t` com `image_id` + resultado) e a ordem importa (não pode haver duas capturas em voo ao mesmo tempo). Event Group não carrega payload; Mutex não enfileira. Queue é a primitiva certa quando o que se quer transmitir **é um dado**, não apenas um sinal.

**Comportamento crucial:** `xQueueReceive` com timeout bloqueia a task chamadora **sem gastar CPU**. `comm_task` fica Blocked até 30 s — 0% de uso de CPU enquanto espera. Isso é o pilar do design de baixo consumo.

**No seu log:**
```
I (35474) TASKS: Timer de captura expirou — disparando ciclo
```
Essa linha vem de `check_state_timeouts()`, que é chamada após `xQueueReceive(..., wait)`. O `wait` era o timeout do estado `IDLE` (30 s). Quando expira **sem mensagem**, `comm_task` processa o timeout e chama `TRIGGER_CAPTURE`.

### Event Group

**O que é:** uma variável de 24 bits compartilhada, onde cada bit é um "evento". Tasks podem **esperar** por uma combinação de bits (todos ou qualquer) e podem **setar/limpar** bits de qualquer task ou ISR.

**Onde você usa:** em `wifi.c` e `mqtt.c`, para sinalizar "conectado":

```c
// wifi.c, no handler de WIFI_EVENT_STA_CONNECTED:
xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);

// em wifi_wait_connected():
xEventGroupWaitBits(s_event_group, WIFI_GOT_IP_BIT, ...);
```

**Por que Event Group e não Queue:** porque a operação típica é "**espere até um dos N eventos**" ou "**espere até todos**". Um Event Group permite `xEventGroupWaitBits(eg, BIT_A | BIT_B, pdFALSE, pdTRUE, ...)` — "espere A **e** B". Com fila, você teria que receber duas mensagens separadas e armazenar o estado em variável. Event Group é o padrão para sincronização de conexão em rede.

**No seu projeto:** `wifi_wait_connected()` e `mqtt_wait_connected()` **bloqueiam** a `main_task` (durante `app_main`) esperando `WIFI_GOT_IP_BIT` / `MQTT_CONNECTED_BIT` com timeout. Isso evita fazer `vTaskDelay(1000)` + checar estado em loop — a task dorme até que o bit esteja setado.

**Ponto sutil:** você usa **dois bits** em `wifi.c` — `WIFI_CONNECTED_BIT` (associado ao AP) e `WIFI_GOT_IP_BIT` (DHCP concluído). O firmware espera `GOT_IP` porque antes dele o socket não funciona. Isso já apareceu na explicação da Parte 1.

### Mutex

**O que é:** um "chaveiro" binário com **herança de prioridade**: só uma task tem a chave por vez. Se a task A segura o mutex e a task B (mais prioritária) tenta pegar, a prioridade de A é **temporariamente elevada** para evitar que uma task C (de prioridade intermediária) preempte A e trave B indiretamente.

**Onde você usa:** dois mutexes estáticos:

```c
// tasks.c — protege s_chunk_buf
static SemaphoreHandle_t s_chunk_mutex;

// ota_server.c — serializa uploads
static SemaphoreHandle_t g_upload_mutex;
```

E um terceiro em `ota_common.c` (`g_ota_mutex`) protegendo o estado do subsistema OTA.

**Por que Mutex e não Spinlock:** porque o recurso protegido é **buffer de 2 KB** ou **estado que pode ser modificado em operação demorada**. O `memcpy` de 2 KB leva microssegundos — se um spinlock estivesse segurando, seria aceitável. Mas se um mutex está sendo mantido por 15 s (como no `g_upload_mutex` durante um POST de 1 MB), spinlock **mataria o sistema**. Mutex bloqueia a task e libera CPU.

**Ponto crítico do seu código:** `ota_server.c` mantém `g_upload_mutex` durante **todo** o POST (60–10 s dependendo da rede). Durante esse tempo, qualquer outra tentativa de upload recebe `409 Conflict` imediato (`xSemaphoreTake(g_upload_mutex, 0)` retorna `pdFALSE`). O mutex protege a integridade da partição — se dois uploads concorressem, escreveriam bytes misturados na `ota_1` e o firmware resultante estaria corrompido.

### Spinlock (`portMUX_TYPE`)

**O que é:** um lock de **busy-wait**: se ocupado, a task **gira em loop** até liberar. **Desabilita interrupções** naquele core durante a seção crítica.

**Onde você usa:** em contadores e flags de controle:

```c
// mqtt.c
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_published;
// ...
portENTER_CRITICAL(&s_lock);
s_published++;
portEXIT_CRITICAL(&s_lock);

// ota_client.c
static portMUX_TYPE s_ctx_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ctx_in_use;
```

**Por que Spinlock e não Mutex:** o que está protegido são **variáveis inteiras** modificadas em operações de nanossegundos (`s_published++`, `s_ctx_in_use = true`). Usar mutex aqui seria **mais caro** que o próprio trabalho:

| Operação | Custo aproximado |
|----------|-----------------|
| `portENTER_CRITICAL` + `s_published++` + `portEXIT_CRITICAL` | ~50 ciclos |
| `xSemaphoreTake(mutex)` + `++` + `xSemaphoreGive(mutex)` | ~200 ciclos + possível troca de contexto |

Além disso, spinlock é a **única primitiva segura em ISR**. Mutex não pode ser chamado de interrupção.

**Regra prática:** se o que está dentro da seção crítica cabe em poucas linhas que manipulam variáveis escalares, **spinlock**. Se envolve `memcpy`, chamada de sistema, I/O ou pode demorar, **mutex**.

### Flag volatile (`g_ota_busy`)

**O que é:** não é uma primitiva do FreeRTOS — é uma variável global marcada `volatile`, escrita por uma task e lida por outra **sem sincronização explícita**.

**Onde você usa:**

```c
// ota_common.c
volatile bool g_ota_busy = false;

// ota_server.c (no início do POST)
g_ota_busy = true;

// ota_client.c (no início da task)
g_ota_busy = true;

// tasks.c (no loop da comm_task)
if (g_ota_busy) {
    vTaskDelay(pdMS_TO_TICKS(500));
    continue;
}
```

**Por que volatile e não mutex/queue:** porque é uma **flag de estado**, um único bit, lido e escrito atomicamente no Xtensa (leitura/escrita de `bool` é uma instrução). O `volatile` diz ao compilador "não otimize, esta variável muda fora desta função". Não há race condition possível em `bool` (não existe write parcial).

**Alternativa mais "correta" tecnicamente:** `atomic_bool` do C11 (`stdatomic.h`). O ESP-IDF suporta. `volatile bool` funciona na prática no Xtensa, mas `atomic_bool` é o padrão correto. No seu projeto, `volatile bool` foi escolhido por simplicidade — funciona porque o compilador gera leitura/escrita atômica em `bool` sem otimizar.

**Foi essa a solução do bug anterior:** você tentou usar `ota_get_state()` (que pega `g_ota_mutex`) no loop da `comm_task`, e isso travava o `app_main` por interação com o scheduler. Trocando por `volatile bool` puro, sumiu o deadlock.

### Task Notification

**O que é:** um mecanismo leve de sinalização direta entre **duas** tasks (ou ISR → task), com valor `uint32_t`. Custa ~10× menos que uma Queue, mas só serve para **uma** task consumir.

**Onde você usaria** (mas **não usou** — decisão consciente): seria útil para um botão físico interromper a `comm_task`, ou para a `actuator_task` avisar "terminei" (mas você usou `comm_msg_queue` para isso).

**Por que você não usou:** o projeto tem **poucas** notificações pontuais. O `comm_msg_queue` já é versátil, e adicionar Task Notifications exigiria uma task por notificação — mais complexidade do que o ganho. **Task Notification é valiosa quando você tem 10+ tasks que se notificam intensamente** (o seu caso é 4 tasks com poucas mensagens por minuto).

**Comparação:** o `comm_msg_queue` recebe 1 mensagem a cada 30 s (o trigger de captura). Task Notification otimizaria 10 µs nesse cenário — irrelevante.

---

## 4. As 4 tasks do seu firmware

Agora que você conhece os mecanismos, aqui está o mapa completo:

### `wifi_mqtt_task` — prio 5, core 0

```c
while (true) {
    bool wifi_ok = wifi_is_connected();
    bool mqtt_ok = mqtt_is_connected();
    bool now_up  = wifi_ok && mqtt_ok;

    if (now_up != was_up) {
        comm_msg_t msg = {
            .type = now_up ? COMM_MSG_NETWORK_READY : COMM_MSG_NETWORK_DOWN,
        };
        xQueueSend(s_comm_msg_queue, &msg, 0);
        was_up = now_up;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

**Prioridade 5** porque rede é o ativo mais crítico do sistema. Se ela cai, todo o resto perde o propósito. Roda a cada 500 ms — **polling** com `vTaskDelay`, porque o custo de duas consultas por 500 ms (~5 µs) é desprezível, e usar dois event groups em um só scheduler exigiria uma task adicional.

**Não faz I/O de rede direto.** Só consulta. Quem gerencia conexão é `wifi.c` e `mqtt.c` internamente (nas tasks do IDF).

### `camera_task` — prio 4, core 1

```c
while (true) {
    camera_cmd_t cmd;
    if (xQueueReceive(s_camera_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        camera_frame_t *frame = NULL;
        esp_err_t err = camera_capture(&frame);
        if (err != ESP_OK || frame == NULL) {
            // enfileira COMM_MSG_CAMERA_FAILED
        } else {
            // enfileira COMM_MSG_CAMERA_FRAME com o frame
        }
    }
}
```

**Prioridade 4, no core 1** (oposto ao Wi-Fi do IDF, que está no core 0). Isso permite que **captura** e **rede** rodem em paralelo **fisicamente** em dois cores, sem competir por CPU. Isso foi crítico na otimização do OTA: `esp_ota_write` roda no core 0 e `camera_task` no core 1 pode continuar sem travar o HTTP server.

**Bloqueia em `portMAX_DELAY`** — fica dormindo indefinidamente até receber comando. Consumo de CPU enquanto ociosa: 0%.

### `comm_task` — prio 3, core 0

O coração do orquestrador. Roda a máquina de estados, envia imagens via MQTT, gerencia timeouts, pausa durante OTA.

```c
while (true) {
    if (g_ota_busy) {   // ← pausa durante OTA
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
    }
    TickType_t wait = compute_state_timeout(&s_comm_ctx);
    comm_msg_t msg;
    if (xQueueReceive(s_comm_msg_queue, &msg, wait) == pdTRUE) {
        process_comm_msg(&s_comm_ctx, &msg);
    }
    check_state_timeouts(&s_comm_ctx);
    handle_transitions(&s_comm_ctx);
}
```

**Prioridade 3** porque é orquestração — não é urgente por si só. Quando precisa de ação rápida (ex: enviar chunk), chama `mqtt_publish_chunk()` que bloqueia até a rede aceitar.

**No core 0** porque compartilha com `wifi_mqtt_task` — todas as operações de rede estão no core 0. Isso reduz **cache misses** porque o driver Wi-Fi do IDF já está lá, e o `esp_ota_write` (que desabilita cache) está no mesmo core.

**Por que `g_ota_busy` no topo do loop:** sem essa checagem, a `comm_task` **competiria** com o HTTP server durante OTA. O MQTT publicaria chunks, a rede dividiria banda, e o upload levaria 66 s (era o seu caso). Com a checagem, a `comm_task` dorme 500 ms por iteração — usa 0% de CPU e libera toda a rede para o upload. Resultado: 10 s.

### `actuator_task` — prio 2, core 1

```c
while (true) {
    actuator_cmd_t cmd;
    if (xQueueReceive(s_actuator_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        actuators_apply_result(cmd.result);   // bloqueia ~3 s
        comm_msg_t msg = { .type = COMM_MSG_APPLY_DONE };
        xQueueSend(s_comm_msg_queue, &msg, pdMS_TO_TICKS(100));
    }
}
```

**Prioridade 2** — a mais baixa. Aceita 3 s de sequência bloqueante, porque ninguém mais depende dela no meio. O servo gira, o LED acende, o buzzer apita — quem se importa é o usuário, não o firmware.

**No core 1** com `camera_task`, porque ambos são periféricos. Não competem entre si porque raramente são acionados ao mesmo tempo (a câmera está ociosa durante `APPLYING`).

**Envia `APPLY_DONE` de volta**, mesmo em caso de erro do atuador. Isso força a máquina de estados a sair de `APPLYING` — melhor ir para `IDLE` com falha registrada do que travar aguardando um ack que nunca vem.

---

## 5. Uma lição concreta: quando o RTOS atrapalha (o bug do `is_ota_busy`)

O bug que travou o `app_main` é um exemplo clássico de **interação sutil entre RTOS e scheduler**. Recapitulemos:

**O que você fez (baseado na minha sugestão anterior):**

```c
// comm_task (prio 3), core 0
if (is_ota_busy()) {   // ← chama ota_get_state()
    vTaskDelay(...);
    continue;
}

// dentro de is_ota_busy():
ota_state_t s = ota_get_state();   // ← pega g_ota_mutex com timeout 1000ms
```

**O que aconteceu:** `comm_task` roda no core 0, prioridade 3. `app_main` também roda no core 0, prioridade **1**. Quando `comm_task` foi criada, ela **preemptou** o `app_main` — e nunca devolveu CPU porque:

- `ota_get_state()` tenta pegar `g_ota_mutex`
- Se o mutex está preso por algum motivo (mesmo sendo 1000 ms de timeout, pode ser loop)
- Se `xSemaphoreTake` retorna sem liberar corretamente
- O `app_main` nunca consegue rodar porque `comm_task` **sempre** tem prioridade maior

**Resultado:** `app_main` nunca chegou em `ota_local_server_start()`. A `comm_task` ficou em loop infinito chamando `is_ota_busy()` a cada 500 ms, mas sem progredir o sistema.

**Correção:** trocar `ota_get_state()` por uma flag `volatile bool`. Sem mutex, sem scheduler, sem interação.

**Lição:** quando você chama uma função de um módulo diferente dentro do **topo** de uma task de alta prioridade, você está sujeito a **qualquer bloqueio que essa função tenha**. `ota_get_state()` era inocente — 1000 ms de timeout, deveria funcionar. Mas a interação com `app_main` de prioridade 1 e o mutex do subsistema OTA criaram um cenário onde a task nunca cedeu CPU. Não é bug de `ota_common.c` nem de `tasks.c` — é **ordem de inicialização** + **prioridade** + **mutex** combinados.

**Como evitar no futuro:**

1. **Nunca chame função que pega mutex em loop de task de alta prioridade sem `vTaskDelay`.** O `vTaskDelay` do seu loop dava CPU — mas o `app_main` precisava de **muito mais** tempo de CPU do que o `vTaskDelay(500)` liberava.
2. **Prefira flags volatiles para status de módulos.** Status é leitura barata, mutex é proteção de consistência — para "estou ocupado?", flag basta.
3. **Não crie tasks antes de terminar `app_main`** se as tasks dependem de coisas que o `app_main` ainda vai inicializar.

---

## 6. O que levar desta Parte 2

1. **RTOS = determinismo, não velocidade.** Ele não acelera rede nem flash — só garante que a task certa rode no momento certo.

2. **Uma task bloqueada não gasta CPU.** Todo o design do seu firmware se apoia em `xQueueReceive` / `xEventGroupWaitBits` / `vTaskDelay` bloqueantes.

3. **Os 5 mecanismos, resumidos:**

| Mecanismo | Uso no projeto | Por quê |
|-----------|----------------|---------|
| **Queue** | 3 filas entre tasks | Transmitir **dados** entre produtor/consumidor |
| **Event Group** | Wi-Fi conectado, MQTT conectado | Esperar **bits** de estado combinado |
| **Mutex** | Buffers, upload OTA, estado OTA | Recurso usado por tempo **longo** |
| **Spinlock** | Contadores, `s_ctx_in_use` | Operações **nanossegundos** em variáveis escalares |
| **Flag volatile** | `g_ota_busy` | Status binário sem proteção necessária |
| **Task Notification** | **Não usado** | Poucas notificações pontuais, queue cobre |

4. **As 4 tasks:** prioridade 5 (rede) → 4 (câmera) → 3 (orquestração) → 2 (atuadores). Cores divididos: rede no core 0, periféricos no core 1.

5. **A lição do bug:** `ota_get_state()` travou a `comm_task` porque combinou prioridade alta, mutex, e ordem de inicialização. Flags volatiles são preferíveis para status — mutex é para consistência.
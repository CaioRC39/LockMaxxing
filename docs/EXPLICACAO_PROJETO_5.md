# Explicação Consolidada — Parte 5/6: Robustez e Qualidade

Esta parte responde: *como o firmware se protege de erros*, *como você descobre o que deu errado*, *o que os testes provam* e *como diagnosticar problemas reais*. Tudo referenciado ao seu código e aos bugs que você efetivamente encontrou.

---

## 1. Memória — o mapa completo

### Os dois heaps

O ESP32-S3 tem duas áreas de RAM utilizáveis:

| Área | Tamanho | Velocidade | Uso típico |
|------|---------|-----------|------------|
| **RAM interna** (SRAM) | ~512 KB | 240 MHz, latência ~5 ns | Pilhas de task, drivers, buffers de rede, TLS |
| **PSRAM** (externa) | 8 MB (na N16R8) | ~80 MHz, latência ~50 ns | Framebuffers da câmera, buffers grandes |

**A diferença de velocidade importa:** PSRAM é ~10× mais lenta. Quando você coloca Wi-Fi na PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`), a latência de rede piora porque cada acesso aos buffers leva 50 ns em vez de 5 ns. Você **removeu essa opção** na otimização, e o OTA passou de 66 s para 10 s.

### `esp_psram_is_initialized()` — a primeira checagem

Em `camera.c`:

```c
if (!esp_psram_is_initialized()) {
    ESP_LOGE(TAG, "PSRAM não inicializada — impossível alocar framebuffers");
    return ESP_ERR_NO_MEM;
}
```

**Por que falha rápido:** sem PSRAM, `esp_camera_init()` falharia com `ESP_ERR_NO_MEM` genérico. Mas **por quê**? Poderia ser falta de PSRAM (`CONFIG_SPIRAM_MODE_OCT` ausente), ou memória fragmentada. A mensagem explícita elimina a ambiguidade.

### Estratégia: buffers estáticos no lugar de `malloc`

Você tem **quatro** buffers estáticos:

| Buffer | Arquivo | Tamanho | Proteção |
|--------|---------|---------|----------|
| `s_chunk_buf` | `mqtt.c` | 2 KB | `s_chunk_mutex` |
| `s_download_buf` | `ota_client.c` | 4 KB | `s_ctx_in_use` (spinlock) |
| `s_upload_buf` | `ota_server.c` | 4 KB | `g_upload_mutex` |
| `s_ctx` | `ota_client.c` | ~600 B | `s_ctx_in_use` (spinlock) |

**Total:** ~11 KB reservados em `.bss` (RAM interna, permanente).

**Por que não `malloc`:**

| Aspecto | `malloc` dinâmico | Buffer estático |
|---------|-------------------|-----------------|
| Fragmentação | Pode fragmentar o heap | Zero (reservado no link) |
| Falha em runtime | `malloc` pode retornar `NULL` | Impossível |
| Overhead | ~16–32 B por alocação | Zero |
| Tamanho conhecido em compile | Não | Sim |
| Thread-safety | Precisa mutex | Já protegido pelo design |

**Por que funciona neste projeto:** os buffers têm tamanho fixo e conhecido. `CONFIG_MQTT_CHUNK_SIZE = 2048` é constante. Não há caso de "chunk maior que 2048" — o protocolo impõe isso.

### Onde `malloc` ainda aparece

Três lugares:

1. **`esp_http_client`** — aloca buffers de socket e TLS internamente
2. **`esp_mqtt_client_publish`** — copia o payload para o outbox
3. **`httpd`** — aloca estrutura por request

Nenhum desses é **código seu**. São bibliotecas do IDF que fazem alocação transacional (aloca → usa → libera em uma única operação). Não fragmentam porque o padrão de uso é curto e repetitivo.

### cJSON — o caso especial

O parser de resultado:

```c
cJSON *root = cJSON_ParseWithLength(data, len);  // ~200–500 B de malloc
// ...processa...
cJSON_Delete(root);                               // free de tudo
```

**Transacional:** aloca uma árvore temporária, usa, libera. Ciclo de vida: ~50 µs por mensagem. Uma mensagem a cada 30 s.

**Fragmentação:** zero na prática. O `heap_caps_malloc` do IDF tem **multiplas arenas por tamanho**. Alocações de ~500 B vão para uma arena dedicada, e como o padrão é sempre o mesmo, o bloco é reusado imediatamente.

**A alternativa (parser manual)** foi testada e falhou com 2 bugs. O custo dos 500 B é **desprezível** comparado à complexidade de manter um parser JSON correto.

### `heap_caps_malloc` — quando você quer controle

Em `ota_client.c`, se você quisesse alocar o buffer de download explicitamente na RAM interna:

```c
buf = (uint8_t *)heap_caps_malloc(OTA_CLIENT_BUF_SIZE,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
```

`heap_caps_malloc` permite especificar **qual área de memória** usar:

| Capability | Significado |
|------------|-------------|
| `MALLOC_CAP_INTERNAL` | RAM interna (rápida, limitada) |
| `MALLOC_CAP_SPIRAM` | PSRAM (lenta, abundante) |
| `MALLOC_CAP_8BIT` | Acesso byte a byte (não DMA) |
| `MALLOC_CAP_DMA` | Acesso via DMA (restringe a certas regiões) |

Você **não usa mais** esse padrão (agora é buffer estático), mas a documentação cobre o caso.

### Sobre `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`

No seu `sdkconfig.defaults`:

```ini
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
```

Significa: **alocações menores que 16 KB vão para RAM interna mesmo quando PSRAM está disponível**. Isso é importante:

- Buffers de rede, TLS, drivers → RAM interna (rápida)
- Framebuffers da câmera (96 KB) → PSRAM (lenta mas abundante)

Sem esse limite, o `malloc` comum poderia jogar buffers pequenos na PSRAM, degradando performance silenciosamente.

### `NO-SOI` — o sintoma de memória (mas não da RAM)

Você viu no log:

```
I (122592) cam_hal: NO-SOI - JPEG start marker missing
```

Isso **não é** falta de RAM. É o sensor entregando um frame corrompido. Causas:

| Causa | Sinais |
|-------|--------|
| Ruído na alimentação (mais comum) | Aparece junto com movimento do servo |
| Cabo flat mal encaixado | Aparece aleatoriamente |
| Sensor superaquecendo | Aparece após uso prolongado |

**Se for RAM**, o sintoma é diferente: `esp_camera_fb_get()` retorna `NULL` ou o `frame->len` é zero. `NO-SOI` significa "frame chegou, mas os primeiros 2 bytes não são `FF D8`".

---

## 2. Tratamento de erros

### `esp_err_t` — o tipo de retorno universal

Todo código do IDF retorna `esp_err_t` (que é `int`). `ESP_OK = 0`; tudo diferente é erro.

```c
esp_err_t err = esp_camera_init(&cfg);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init() falhou: %s", esp_err_to_name(err));
    return err;
}
```

**`esp_err_to_name`** traduz o código numérico em string: `0x101` → `"ESP_ERR_NO_MEM"`. Sem isso, o log mostraria só um número.

**Erros comuns:**

| Código | Nome | Significado |
|--------|------|-------------|
| 0 | `ESP_OK` | Sucesso |
| -1 | `ESP_FAIL` | Erro genérico |
| 0x101 | `ESP_ERR_NO_MEM` | Sem memória |
| 0x102 | `ESP_ERR_INVALID_ARG` | Argumento inválido |
| 0x103 | `ESP_ERR_INVALID_STATE` | Estado inválido |
| 0x105 | `ESP_ERR_NOT_FOUND` | Não encontrado |
| 0x107 | `ESP_ERR_TIMEOUT` | Timeout |

### Propagação de erros

O padrão do projeto:

```c
esp_err_t camera_init(void)
{
    // ...
    if (!esp_psram_is_initialized()) {
        return ESP_ERR_NO_MEM;
    }
    // ...
    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        return err;    // propaga código do IDF
    }
    // ...
    return ESP_OK;
}
```

**O chamador decide o que fazer:**

```c
// main.c
err = camera_init();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Falha ao inicializar câmera: %s", esp_err_to_name(err));
    critical_ok = false;    // ← flag de decisão (rollback)
}
```

**Por que `camera_init` não chama `esp_restart()`:** porque ele não sabe o contexto. É chamado em `app_main` (onde o reboot é razoável) e potencialmente em testes (onde não é). **O chamador decide**.

### `goto cleanup` — o padrão de limpeza

Em funções com múltiplos recursos (`ota_client_perform_download`, `update_post_handler`):

```c
esp_err_t ota_client_perform_download(...)
{
    esp_http_client_handle_t http = NULL;
    esp_ota_handle_t ota = 0;
    bool sha_initialized = false;

#define OTA_FAIL(code, fmt, ...)                                   \
    do {                                                           \
        err = (code);                                              \
        snprintf(out_message, msg_size, fmt, ##__VA_ARGS__);       \
        ESP_LOGE(TAG, "%s", out_message);                          \
        goto cleanup;                                              \
    } while (0)

    http = esp_http_client_init(&cfg);
    if (http == NULL) OTA_FAIL(ESP_FAIL, "init falhou");

    err = esp_ota_begin(...);
    if (err != ESP_OK) OTA_FAIL(err, "begin falhou");

    // ...

cleanup:
    if (sha_initialized) mbedtls_sha256_free(&sha_ctx);
    if (ota) esp_ota_abort(ota);
    if (http) { esp_http_client_close(http); esp_http_client_cleanup(http); }
    return err;
}
```

**Por que `goto` é aceitável em C (neste caso):** o alternativo seria repetir o bloco de liberação em cada caminho de erro. Com 5+ recursos, isso gera código duplicado e o **bug clássico**: esquecer de liberar um recurso em um caminho. O `goto cleanup` é o padrão do kernel Linux e do próprio IDF.

**Regras do `goto` seguro:**

1. **Só `goto` para frente** (nunca para trás — isso é o "goto espaguete")
2. **Um só label `cleanup:`** no final da função
3. **Libera recursos na ordem inversa** da aquisição
4. **Checa se cada ponteiro é válido** (pode ser que o erro tenha ocorrido antes da alocação)

### `ESP_ERROR_CHECK` — quando usar e não usar

```c
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
```

Se `esp_wifi_init` falhar, `ESP_ERROR_CHECK` **aborta** (panic). Útil quando:

- A falha é irrecuperável
- Você **quer** um crash explícito para debug

**Onde você não usa:**

```c
err = camera_init();
if (err != ESP_OK) {
    // loga, decide se é crítico
}
```

Você **não** quer panic em `camera_init` falhando — quer **decisão**. Um firmware que aborta no primeiro erro é frágil. Um firmware que **degrada** (câmera falhou, mas Wi-Fi ainda funciona) é robusto.

**Regra prática:**

- Inicialização de módulos essenciais no boot: checar erro, decidir
- Chamadas dentro de funções de negócio: checar erro, propagar
- `ESP_ERROR_CHECK`: só em testes ou quando o erro é bug de lógica (nunca deveria acontecer)

---

## 3. Watchdog — as três redes de segurança

O ESP-IDF tem **três watchdogs** independentes:

### 1. Task Watchdog (TWDT) — 10 s

```ini
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP_TASK_WDT_PANIC=y
```

**O que monitora:** qualquer task registrada no WDT. Se ela não chamar `esp_task_wdt_reset()` no timeout, o chip reinicia.

**Por padrão, só a IDLE task é monitorada** (e você desabilitou isso no `sdkconfig.defaults`, porque IDLE fica muito tempo bloqueada — é normal). Se você registrasse `comm_task` no WDT, ela precisaria "alimentar" o WDT a cada ≤10 s.

**No seu projeto:** você **não registrou** nenhuma task explicitamente. O TWDT serve como rede de segurança genérica — se uma task travar de forma que o IDLE não consiga rodar por 10 s, o chip reinicia.

**Sintoma no log:**

```
E (10xxx) task_wdt: Task watchdog got triggered.
E (10xxx) task_wdt: CPU 0: main
```

### 2. Interrupt Watchdog (INT_WDT) — 800 ms

```ini
CONFIG_ESP_INT_WDT=y
CONFIG_ESP_INT_WDT_TIMEOUT_MS=800
```

**O que monitora:** se interrupções estão sendo tratadas em tempo aceitável. Cada ISR desabilita interrupções por um tempo. Se **qualquer** ISR levar mais de 800 ms (ou se interrupções ficarem desabilitadas por muito tempo), reinicia.

**Por que 800 ms e não os 300 ms default:** operações de flash (escrever OTA, salvar NVS) **desabilitam interrupções por dezenas de ms por setor**. Em placas com PSRAM octal + flash quad, a janela pode chegar a 200–300 ms. O default de 300 ms é apertado demais. 800 ms é a margem que evita **falso positivo** em operação normal.

**Sintoma no log:**

```
E (10xxx) esp_int_wdt: Interrupt watchdog timeout
E (10xxx) esp_int_wdt: The following ISR(s) failed to yield
```

### 3. RTC Watchdog (RTC_WDT) — ~9 s (padrão desabilitado)

Monitora o bootloader e a inicialização. Usado em produção com Secure Boot para evitar boot loop. **Você não usa.**

### Por que essas três redes importam

O firmware **nunca** deve travar completamente. Cenário:

1. `comm_task` entra em loop infinito (bug de lógica)
2. `wifi_mqtt_task` continua rodando (task independente)
3. **Mas** `comm_task` nunca libera a CPU, então o IDLE não roda
4. TWDT detecta e reinicia após 10 s

**Sem TWDT:** o firmware ficaria "vivo" (Wi-Fi respondendo) mas **nunca processaria uma nova captura**. O usuário veria "o dispositivo está no ar mas não faz nada". É o pior tipo de bug — silencioso e persistente.

**Com TWDT:** reboot automático em ≤10 s. O firmware volta a funcionar. O log mostra **por quê** (task travada).

### Reset reasons no boot

No seu `main.c`, você loga:

```c
ESP_LOGI(TAG, "Motivo do reset: %s", reason_str);
```

| Reason | Significado |
|--------|-------------|
| `power-on` | Ligou a placa |
| `software` | `esp_restart()` chamado (fim do OTA) |
| `panic` | Crash (divisão por zero, assert, Guru Meditation) |
| `task-wdt` | Task Watchdog disparou |
| `int-wdt` | Interrupt Watchdog disparou |
| `brownout` | Fonte de alimentação fraca |

**Foi assim que você confirmou que o OTA funcionou:** o log mostrou `Motivo do reset: software` — sinal de que o `esp_restart()` do callback foi chamado, não um crash.

---

## 4. Logs — como diagnosticar pelo monitor

### Níveis

```c
ESP_LOGE(TAG, "...")    // Erro — sempre exibir
ESP_LOGW(TAG, "...")    // Warning — comportamento anormal recuperável
ESP_LOGI(TAG, "...")    // Info — marcos de progresso
ESP_LOGD(TAG, "...")    // Debug — detalhe útil em desenvolvimento
ESP_LOGV(TAG, "...")    // Verbose — detalhe extremo
```

Configurado em `sdkconfig.defaults`:

```ini
CONFIG_LOG_DEFAULT_LEVEL_INFO=y      // INFO, WARN, ERROR aparecem
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y     // permite elevar para DEBUG em runtime
```

**Elevar nível em runtime:**

```c
esp_log_level_set("MQTT", ESP_LOG_DEBUG);
```

Útil em diagnóstico — você não precisa recompilar para ver logs extras.

### Tags por módulo

Seu `config.h` centraliza:

```c
#define CONFIG_LOG_TAG_MAIN     "APP"
#define CONFIG_LOG_TAG_CAMERA   "CAMERA"
#define CONFIG_LOG_TAG_WIFI     "WIFI"
#define CONFIG_LOG_TAG_MQTT     "MQTT"
#define CONFIG_LOG_TAG_ACTUATOR "ACTUATOR"
#define CONFIG_LOG_TAG_STATE    "STATE"
#define CONFIG_LOG_TAG_TASKS    "TASKS"
```

Cada módulo usa `static const char *TAG = CONFIG_LOG_TAG_X;`.

**Vantagem:** o monitor mostra:

```
I (2452) CAMERA: Câmera inicializada: OV5640, frame size 11, qualidade 12
I (5185) WIFI: IP obtido: 192.168.15.7
I (5574) MQTT: Conectado ao broker
I (5623) STATE: TRANSITION: INIT --[NETWORK_READY]--> IDLE
```

Você filtra por tag: `idf.py monitor | Select-String "STATE"`.

### Um log bem escrito

**Mau:**

```c
ESP_LOGI(TAG, "deu ruim");
```

**Bom:**

```c
ESP_LOGE(TAG, "mqtt_publish_chunk %u/%u falhou: %s",
         (unsigned)(i + 1), (unsigned)total_chunks, esp_err_to_name(err));
```

O bom responde a três perguntas: **onde** (função/módulo), **o quê** (chunk 2/21), **por quê** (código do erro).

### O log como "trace"

No seu projeto, o log é **suficientemente detalhado** para reconstruir toda a execução. Exemplo:

```
I (676410) STATE: TRANSITION: IDLE --[TRIGGER_CAPTURE]--> CAPTURING
I (676410) TASKS: Capturando frame...
I (676414) CAMERA: Frame: 800×600, 42288 bytes, 0 ms
I (676418) TASKS: Frame capturado: 800×600, 42288 bytes
I (676423) STATE: TRANSITION: CAPTURING --[CAPTURE_OK]--> SENDING
I (676429) TASKS: Enviando imagem id=2: 42288 bytes, 21 chunks
I (676436) MQTT: Request publicado: id=2, size=42288, chunks=21
```

Em 26 ms, você sabe **tudo** o que aconteceu: trigger, captura, transição, envio. Se der erro no MQTT, você sabe exatamente **onde** e **quando**.

**Foi assim que você diagnosticou:**

- Timeout de MQTT: `Poll timeout or error, errno=I/O error`
- WiFi caiu: `Desconectado (reason=X)`
- Parser rejeitou: `Campo 'id' fora do range`
- OTA rejeitou: `Certificado não parece ser PEM`
- Rollback funcionou: `Rodando da partição 'ota_1'` + `Firmware confirmado`

---

## 5. Testes — o que os 78 provam e o que **não** provam

### Estrutura

```
tools/host_tests/
├── mocks/
│   ├── esp_err.h
│   ├── esp_log.h
│   └── mock_esp_err.c
├── test_chunking.c        (13 testes)
├── test_json_parser.c     (19 testes)
├── test_sha256_equals.c   (10 testes)
├── test_state_machine.c   (36 testes)
├── run_all.sh
├── run_all.ps1
└── external/cJSON/        (baixado automaticamente)
```

**Total: 78 testes, 0 falhas.**

### O que cada suíte prova

| Suíte | O que valida | Como |
|-------|-------------|------|
| `test_chunking` | `(len + 2047) / 2048` está correto | 13 casos de fronteira (0, 1, 2047, 2048, 2049...) |
| `test_json_parser` | Parser cJSON aceita formatos válidos e rejeita inválidos | 19 casos: JSON truncado, campos ausentes, tipos errados |
| `test_sha256_equals` | Comparação case-insensitive de SHA-256 | NULL, string curta, string longa, case diferente |
| `test_state_machine` | 36 transições, caminhos felizes e de erro | Compila `state_machine.c` de verdade, testa cada estado |

### Como o `state_machine.c` é testado

```c
#include "state_machine.c"   // ← compila o .c REAL, não uma cópia

sm_init();
sm_process_event(SM_EVENT_NETWORK_READY);
assert(sm_get_state() == SM_STATE_IDLE);

sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
assert(sm_get_state() == SM_STATE_CAPTURING);

// ... até APPLYING, ERROR, etc.
```

**Vantagem:** se você mudar uma transição em `state_machine.c`, o teste **detecta automaticamente**. Não há cópia para sincronizar.

**Por que funciona:** `state_machine.c` só depende de `esp_log.h` (mocado) e do próprio `state_machine.h`. Não depende de FreeRTOS, hardware, rede.

### Os 2 bugs reais que os testes pegaram

| Bug | Sintoma no teste | Custo se fosse para produção |
|-----|------------------|------------------------------|
| Loop infinito no parser manual | Teste travou em `[CASE] JSON válido` | `comm_task` travaria na primeira mensagem → watchdog → boot loop |
| Parser aceita JSON truncado | `[FAIL] faltando }` | Mensagem malformada aceita como aprovação → servo gira sem motivo |

**Ambos foram encontrados no PC, em segundos, antes de qualquer flash.** Você não pagaria esse preço com testes no hardware — cada ciclo de debug no hardware leva ~2 min (build, flash, monitor, reproduzir).

### O que os testes **não** provam

| O que não prova | Como testar |
|-----------------|-------------|
| Câmera OV5640 responde | Flash + self-test no boot (log `Sensor detectado: OV5640`) |
| Wi-Fi conecta | Flash + log `WIFI: Associado ao AP` |
| MQTT publica | Flash + Mosquitto + `mosquitto_sub` |
| Servo gira | Flash + observação visual |
| OTA funciona | Flash + push + reboot + self-test |
| Rede é rápida | `ping` + `Measure-Command` no push |
| Sem vazamento de memória | Rodar por horas + `heap_caps_get_free_size()` |

**Regra:** testes de host provam **lógica**, não **integração com hardware**. Os dois são necessários.

### Por que o `run_all.ps1` baixa cJSON

`test_json_parser.c` usa cJSON. O script clona `v1.7.18` do GitHub na primeira execução:

```powershell
git clone --depth 1 --branch v1.7.18 https://github.com/DaveGamble/cJSON.git external/cJSON
```

**Por que a mesma versão do firmware:** o `idf_component.yml` declara `espressif/cjson: "^1.7.19"`. Usar a mesma linha (1.7.x) garante que o comportamento testado no PC bate com o runtime no firmware.

**`external/` está no `.gitignore`.** Re-clone é automático — não precisa comitar.

---

## 6. Os 8 problemas reais que você teve

Cada um com **causa**, **sintoma** e **como você resolveu**. Esta é a parte mais valiosa da Parte 5 — é a memória do projeto.

### 1. `undefined reference to 'ota_sha256_hex_equals'`

**Sintoma:** linker falhou no final do build.

**Causa:** a função existia em `ota_common.c`, mas foi perdida durante a migração para PSA Crypto (é só comparação de strings, não usava mbedtls — mas sumiu junto com outras funções).

**Solução:** reimplementar em `ota_common.c`.

**Lição:** ao migrar bibliotecas, **diffar** os arquivos. Funções que "não deveriam mudar" às vezes são deletadas por engano.

### 2. `multiple definition of 'wifi_deinit'`

**Sintoma:** linker falhou com símbolo duplicado.

**Causa:** o blob binário do Wi-Fi (`libnet80211.a`) do IDF **já define** `wifi_deinit`. A função sua colidia.

**Solução:** renomear para `wifi_teardown`.

**Lição:** nomes "óbvios" (`wifi_init`, `wifi_deinit`) podem colidir com símbolos internos do IDF. Use prefixos (`app_`, `my_`) ou nomes mais específicos.

### 3. `'camera_frame_t' incomplete typedef`

**Sintoma:** `invalid use of incomplete typedef 'camera_frame_t'` em todos os acessos a `frame->len`, `frame->width`.

**Causa:** em `camera.h`, você tinha:

```c
typedef struct camera_fb_t camera_frame_t;   // ← cria struct vazia!
```

A `esp32-camera` **não define** `struct camera_fb_t` — define um `typedef` de struct anônima. Então `struct camera_fb_t` do seu código criava uma struct **nova e vazia**.

**Solução:**

```c
#include "esp_camera.h"        // fornece camera_fb_t
typedef camera_fb_t camera_frame_t;   // alias correto
```

**Lição:** `typedef struct X Y;` cria struct **nova** se `X` não existir. Para fazer alias, use `typedef X Y;`.

### 4. `No server verification option set in esp_tls_cfg_t`

**Sintoma:** `Pull OTA falhou: ESP_ERR_MQTT_X509_CRT_PARSE_FAILED`.

**Causa:** em `ota_client.c`, `crt_bundle_attach = NULL` **sem** nenhuma outra opção de verificação TLS. O ESP-IDF 6.x **proíbe** conexões TLS sem verificação explícita.

**Solução:** adicionar `.common_name = "esp32-camera.local"` + `.cert_pem = SERVER_CERT_PEM`.

**Lição:** no IDF 6.x, "pular TLS" não é mais possível acidentalmente. Use `cert_pem` (pin do cert específico) ou `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` (explícito, com warning).

### 5. `mbedtls_ssl_handshake returned -0x2700`

**Sintoma:** pull falhava com erro `-0x2700` (CN mismatch).

**Causa:** o certificado do `serve` tem `CN=esp32-camera.local`, mas a URL usa **IP** (`https://192.168.15.5:8443`). O mbedtls compara CN com hostname, e falha.

**Solução:** `.common_name = "esp32-camera.local"` no `esp_http_client_config_t`. O `skip_cert_common_name_check = true` **não** funciona em algumas versões — `common_name` explícito sim.

**Lição:** CN match é validação **obrigatória** por padrão. Para IP, você precisa dizer ao mbedtls qual CN esperar.

### 6. `app_main` travou após `Subscribe confirmado`

**Sintoma:** `OTA_SRV: Servidor HTTPS ativo` **nunca** aparecia no log. `Test-NetConnection` retornava `False`.

**Causa:** `comm_task` chamava `is_ota_busy()` → `ota_get_state()` → `xSemaphoreTake(g_ota_mutex)`. A combinação de **prioridade alta** (`comm_task` prio 3) + **mesmo core do `app_main`** (prio 1) + **mutex** fez a `comm_task` **nunca ceder CPU** para o `app_main` terminar.

**Solução:** trocar `ota_get_state()` por `volatile bool g_ota_busy`.

**Lição:** em task de alta prioridade, **nunca** chame função que pega mutex sem `vTaskDelay` garantido. Flags volatiles são preferíveis para status.

### 7. `httpd_req_recv failed` após 17 s de upload

**Sintoma:** upload travava em ~17 s, log `httpd_req_recv failed`.

**Causa:** Wi-Fi com power save (`WIFI_PS_MIN_MODEM` default) entregando pacotes a 60 KB/s. TCP buffer do httpd esvaziava, socket morria por timeout.

**Solução:** `esp_wifi_set_ps(WIFI_PS_NONE)` em `wifi_event_handler`.

**Lição:** **sempre** desligar power save em projetos com OTA ou tráfego intenso. Dobra o consumo, mas o throughput triplica.

### 8. Push levava 66 s

**Sintoma:** 1 MB em 66 s.

**Causa:** três combinadas — `WIFI_PS_MIN_MODEM`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, `comm_task` competindo com upload.

**Solução:** `WIFI_PS_NONE` + remover `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` + `g_ota_busy`.

**Resultado:** **9.85 s**. Ganho de **6.7×**.

**Lição:** otimização **isolada** de cada fator. Se você tivesse aplicado as três juntas sem medir cada uma, não saberia qual resolveu de fato.

---

## 7. OTA robustez — o que garante que não vai corromper a placa

### Os cinco mecanismos de proteção

| Mecanismo | O que protege | Onde no código |
|-----------|---------------|----------------|
| **Partição inativa** | O firmware atual continua rodando durante o download | `partitions.csv` (`ota_0`, `ota_1`) |
| **SHA-256** | Detecta corrupção de transporte | `ota_client.c`, `ota_server.c` |
| **`esp_ota_end()`** | Rejeita imagem sem estrutura válida (magic byte, checksum) | chamado antes de `set_boot_partition` |
| **Rollback** | Reverte se o novo firmware não passar no self-test | `main.c` + `ota_mark_app_invalid_rollback_and_reboot()` |
| **Warmup** | Rejeita OTA nos primeiros 5 min (rede ainda instável) | `ota_server.c` |

### Fluxo de rollback

```
1. Firmware A (na ota_0) recebe OTA.
2. Grava firmware B na ota_1.
3. Marca ota_1 como "bootable".
4. Reboot → bootloader carrega ota_1 (B).
5. Firmware B roda. Estado da partição: PENDING_VERIFY.
6. main.c executa self-test:
   ├─ OK → ota_mark_app_valid() → estado = VALID → rollback cancelado
   └─ FALHA → ota_mark_app_invalid_rollback_and_reboot()
              → bootloader reverte para ota_0 (A) no próximo boot
```

**Janela de rollback:** o firmware B tem **uma chance** de confirmar. Se travar antes de `ota_mark_app_valid()`, o próximo boot reverte automaticamente. É isso que evita "firmware novo entra em boot loop".

### SHA-256 — o que garante

Você calcula SHA-256 **em streaming** durante o download:

```c
mbedtls_sha256_init(&sha_ctx);
// ... durante o download ...
mbedtls_sha256_update(&sha_ctx, buf, n);
// ... ao final ...
mbedtls_sha256_finish(&sha_ctx, computed_hex);

if (!ota_sha256_hex_equals(computed_hex, expected_hex)) {
    return ESP_ERR_INVALID_CRC;
}
```

**Vantagem do streaming:** você não precisa guardar 1 MB na RAM para calcular. Calcula em paralelo com o download.

**O que protege:** corrupção de transporte (byte trocado no Wi-Fi, pacote perdido, bit flip). Se o servidor mandar o `.bin` correto e o Wi-Fi corromper 1 byte, o SHA diverge e o OTA é rejeitado **antes** de `esp_ota_set_boot_partition`. O firmware atual continua intacto.

**O que não protege:** servidor malicioso (que manda um `.bin` com SHA-256 **desse** `.bin`, não do que você esperava). Para isso, você precisaria de **assinatura digital** (Secure Boot V2 — exige eFuse, você decidiu não usar).

### Warmup — por que 300 s

Nos primeiros minutos após boot:

- DHCP pode não ter completado (raro, mas possível)
- TCP slow start (janela pequena → throughput baixo)
- Beacon sync ainda ajustando
- ARP cache frio

OTA durante essa janela **funcionaria** mas seria lenta e propensa a timeout. O warmup evita:

```c
int64_t uptime_s = esp_timer_get_time() / 1000000;
if (uptime_s < CONFIG_LOCKMAXXING_OTA_WARMUP_S) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "60");
    // ... retorna JSON {"error":"warmup","retry_in_s":285}
}
```

O cliente (Python) lê o `Retry-After` e informa o usuário. **Cortesia de protocolo** — não é obrigatório, mas é o comportamento esperado de APIs HTTP bem-feitas.

---

## 8. O que levar desta Parte 5

1. **PSRAM é 10× mais lenta que RAM interna.** Não coloque buffers de rede na PSRAM. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` mantém alocações pequenas na RAM rápida.

2. **Buffers estáticos evitam fragmentação e falha em runtime.** Você tem 4 buffers totalizando ~11 KB em `.bss`. cJSON usa `malloc` transacionalmente — sem problema.

3. **`goto cleanup` é o padrão correto em C** para liberar recursos em todos os caminhos. Usado pelo kernel Linux e pelo IDF.

4. **Watchdog em três camadas:** Task (10 s), Interrupt (800 ms), RTC (opcional). Nenhuma é redundante — cada uma pega um tipo diferente de travamento.

5. **Logs com tag por módulo** permitem filtrar por `STATE`, `MQTT`, `OTA_CLIENT`. E o log é detalhado o bastante para reconstruir toda a execução.

6. **78 host tests encontraram 2 bugs reais** (loop infinito, JSON truncado) que teriam virado boot loop ou comportamento errático no hardware. **Investimento de 150 linhas de teste que economizou horas de debug.**

7. **Os 8 problemas reais** (undefined refs, incomplete typedef, TLS, is_ota_busy, Wi-Fi PS, etc.) não foram previstos. Cada um ensinou algo concreto. **A memória do projeto está nesses 8 casos.**

8. **OTA tem 5 proteções independentes.** Partição inativa, SHA-256, `esp_ota_end`, rollback, warmup. Cada uma resolve um tipo diferente de falha.
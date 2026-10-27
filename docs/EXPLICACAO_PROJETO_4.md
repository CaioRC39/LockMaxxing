# Explicação Consolidada — Parte 4/6: Periféricos

Esta parte responde: *por que cada pino foi escolhido*, *como a câmera OV5640 conversa com o ESP32*, *como o Wi-Fi e o MQTT funcionam por dentro*, *como a imagem é fragmentada* e *como os atuadores são controlados por PWM*. Tudo referenciado ao seu código real.

---

## 1. GPIOs — por que cada pino foi escolhido

O ESP32-S3 tem 45 GPIOs utilizáveis, mas **nem todos são livres** em uma placa ESP32-CAM. Antes de escolher qualquer pino para LED/buzzer/servo, é preciso mapear o que já está comprometido.

### O que **não** pode ser usado

| Faixa | Por que está ocupado | Consequência se usar |
|-------|---------------------|----------------------|
| **GPIO 26–32** | Flash SPI interna | Firmware corrompe, placa não boota |
| **GPIO 33–37** | PSRAM octal (na N16R8) | PSRAM não detectada, câmera falha |
| **GPIO 19, 20** | USB D-/D+ nativo | USB deixa de funcionar; você perde flash/monitor |
| **GPIO 0** | Strapping (modo boot) | Nível baixo no boot = modo download |
| **GPIO 45, 46** | Strapping (tensão VDD_SPI, boot) | Boot imprevisível |
| **GPIO 43, 44** | UART0 (console) | Sem monitor serial |

### O que está ocupado pela câmera (no seu `config.h`)

A câmera DVP usa 14 pinos:

```
D0=11  D1=9   D2=8   D3=10
D4=12  D5=18  D6=17  D7=16
XCLK=15  PCLK=13  VSYNC=6  HREF=7
SIOD=4   SIOC=5
```

Essa pinagem é a **padrão CAMERA_MODEL_ESP32S3_EYE**, que a N16R8 segue. Não é escolha sua — é a placa.

### Os pinos escolhidos para os atuadores

Sobram `1, 2, 3, 14, 21, 38–42, 47, 48`. Você escolheu:

```c
#define CONFIG_GPIO_LED_GREEN   1
#define CONFIG_GPIO_LED_RED     2
#define CONFIG_GPIO_BUZZER      3
#define CONFIG_GPIO_SERVO       21
```

**Critérios da escolha:**

| Pino | Pode ser usado para... | Por que foi escolhido |
|------|------------------------|-----------------------|
| **1, 2, 3** | Saída digital qualquer | Adjacentes fisicamente na placa, facilitam protoboard |
| **21** | Saída digital **ou PWM (LEDC)** | Canal LEDC do S3 pode mapear qualquer pino; 21 é livre e suporta PWM |

**Por que não os outros livres?** `14` é I2C (pode ser usado, mas a câmera já ocupa o I2C dela); `38–42` são adjacentes à antena (ruído); `47, 48` são strapping secundários. `1, 2, 3, 21` são **os mais seguros** da placa.

### Conflito crítico: LEDC timer 0 vs timer 1

O driver da câmera (`esp32-camera`) usa **`LEDC_TIMER_0` / `LEDC_CHANNEL_0`** para gerar o XCLK de 20 MHz. Se o servo usar o mesmo canal, há conflito: quando a câmera inicializa, ela reconfigura o timer 0, e o servo para de responder.

Por isso, no seu `config.h`:

```c
#define CONFIG_SERVO_TIMER      1     // ← não 0
#define CONFIG_SERVO_CHANNEL    1     // ← não 0
```

**Esse foi um bug que você teve na primeira versão** — `CONFIG_SERVO_CHANNEL` era 0, e o servo funcionava no self-test (antes de `camera_init`) mas parava depois. A correção foi mudar para canal 1.

**O que ensina:** LEDs, buzzer e câmera compartilham o hardware do ESP32. A escolha de pino não é só "livre ou não" — é também "livre de conflito de periférico".

---

## 2. Câmera OV5640 — do sensor ao JPEG

### `camera_config_t` em detalhe

O bloco em `camera.c`:

```c
camera_config_t cfg = {
    .pin_pwdn       = CONFIG_CAM_PIN_PWDN,        // -1: não conectado
    .pin_reset      = CONFIG_CAM_PIN_RESET,       // -1: reset por software
    .pin_xclk       = CONFIG_CAM_PIN_XCLK,        // 15: clock principal
    .pin_sccb_sda   = CONFIG_CAM_PIN_SIOD,        // 4: I2C SDA
    .pin_sccb_scl   = CONFIG_CAM_PIN_SIOC,        // 5: I2C SCL

    .pin_d7         = CONFIG_CAM_PIN_D7,          // 8 bits de dados
    .pin_d6         = CONFIG_CAM_PIN_D6,
    .pin_d5         = CONFIG_CAM_PIN_D5,
    .pin_d4         = CONFIG_CAM_PIN_D4,
    .pin_d3         = CONFIG_CAM_PIN_D3,
    .pin_d2         = CONFIG_CAM_PIN_D2,
    .pin_d1         = CONFIG_CAM_PIN_D1,
    .pin_d0         = CONFIG_CAM_PIN_D0,

    .pin_vsync      = CONFIG_CAM_PIN_VSYNC,       // sincronismo vertical
    .pin_href       = CONFIG_CAM_PIN_HREF,        // sincronismo horizontal
    .pin_pclk       = CONFIG_CAM_PIN_PCLK,        // clock de pixel

    .xclk_freq_hz   = CONFIG_CAM_XCLK_FREQ_HZ,    // 20 MHz

    .ledc_timer     = LEDC_TIMER_0,               // fixo no driver
    .ledc_channel   = LEDC_CHANNEL_0,             // fixo no driver

    .pixel_format   = CONFIG_CAM_PIXEL_FORMAT,    // PIXFORMAT_JPEG
    .frame_size     = CONFIG_CAM_FRAMESIZE,       // FRAMESIZE_SVGA (800×600)
    .jpeg_quality   = CONFIG_CAM_JPEG_QUALITY,    // 12

    .fb_count       = CONFIG_CAM_FB_COUNT,        // 2
    .fb_location    = CAMERA_FB_IN_PSRAM,         // framebuffers em PSRAM

    .grab_mode      = CONFIG_CAM_GRAB_MODE,       // CAMERA_GRAB_LATEST

    .sccb_i2c_port  = -1,                         // -1 = cria barramento novo
};
```

### O que cada campo faz

**`pin_pwdn` e `pin_reset` = -1**

A N16R8 não conecta esses pinos do OV5640 ao ESP32. O sensor sai do power-down automaticamente quando recebe XCLK, e o reset é feito por registro I2C. **Se você colocar um número de pino aqui, o driver tenta acioná-lo e falha** — o sensor não responde.

**`xclk_freq_hz = 20 MHz`**

O XCLK é o **clock mestre** do sensor. O OV5640 precisa dele para gerar PCLK (pixel clock), que sincroniza a saída de bytes. Escolhas possíveis:

| Frequência | Efeito |
|-----------|--------|
| 8 MHz | Menos consumo, sensor lento, mais chance de PCLK falhar |
| **20 MHz** | Padrão, funcionou em todos os testes |
| 24 MHz | Alguns módulos OV5640 preferem; mais rápido, mais exigente com PSRAM |

**`pixel_format = PIXFORMAT_JPEG`**

Isso é **a decisão mais importante** da câmera. O OV5640 tem engine JPEG interna — quando configurado para `PIXFORMAT_JPEG`, ele comprime a imagem no próprio sensor e entrega bytes JPEG prontos.

Alternativa: `PIXFORMAT_RGB565` ou `PIXFORMAT_YUV422`. Nesses, o sensor entrega pixels crus, e o ESP32 teria que comprimir com uma biblioteca de JPEG em software. Para 800×600, isso levaria **~1 segundo por frame** (vs ~100 ms com JPEG nativo).

**`frame_size = FRAMESIZE_SVGA (800×600)`**

Resoluções do OV5640 e custo aproximado:

| Resolução | Tamanho JPEG típico | Frame buffer (×2) |
|-----------|--------------------|--------------------|
| QVGA (320×240) | ~10 KB | ~76 KB |
| VGA (640×480) | ~25 KB | ~150 KB |
| **SVGA (800×600)** | **~40 KB** | **~230 KB** |
| UXGA (1600×1200) | ~100 KB | ~920 KB |

Você escolheu SVGA — equilíbrio entre detalhe e tamanho de transmissão. UXGA é possível, mas 100 KB ÷ 2 KB/chunk = 50 chunks, e o tempo total de MQTT fica mais longo.

**`fb_count = 2` e `fb_location = CAMERA_FB_IN_PSRAM`**

O driver mantém **2 framebuffers** em PSRAM. Isso permite **double buffering**:

- O sensor escreve no framebuffer A enquanto o firmware processa o framebuffer B.
- Sem isso, cada captura teria que esperar o framebuffer anterior ser liberado.

**Custo:** 2 × ~96 KB = ~192 KB de PSRAM só para a câmera. Com 8 MB, sobra bastante. Em placas com 4 MB (AI-Thinker), isso é apertado.

**`grab_mode = CAMERA_GRAB_LATEST`**

Com `CAMERA_GRAB_LATEST`:

- O driver captura **continuamente** para os 2 framebuffers.
- Quando você chama `esp_camera_fb_get()`, recebe o frame **mais recente**.
- Latência de captura: **~0 ms** (o frame já está pronto).

Com `CAMERA_GRAB_WHEN_EMPTY`:

- O driver só captura quando você chama `fb_get()`.
- Latência de captura: **~100 ms** (espera o sensor terminar o frame).
- **Problema:** se você capturou no boot e não chamou `fb_get()` de novo, o "próximo frame" seria o de 30 s atrás.

Você escolheu `LATEST` para garantir que cada captura reflete o estado atual do mundo.

**`sccb_i2c_port = -1`**

O driver `esp32-camera` cria um **barramento SCCB** (protocolo I2C simplificado) para configurar o sensor. `-1` = cria um novo nos pinos `SIOD`/`SIOC`. Se você tivesse um I2C já inicializado (ex: BME280), passaria o número do port (`I2C_NUM_0`).

### `set_vflip(1)` para OV5640

Logo após `esp_camera_init()`:

```c
if (g_sensor_pid == 0x5640) {
    s->set_vflip(s, 1);
}
```

**Por que:** o OV5640 na N16R8 é montado **fisicamente invertido** em relação ao OV3660 (o sensor é soldado com giro de 180° no módulo). Sem o flip, a imagem sai de cabeça para baixo.

**Como funciona:** o `set_vflip` escreve em um **registrador do próprio sensor**. O OV5640 inverte a imagem no hardware antes de entregar os bytes ao ESP32. Custo: **zero CPU** (é hardware).

**Detecção condicional:** o `set_vflip` só é aplicado se o PID for `0x5640`. Se você trocar o módulo por um OV3660, ele não aplica — porque o OV3660 está montado na orientação correta.

### `camera_capture()` — o que acontece por dentro

```c
esp_err_t camera_capture(camera_frame_t **out_frame)
{
    int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb  = esp_camera_fb_get();      // ← bloqueia
    int64_t elapsed_us = esp_timer_get_time() - start_us;

    if (fb == NULL) {
        ESP_LOGE(TAG, "esp_camera_fb_get() retornou NULL");
        return ESP_FAIL;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Frame não é JPEG");
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_STATE;
    }
    *out_frame = (camera_frame_t *)fb;
    return ESP_OK;
}
```

**`esp_camera_fb_get()` bloqueia** até um framebuffer estar disponível. Com `CAMERA_GRAB_LATEST`, isso é quase instantâneo (~0 ms) — o driver está sempre capturando, então o frame já está pronto quando você pede.

**`esp_camera_fb_return()`** devolve o framebuffer ao driver. Sem esse return, o driver fica sem framebuffers livres e a próxima `fb_get()` **trava para sempre**. Por isso a política de ownership é crítica: `camera_release()` é chamada **em todos os caminhos** em `tasks.c` (`do_send_chunked`, `on_enter_state(ERROR)`, etc.).

**`fb->format != PIXFORMAT_JPEG`**: sanidade. Se o sensor não estiver com JPEG ativo (por algum motivo), o frame vem em RGB565 ou YUV. Verificar evita mandar bytes crus para o MQTT pensando que é JPEG.

### Estrutura `camera_fb_t`

```c
typedef struct {
    uint8_t   *buf;       // bytes do JPEG (em PSRAM)
    size_t     len;       // tamanho em bytes
    size_t     width;     // 800 (SVGA)
    size_t     height;    // 600 (SVGA)
    pixformat_t format;   // PIXFORMAT_JPEG
    int64_t    timestamp; // quando foi capturada
} camera_fb_t;
```

O `camera.h` do seu projeto expõe esse tipo como `camera_frame_t` (typedef), mas sem obrigar quem usa `camera.h` a incluir `esp_camera.h` — exceto que, no final, precisou incluir por causa do acesso a `buf` e `len`. Isso foi um trade-off consciente: opacidade total exigiria funções getter; no final, `esp_camera.h` acaba vazando. É aceitável.

---

## 3. Wi-Fi — do rádio ao IP

O módulo `wifi.c` encapsula o driver, `esp_netif`, `esp_event` e adiciona reconexão com backoff. Cinco conceitos aparecem.

### `esp_netif` — a "interface de rede"

`esp_netif` é a **abstração de rede** do ESP-IDF. Cada interface (station, softAP, ethernet) tem seu `esp_netif_t`.

Quando você chama:

```c
s_netif = esp_netif_create_default_wifi_sta();
```

Ele:

- Registra a interface com o LWIP (stack TCP/IP)
- Configura DHCP client
- Emite eventos de IP (`IP_EVENT_STA_GOT_IP`)
- Gerencia `esp_netif_ip_info_t` (IP, máscara, gateway)

Sem `esp_netif`, você teria que falar com o LWIP diretamente. É a camada certa de abstração.

### `esp_event` — o event loop

O event loop default é **uma task do IDF** que recebe eventos de múltiplos drivers (Wi-Fi, IP, MQTT) e chama os handlers registrados.

Sua `wifi.c` registra:

```c
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                    &wifi_event_handler, NULL, &s_wifi_handler);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                    &wifi_event_handler, NULL, &s_ip_handler);
```

O handler recebe **quatro** eventos:

| Evento | Quando | O que o handler faz |
|--------|--------|---------------------|
| `WIFI_EVENT_STA_START` | Driver iniciado | Chama `esp_wifi_connect()` |
| `WIFI_EVENT_STA_CONNECTED` | Associado ao AP | `WIFI_CONNECTED_BIT`, `esp_wifi_set_ps(WIFI_PS_NONE)` |
| `WIFI_EVENT_STA_DISCONNECTED` | Desconectado | Se reason ≠ `ASSOC_LEAVE`, agenda reconexão |
| `IP_EVENT_STA_GOT_IP` | DHCP concluído | `WIFI_GOT_IP_BIT`, cancela timer de reconexão |

**`WIFI_EVENT_STA_CONNECTED` vs `IP_EVENT_STA_GOT_IP`:** o primeiro é associação (camada 2), o segundo é IP atribuído (camada 3). Sua `wifi_is_connected()` só retorna `true` quando o **segundo** acontece — porque antes do IP, não há como fazer socket.

### Backoff exponencial — por que 1→2→4→8→16→30 s

O handler de `STA_DISCONNECTED` **não** chama `esp_wifi_connect()` diretamente. Ele agenda um **timer** com delay calculado:

```c
static void schedule_reconnect(void)
{
    s_retry_count++;
    uint32_t delay = WIFI_BACKOFF_BASE_MS;   // 1000
    for (uint32_t i = 1; i < s_retry_count && delay < WIFI_BACKOFF_MAX_MS; i++) {
        delay *= 2;
    }
    if (delay > WIFI_BACKOFF_MAX_MS) delay = WIFI_BACKOFF_MAX_MS;

    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000ULL);
}
```

**Por que backoff e não retry imediato:**

| Tentativa | Delay | Cenário |
|-----------|-------|---------|
| 1 | 1 s | Interferência momentânea — resolve rápido |
| 2 | 2 s | AP reiniciando |
| 3 | 4 s | |
| 4 | 8 s | |
| 5 | 16 s | |
| 6+ | 30 s | AP fora do ar — não sobrecarrega |

Sem backoff, um roteador desligado receberia 1000 tentativas por segundo. Com backoff, uma tentativa a cada 30 s — o roteador tem tempo para reiniciar, e o ESP32 economiza energia.

**Reset do contador:** em `IP_EVENT_STA_GOT_IP`, `s_retry_count = 0`. Assim, a próxima queda recomeça em 1 s.

### `WIFI_REASON_ASSOC_LEAVE` — a exceção

```c
if (evt->reason == WIFI_REASON_ASSOC_LEAVE) {
    ESP_LOGI(TAG, "Desconexão intencional — não reconectando");
    break;
}
```

`reason = 8` significa "desconexão por chamada de `esp_wifi_disconnect()`". Se o firmware chamou a desconexão (para teste), não queremos reconectar automaticamente — senão o teste de "forçar queda" ficaria num loop.

### `WIFI_PS_NONE` — a correção que fez o OTA funcionar

Você viu nos logs:

```
I (3782) wifi:pm start, type: 1
```

`type: 1` = `WIFI_PS_MIN_MODEM`. A placa dorme entre beacons (~100 ms cada), acordando apenas para receber. Resultado:

- **Consumo:** ~30–60 mA
- **Latência:** ping de **250 ms**
- **Throughput:** ~60 KB/s no OTA

Com `WIFI_PS_NONE`:

```c
case WIFI_EVENT_STA_CONNECTED:
    ESP_LOGI(TAG, "Associado ao AP");
    esp_wifi_set_ps(WIFI_PS_NONE);   // ← adicionado
    xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    break;
```

- **Consumo:** ~100–130 mA
- **Latência:** ping de **5 ms**
- **Throughput:** ~110 KB/s (OTA de 1 MB em ~10 s)

**Trade-off:** dobra o consumo, mas reduz a latência em 50×. Para dispositivo plugado (seu caso), é escolha óbvia. Para bateria, seria problemático.

**Por que é importante colocar em `WIFI_EVENT_STA_CONNECTED` e não em `wifi_init`:** porque `esp_wifi_set_ps()` só tem efeito **após** o Wi-Fi estar associado. Chamar antes retorna erro silencioso.

---

## 4. MQTT — publish de chunks, subscribe de resultado

### `esp_mqtt_client` — visão geral

O componente `mqtt` do IDF gerencia:

- Conexão TCP (ou TLS)
- Handshake MQTT (CONNECT/CONNACK)
- Keepalive (PINGREQ a cada 60 s)
- Reconexão automática
- Fila de saída ("outbox")
- Dispatch de mensagens recebidas

Sua `mqtt.c` **encapsula** o driver: o resto do firmware nunca chama `esp_mqtt_client_*`.

### Configuração

```c
esp_mqtt_client_config_t cfg = {
    .broker.address.uri                  = CONFIG_MQTT_BROKER_URI,
    .credentials.client_id               = CONFIG_MQTT_CLIENT_ID,
    .credentials.username                = CONFIG_MQTT_USERNAME,
    .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    .session.keepalive                   = CONFIG_MQTT_KEEPALIVE_S,      // 60
    .session.disable_clean_session       = false,
    .network.reconnect_timeout_ms        = CONFIG_MQTT_RECONNECT_MS,     // 5000
    .network.timeout_ms                  = CONFIG_MQTT_CONNECT_TIMEOUT_MS, // 15000
    .network.disable_auto_reconnect      = false,
};
```

**`client_id`:** string única que identifica o cliente no broker. Se duas placas usarem o mesmo `client_id`, o broker **desconecta a primeira** quando a segunda se conecta (comportamento do protocolo). Por isso é `CONFIG_LOCKMAXXING_MQTT_CLIENT_ID` — cada instalação deve ter o seu.

**`keepalive = 60`:** o cliente envia PINGREQ a cada 60 s. Se o broker não responder em 90 s (1.5× o keepalive), o cliente considera a conexão morta e reconecta.

**`disable_clean_session = false`:** cada reconexão **zera** a sessão do broker. Vantagem: sem mensagens acumuladas antigas. Desvantagem: precisa re-inscrever em `/result` após cada reconexão — o que o `on_connect` handler faz.

### Tópicos

Três tópicos definidos em `config.h`:

```c
#define CONFIG_MQTT_TOPIC_BASE      "lockmaxxing/camera"
#define CONFIG_MQTT_TOPIC_IMAGE     CONFIG_MQTT_TOPIC_BASE "/image"
#define CONFIG_MQTT_TOPIC_RESULT    CONFIG_MQTT_TOPIC_BASE "/result"
#define CONFIG_MQTT_TOPIC_REQUEST   CONFIG_MQTT_TOPIC_BASE "/request"
```

| Tópico | Direção | Payload |
|--------|---------|---------|
| `/request` | ESP32 → broker | JSON com metadados (id, size, chunks) |
| `/image` | ESP32 → broker | Binário (header 8 B + JPEG) |
| `/result` | broker → ESP32 | JSON com resultado (id, resultado) |

**Por que tópicos separados:** o `/result` é **subscribed** pelo ESP32 e **published** pelo Python. Os outros dois são o oposto. Separar permite permissões granulares no broker (ex: ESP32 pode publicar em `/image` mas não em `/result`).

### `mqtt_publish_chunk` — o caminho quente

```c
esp_err_t mqtt_publish_chunk(uint32_t image_id,
                             uint16_t chunk_index,
                             uint16_t total_chunks,
                             const uint8_t *data, size_t len)
{
    if (len == 0 || len > CONFIG_MQTT_CHUNK_SIZE) return ESP_ERR_INVALID_ARG;
    if (chunk_index >= total_chunks)               return ESP_ERR_INVALID_ARG;

    size_t buf_len = CONFIG_MQTT_CHUNK_HEADER_SIZE + len;

    /* Buffer estático protegido por mutex */
    xSemaphoreTake(s_chunk_mutex, portMAX_DELAY);
    put_u32_be(s_chunk_buf + 0, image_id);
    put_u16_be(s_chunk_buf + 4, chunk_index);
    put_u16_be(s_chunk_buf + 6, total_chunks);
    memcpy(s_chunk_buf + 8, data, len);

    int msg_id = esp_mqtt_client_publish(s_client,
                                         CONFIG_MQTT_TOPIC_IMAGE,
                                         (const char *)s_chunk_buf,
                                         (int)buf_len,
                                         CONFIG_MQTT_QOS, 0);
    xSemaphoreGive(s_chunk_mutex);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}
```

**Cuidados:**

1. **Validação antes do lock.** Se `len == 0`, retorna sem pegar o mutex — evita pegar/release desnecessário.
2. **Buffer estático** (`s_chunk_buf`) protegido por mutex. Não precisa alocar 2 KB por chamada.
3. **`esp_mqtt_client_publish` copia** o payload para o outbox interno antes de retornar. O `s_chunk_buf` pode ser reusado logo depois do `xSemaphoreGive`.
4. **Retorno `msg_id < 0`** = fila cheia ou desconexão. O `comm_task` trata como `SEND_FAILED`.

### QoS 1 — at-least-once

```c
#define CONFIG_MQTT_QOS 1
```

| QoS | Garantia | Custo | Duplicatas |
|-----|----------|-------|-----------|
| 0 | "Fire and forget" | 0 round-trips | Nenhuma garantia |
| **1** | "At least once" | 1 PUBACK por mensagem | Possível |
| 2 | "Exactly once" | 2 round-trips (PUBREC, PUBREL) | Nunca |

**Escolha:** QoS 1 porque:

- QoS 0 perderia chunk em Wi-Fi instável → imagem incompleta → `RESULT_TIMEOUT` no servidor
- QoS 2 dobra o round-trip → 24 chunks × 2 = 48 round-trips por imagem

**Duplicatas no QoS 1:** o broker pode reenviar um chunk se o PUBACK se perder. O **servidor Python** trata isso: `dict[chunk_idx] = data` — última ocorrência sobrescreve. Duplicata é idempotente.

### Parse de resultado com cJSON

O callback MQTT:

```c
case MQTT_EVENT_DATA:
    if (strncmp(event->topic, CONFIG_MQTT_TOPIC_RESULT, event->topic_len) == 0) {
        handle_result_message(event->data, event->data_len);
    }
    break;
```

`handle_result_message` usa cJSON:

```c
cJSON *root = cJSON_ParseWithLength(data, len);
if (!cJSON_IsObject(root)) goto drop;

cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
uint32_t image_id;
bool id_ok = cJSON_IsNumber(id_item) && id_item->valuedouble >= 0
             && id_item->valuedouble <= UINT32_MAX;

cJSON *res_item = cJSON_GetObjectItemCaseSensitive(root, "resultado");
if (!cJSON_IsString(res_item)) goto drop;

mqtt_result_t result;
if (strcasecmp(res_item->valuestring, "aprovado") == 0) result = MQTT_RESULT_APPROVED;
else if (strcasecmp(res_item->valuestring, "reprovado") == 0) result = MQTT_RESULT_REJECTED;
else goto drop;

if (s_result_cb) s_result_cb(image_id, result);
cJSON_Delete(root);
return;

drop:
    cJSON_Delete(root);
    s_dropped++;
```

**`cJSON_ParseWithLength`** — não `cJSON_Parse`. O `event->data` do `esp-mqtt` **não é null-terminated**; `cJSON_Parse` leria até encontrar um `\0` aleatório. `ParseWithLength` respeita o `data_len`.

**`strcasecmp`** — aceita "APROVADO", "aprovado", "Aprovado". O servidor Python envia "aprovado"; se um dia enviar em maiúsculas, funciona.

**Callback na task do esp-mqtt:** o `s_result_cb` roda na task interna do MQTT, não na `comm_task`. Por isso ele **só enfileira** (`xQueueSend` com timeout 0) e retorna. Se chamasse `actuators_apply_result()` direto, travaria a recepção de PUBACK/PINGRESP e o broker desconectaria por keepalive.

### Por que cJSON e não parser manual

Você teve a experiência:

- **Parser manual** (primeira versão): ~120 linhas, 2 bugs descobertos nos host tests (loop infinito em `}`, aceitação de JSON truncado).
- **cJSON** (versão atual): ~40 linhas no `handle_result_message`, 19 testes passam, zero bugs.

cJSON aloca ~200–500 B por parse (árvore temporária), liberada com `cJSON_Delete`. **Transacional, curta duração, sem fragmentação.** É o caso em que `malloc` é aceitável.

---

## 5. Protocolo de fragmentação de imagem

### O problema

Uma imagem SVGA JPEG tem ~40 KB. O MQTT tem buffer padrão de 1 KB (você configurou 4096). **Não cabe.** Você precisa fragmentar.

### O formato binário

Cada chunk publicado em `/image`:

```
[0..3]   image_id       uint32 big-endian
[4..5]   chunk_index    uint16 big-endian
[6..7]   total_chunks   uint16 big-endian
[8..N]   payload        bytes do JPEG
```

**Total:** `8 + CONFIG_MQTT_CHUNK_SIZE` = **2056 bytes**.

### Por que big-endian

Big-endian é a **ordem de bytes de rede** (network byte order). Se o servidor Python interpretar os bytes com `struct.unpack(">IHH", ...)` (`>` = big-endian), funciona em qualquer arquitetura (x86, ARM, ESP32 — todos little-endian, mas Python aplica a conversão).

**Se fosse little-endian** (`<IHH` no Python), funcionaria também, mas quebraria convenções. Big-endian é o padrão da internet.

**Funções usadas:**

```c
static inline void put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

static inline void put_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] =  v       & 0xFF;
}
```

Manual — evita dependência de `<arpa/inet.h>` (`htonl`, `htons`) e é mais rápido (inlined).

### Por que `image_id` (32 bits)

`image_id` é um contador monotônico (`s_next_image_id++`). Começa em 1, incrementa a cada captura. 4 bilhões de imagens antes de overflow (~5 mil anos a 1 imagem a cada 30 s).

**Função do ID:** correlacionar resultado com imagem. O servidor recebe `{"id": 42, "resultado": "aprovado"}` — o firmware verifica se `42 == pending_image_id`. Se não, é resposta atrasada ou cruzada → `RESULT_INVALID` → `ERROR`.

### Por que `chunk_index` e `total_chunks` (16 bits cada)

`chunk_index`: qual chunk é este (0 a N-1). `total_chunks`: quantos chunks no total.

**16 bits = até 65535 chunks** = 128 MB de imagem. Muito mais do que o limite de 200 KB do projeto.

**Uso pelo servidor:** reconstruir na ordem correta (`sorted(chunks.items())`) e saber quando terminou (`len(chunks) == total_chunks`).

### Fluxo no firmware

```
1. do_send_chunked():
   - calcula total_chunks = ceil(frame->len / 2048)
   - publica /request com {id, size, chunks}
   - para i em 0..total_chunks-1:
     - publica /image com header + 2048 bytes
2. servidor Python:
   - recebe /request, aloca buffer
   - recebe cada /image, armazena em dict[idx] = data
   - quando len(dict) == chunks, concatena e valida JPEG (FF D8 ... FF D9)
   - envia resultado em /result
```

---

## 6. Atuadores — LEDs, servo e buzzer

### LEDs e buzzer — GPIO simples

```c
gpio_config_t io_cfg = {
    .pin_bit_mask = (1ULL << CONFIG_GPIO_LED_GREEN) |
                    (1ULL << CONFIG_GPIO_LED_RED)   |
                    (1ULL << CONFIG_GPIO_BUZZER),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
gpio_config(&io_cfg);

gpio_set_level(GPIO_NUM_1, 1);  // LED verde on
gpio_set_level(GPIO_NUM_1, 0);  // LED verde off
```

**Nível ativo configurável:**

```c
#define CONFIG_LEVEL_LED_ON      1
#define CONFIG_LEVEL_BUZZER_ON   1
```

Se um dia você inverter a polaridade (transistor PNP, por exemplo), muda para `0` — o código não muda.

### Servo SG90 — PWM via LEDC

O SG90 é controlado por **PWM de 50 Hz** (período de 20 ms):

- **500 µs** = 0°
- **1500 µs** = 90° (centro)
- **2500 µs** = 180°

O **LEDC** (LED Control) do ESP32 gera PWM por hardware — sem CPU.

### Configuração

```c
ledc_timer_config_t timer_cfg = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .duty_resolution = CONFIG_SERVO_TIMER_RES,   // 13 bits (0–8191)
    .timer_num       = CONFIG_SERVO_TIMER,       // 1 (câmera usa 0)
    .freq_hz         = CONFIG_SERVO_FREQ_HZ,     // 50 Hz
    .clk_cfg         = LEDC_AUTO_CLK,
};
ledc_timer_config(&timer_cfg);

ledc_channel_config_t ch_cfg = {
    .gpio_num   = CONFIG_GPIO_SERVO,             // 21
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = CONFIG_SERVO_CHANNEL,          // 1 (câmera usa 0)
    .timer_sel  = CONFIG_SERVO_TIMER,            // 1
    .duty       = angle_to_duty(0),              // 0° inicial
    .hpoint     = 0,
    .intr_type  = LEDC_INTR_DISABLE,
};
ledc_channel_config(&ch_cfg);
```

### Cálculo duty ↔ ângulo

Resolução de 13 bits = **8192 níveis** (0 a 8191).

```
duty = (pulse_us / period_us) * 8191

500 µs  → (500/20000) * 8191 ≈ 205     // 0°
1500 µs → (1500/20000) * 8191 ≈ 614    // 90°
2500 µs → (2500/20000) * 8191 ≈ 1023   // 180°
```

Seu `config.h` (após a correção que fizemos para o servo voltar ao HOME):

```c
#define CONFIG_SERVO_DUTY_MIN      205     // 0°
#define CONFIG_SERVO_DUTY_MAX      983     // 180° (um pouco menos de 2500 µs)
#define CONFIG_SERVO_DUTY_HOME     CONFIG_SERVO_DUTY_MIN
#define CONFIG_SERVO_DUTY_APPROVED CONFIG_SERVO_DUTY_MAX
```

**Por que `DUTY_MAX = 983` e não 1023:** SG90 barato tem folga mecânica; forçar 1023 pode fazer o servo "bater no fim de curso" e travar. 983 (~2400 µs) fica dentro da faixa segura.

### Conversão linear

```c
static uint32_t angle_to_duty(uint8_t degrees)
{
    if (degrees > 180) degrees = 180;
    uint32_t range = CONFIG_SERVO_DUTY_MAX - CONFIG_SERVO_DUTY_MIN;
    return CONFIG_SERVO_DUTY_MIN + ((uint32_t)degrees * range / 180U);
}
```

- `degrees = 0`   → `205 + 0` = **205**
- `degrees = 90`  → `205 + 90*778/180` = `205 + 389` = **594**
- `degrees = 180` → `205 + 180*778/180` = `205 + 778` = **983**

**Cálculo em `uint32_t`** evita overflow em resoluções maiores (14 bits = 16383).

### `ledc_set_duty` + `ledc_update_duty`

```c
static esp_err_t servo_set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_SERVO_CHANNEL);
    return ESP_OK;
}
```

**Por que duas chamadas:** `ledc_set_duty` grava o valor no registrador de "próximo duty" (latched). `ledc_update_duty` aplica o valor latched no duty ativo. **Sem a segunda, o servo não se move.** É o erro #1 de quem usa LEDC para servo pela primeira vez.

### Sequência APROVADO

```c
case ACTUATOR_RESULT_APPROVED:
    actuators_set_led_red(false);
    actuators_set_led_green(true);
    servo_set_duty(CONFIG_SERVO_DUTY_APPROVED);   // 180°
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));  // 800 ms
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_HOLD_MS));    // 1500 ms
    servo_set_duty(CONFIG_SERVO_DUTY_HOME);       // 0°
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));  // 800 ms
    actuators_set_led_green(false);
    return ESP_OK;
```

**Tempo total:** 800 + 1500 + 800 = **3.1 s**.

**Por que 800 ms:** SG90 sem carga faz 180° em ~400 ms, mas com carga leve pode levar o dobro. 800 ms é folgado e garante que o servo chegue ao fim antes da próxima instrução.

**Por que 1500 ms de hold:** é o tempo em que o resultado "aprovação" é visível para o usuário (LED verde + servo no fim de curso). 1.5 s é curto o bastante para não travar o ciclo, longo o bastante para ser percebido.

### Sequência REPROVADO

```c
case ACTUATOR_RESULT_REJECTED:
    actuators_set_led_green(false);
    actuators_set_led_red(true);
    actuators_set_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BUZZER_DURATION_MS));  // 3000 ms
    actuators_set_buzzer(false);
    actuators_set_led_red(false);
    return ESP_OK;
```

**Buzzer ativo vs passivo:** você usa **buzzer ativo** (tem oscilador interno). Ligar o GPIO = som. Se fosse passivo, precisaria de PWM com frequência específica (~2 kHz) — e aí o `ledc` do buzzer precisaria de outro canal. Seu buzzer é ativo, GPIO puro resolve.

### Alimentação externa do servo

O regulador AMS1117 da N16R8 fornece ~500 mA. O servo SG90 puxa **700 mA de pico** durante movimento. Se alimentar o servo pela placa:

- O regulador entra em shutdown térmico
- A placa reinicia (brownout)
- O Wi-Fi cai no meio da transmissão

**Ligação correta:**

```
Fonte externa 5V/2A ──► V+ do servo
                    └──► GND do servo ──► GND da ESP32-CAM (obrigatório!)
```

O GND comum é **obrigatório** — sem ele, o sinal PWM do GPIO 21 não tem referência e o servo não responde.

**No seu caso específico:** você não tem servo conectado ainda, mas quando conectar, a fonte externa é obrigatória. O `NO-SOI` que apareceu no log em alguns testes é justamente sintoma de **ruído na alimentação da câmera** — quando o servo for conectado, isso pode ficar pior se a fonte for fraca.

---

## 7. O que levar desta Parte 4

1. **GPIOs são restritos por hardware, não por escolha.** 26–32 (flash), 33–37 (PSRAM), 19–20 (USB), 0/45/46 (strapping) — se usar, firmware corrompe. Os pinos escolhidos (`1, 2, 3, 21`) são os mais seguros.

2. **`LEDC_TIMER_0` é da câmera.** Servo usa `TIMER_1`/`CHANNEL_1`. Esse conflito foi um bug real do projeto.

3. **OV5640 tem engine JPEG.** Sem isso, comprimir em software seria 10× mais lento. `PIXFORMAT_JPEG` explora o hardware.

4. **`set_vflip(1)` é hardware, não software.** O sensor inverte antes de entregar. Custo zero de CPU.

5. **Wi-Fi default é power-save.** `WIFI_PS_NONE` foi a correção que fez o OTA passar de 66 s para 10 s. Uma linha.

6. **Backoff exponencial salva o roteador.** 1→2→4→8→16→30 s. Reset no `GOT_IP`.

7. **MQTT QoS 1 = 1 PUBACK por chunk.** Aceitável para 24 chunks. QoS 0 perderia chunks; QoS 2 dobraria o round-trip.

8. **Protocolo de imagem é binário big-endian** (network order). 8 B de header + payload. Detecta duplicata, ordem, correlação por `image_id`.

9. **LEDC precisa de duas chamadas** (`set_duty` + `update_duty`) para o servo se mover. É o erro clássico.

10. **Servo NUNCA pela placa.** Fonte externa 5V/2A, GND comum. Picos de 700 mA.
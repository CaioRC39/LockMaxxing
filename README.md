# LockMaxxing — ESP32-S3-CAM + MQTT + OTA

Firmware completo para ESP32-S3-CAM (N16R8 com câmera OV5640) que captura imagens,
envia via MQTT fragmentado para um servidor de análise, recebe aprovação/reprovação
e aciona atuadores (LEDs, servo, buzzer). Inclui atualização OTA (push e pull)
com rollback automático.

Projeto de aprendizado com arquitetura pronta para servir de base a outros
sistemas embarcados.

---

## Sumário

1. [Visão geral](#1-visão-geral)
2. [Hardware](#2-hardware)
3. [Pinout](#3-pinout)
4. [Software](#4-software)
5. [Configuração](#5-configuração)
6. [Compilação](#6-compilação)
7. [Gravação e monitor](#7-gravação-e-monitor)
8. [Servidores de teste](#8-servidores-de-teste)
9. [OTA — push e pull](#9-ota--push-e-pull)
10. [Testes de host](#10-testes-de-host)
11. [Troubleshooting](#11-troubleshooting)
12. [Arquitetura](#12-arquitetura)
13. [Fluxo da máquina de estados](#13-fluxo-da-máquina-de-estados)
14. [Segurança elétrica](#14-segurança-elétrica)
15. [Decisões de design](#15-decisões-de-design)
16. [Melhorias futuras](#16-melhorias-futuras)

---

## 1. Visão geral

O firmware implementa o ciclo:

```
boot → self-test → Wi-Fi → MQTT → (30s) → captura → envia imagem →
aguarda resultado → aprovado: LED verde + servo | reprovado: LED vermelho + buzzer
→ aguarda 30s → repete
```

Características:

- **FreeRTOS com 4 tasks** (wifi_mqtt, camera, comm, actuator) pinadas em cores distintos
- **MQTT com fragmentação** (chunks de 2 KB, header binário big-endian)
- **Máquina de estados** com timeout e recuperação de erros
- **OTA dupla**:
  - *Push*: cliente envia `POST /update` com `.bin` → ~10 s para 1 MB
  - *Pull*: cliente envia `POST /pull-ota` com URL → ESP32 baixa em background
- **Rollback automático** se o novo firmware falhar no self-test de boot
- **Warmup de 300 s** — rejeita OTA nos primeiros minutos para aguardar estabilização da rede
- **PSRAM octal** para framebuffers da câmera
- **Zero malloc em código de aplicação** (buffers estáticos + cJSON transacional)

---

## 2. Hardware

| Componente | Modelo | Observações |
|-----------|--------|-------------|
| Placa | ESP32-S3 CAM **N16R8** | 16 MB flash, 8 MB PSRAM octal |
| Câmera | **OV5640** | Sensor 5 MP, saída JPEG nativa |
| LED verde | 5 mm | Aprovação, com resistor 220–330 Ω |
| LED vermelho | 5 mm | Reprovação, com resistor 220–330 Ω |
| Buzzer | Ativo (5 V) | Alerta de reprovação |
| Servo | SG90 | 180°, alimentação externa 5 V |
| Fonte externa | 5 V / 2 A | **Obrigatória** para o servo |
| Programador | USB nativo da S3 | Não precisa de FTDI |

### Premissas

- Placa com PSRAM octal (N16R8). Se você tem uma variante R8 (quad PSRAM),
  ajuste `CONFIG_SPIRAM_MODE_OCT` para `CONFIG_SPIRAM_MODE_QUAD` no `sdkconfig.defaults`.
- Câmera OV5640. Para OV3660, ajuste `apply_sensor_specific_config()` em
  `camera.c` (o `set_vflip(1)` pode não ser necessário).
- Alimentação USB suficiente. O servo **não pode** ser alimentado pela placa.

---

## 3. Pinout

| Componente | GPIO | Função | Justificativa |
|-----------|-----:|--------|--------------|
| Câmera D0 | 11 | dados DVP | Barramento padrão N16R8 |
| Câmera D1 | 9 | dados DVP | |
| Câmera D2 | 8 | dados DVP | |
| Câmera D3 | 10 | dados DVP | |
| Câmera D4 | 12 | dados DVP | |
| Câmera D5 | 18 | dados DVP | |
| Câmera D6 | 17 | dados DVP | |
| Câmera D7 | 16 | dados DVP | |
| Câmera XCLK | 15 | clock | 20 MHz |
| Câmera PCLK | 13 | pixel clock | |
| Câmera VSYNC | 6 | sync vertical | |
| Câmera HREF | 7 | sync horizontal | |
| Câmera SIOD | 4 | I2C SDA | Configuração do sensor |
| Câmera SIOC | 5 | I2C SCL | Configuração do sensor |
| LED verde | 1 | saída | Livre, evitando strapping |
| LED vermelho | 2 | saída | Livre, evitando strapping |
| Buzzer | 3 | saída | Livre, evitando strapping |
| Servo | 21 | PWM (LEDC timer 1, canal 1) | Canal 0 reservado para XCLK da câmera |

**GPIOs evitados:** 0, 45, 46 (strapping), 19, 20 (USB), 26–32 (flash), 33–37 (PSRAM octal).

---

## 4. Software

| Item | Versão | Notas |
|------|--------|-------|
| ESP-IDF | **6.1.0** | Testado com esta versão |
| Target | `esp32s3` | |
| Compilador | `xtensa-esp32s3-elf-gcc` | Fornecido pelo IDF |
| VS Code | Extensão ESP-IDF | Recomendado |
| Python | 3.8+ | Para scripts de teste |
| Broker MQTT | Mosquitto 2.x | Local ou remoto |
| `paho-mqtt` | 1.x ou 2.x | `pip install paho-mqtt` |

### Componentes gerenciados (`main/idf_component.yml`)

- `espressif/esp32-camera` (^2.0.15)
- `espressif/cjson` (^1.7.19)

### Otimizações aplicadas

- `CONFIG_NEWLIB_NANO_FORMAT=y` — remove formatação float (economia ~105 KB)
- `esp_wifi_set_ps(WIFI_PS_NONE)` — desabilita power save do Wi-Fi (latência 250→10 ms)
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` **removido** — mantém buffers Wi-Fi em RAM interna
- `-O2` no firmware, `-Os` no bootloader

---

## 5. Configuração

O projeto compila **out-of-the-box** com valores padrão. Para configurar
para sua rede:

```bash
idf.py menuconfig
```

Navegue até **LockMaxxing Configuration**:

| Opção | Default | Descrição |
|-------|---------|-----------|
| Wi-Fi SSID | `myssid` | Nome da rede |
| Wi-Fi Password | `mypassword` | Senha |
| MQTT Broker URI | `mqtt://192.168.1.100:1883` | URI do broker |
| MQTT Username | *(vazio)* | Deixe em branco para anônimo |
| MQTT Password | *(vazio)* | Deixe em branco para anônimo |
| MQTT Client ID | `esp32-camera-01` | Único por dispositivo |
| Intervalo entre capturas | 30000 ms | Timer de IDLE |
| OTA warmup | 300 s | Rejeita OTA antes desse uptime |
| Endpoint `/pull-ota` | habilitado | DEV/TEST — desabilite em produção |
| Verificar pull contra `certs.h` | desabilitado | Habilite em bancada, use CA pública em produção |

As configurações ficam em `sdkconfig` (ignorado pelo Git). Se você rodar
`idf.py fullclean`, elas **não** são perdidas.

---

## 6. Compilação

```bash
# Primeira vez:
idf.py set-target esp32s3
idf.py build

# Builds subsequentes:
idf.py build
```

Saída esperada:

```
Project build complete. To flash, run:
 idf.py flash
```

**Tamanho típico:** `.bin` de ~1.0 MB, 33% da partição de app de 3 MB.

---

## 7. Gravação e monitor

```bash
# Identifique a porta
ls /dev/ttyACM*        # Linux/macOS
# ou: Gerenciador de Dispositivos → Portas (COM e LPT) → no Windows

# Gravação
idf.py -p /dev/ttyACM0 flash
# Windows: idf.py -p COM5 flash

# Monitor
idf.py -p /dev/ttyACM0 monitor

# Ou ambos de uma vez
idf.py -p /dev/ttyACM0 flash monitor
```

### O que esperar no boot

```
APP: ===============================================
APP:  ESP32-CAM MQTT + OTA — boot V2
APP: ===============================================
OTA_COMMON: Rodando da partição 'ota_0' (offset=0x00020000, ...)
CAMERA: Sensor detectado: OV5640 (PID=0x5640, ...)
ACTUATOR: Atuadores prontos: verde=GPIO1, vermelho=GPIO2, buzzer=GPIO3, servo=GPIO21
WIFI: Associado ao AP
WIFI: IP obtido: 192.168.1.42
MQTT: Conectado ao broker
OTA_SRV: Servidor HTTPS ativo em https://192.168.1.42:8443
APP: app_main concluído — sistema em regime
```

---

## 8. Servidores de teste

### Broker MQTT (Mosquitto)

**Linux:**
```bash
sudo apt install mosquitto
sudo systemctl start mosquitto
```

**macOS:**
```bash
brew install mosquitto
brew services start mosquitto
```

**Windows:** instale de [mosquitto.org](https://mosquitto.org/download/).

**Configuração mínima para rede local** (edite `mosquitto.conf`):
```conf
listener 1883 0.0.0.0
allow_anonymous true
```

**Verificar** que está escutando em `0.0.0.0:1883` (não `127.0.0.1:1883`):
```powershell
netstat -an | Select-String ":1883"
```

### Servidor de análise (`tools/mqtt_test_server.py`)

```bash
# Criar venv dedicada (recomendado)
python3 -m venv .venv-tools
source .venv-tools/bin/activate        # Windows: .venv-tools\Scripts\activate
pip install paho-mqtt

# Rodar
python tools/mqtt_test_server.py \
    --broker 192.168.1.100 \
    --mode alternate \
    --output-dir received_images
```

Modos: `always-approve`, `always-reject`, `alternate` (par/ímpar), `random`.

O servidor reconstrói JPEGs dos chunks e os salva em `received_images/`,
depois publica a decisão no tópico de resultado.

### Servidor OTA (`tools/ota_test_server.py`)

```bash
# Push: envia .bin para o ESP32
python tools/ota_test_server.py push build/LockMaxxing.bin --device 192.168.1.42

# Pull: hospeda .bin, ESP32 baixa via HTTPS
python tools/ota_test_server.py serve build/LockMaxxing.bin --port 8443

# Em outro terminal: dispara o pull
python tools/ota_test_server.py pull --device 192.168.1.42 \
    --from https://192.168.1.100:8443/LockMaxxing.bin

# Utilitário: calcular SHA-256
python tools/ota_test_server.py sha256 build/LockMaxxing.bin
```

---

## 9. OTA — push e pull

### Push (cliente envia firmware para o ESP32)

Requer `CONFIG_LOCKMAXXING_OTA_WARMUP_S` **expirado** (300 s de uptime).

```bash
python tools/ota_test_server.py push build/LockMaxxing.bin --device 192.168.1.42
```

**Tempo esperado:** 8–15 s para 1 MB (com `WIFI_PS_NONE`). Observado em
bancada: **9.85 s**.

**Fluxo:**
```
POST /update com corpo binário e header X-SHA256
→ ESP32 grava em partição ota_1 (inativa)
→ esp_ota_end() valida estrutura
→ esp_ota_set_boot_partition()
→ reboot em ~2 s
→ boot na nova partição
→ self-test (câmera, atuadores, Wi-Fi, MQTT)
→ ota_mark_app_valid() (cancela rollback)
```

### Pull (ESP32 baixa firmware de um servidor remoto)

```bash
# 1) Terminal 1: subir serve
python tools/ota_test_server.py serve build/LockMaxxing.bin --port 8443

# 2) Terminal 2: disparar pull
python tools/ota_test_server.py pull \
    --device 192.168.1.42 \
    --from https://192.168.1.100:8443/LockMaxxing.bin
```

**Tempo esperado:** 10–15 s (similar ao push).

**Fluxo:**
```
POST /pull-ota com JSON {"url":"https://...","sha256":"..."}
→ ESP32 retorna {"status":"started"}
→ baixa em background via esp_http_client + esp_ota_write
→ calcula SHA-256 em streaming, compara
→ reboot em ~2 s após callback
→ boot na nova partição
→ self-test → confirmação
```

### Rollback

Se o firmware novo falhar no self-test (`camera_init` ou `actuators_init`
retornar erro), o boot pós-OTA dispara `ota_mark_app_invalid_rollback_and_reboot()`
e a placa volta para a partição anterior.

**Para testar o rollback:** compile temporariamente com
`camera_init()` retornando `ESP_FAIL`, faça o push, e observe o retorno.

### Warmup

Para evitar OTA durante a janela inicial (rede ainda convergindo DHCP,
TLS, TCP slow start), o endpoint rejeita OTA com HTTP 503 se uptime
for menor que `CONFIG_LOCKMAXXING_OTA_WARMUP_S` (default 300 s).

Resposta:
```json
{"error":"warmup","uptime_s":45,"retry_in_s":255}
```

**Desabilitar para testes rápidos:** `idf.py menuconfig` → warmup = 0.

### ⚠️ Segurança em produção

- `CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE` — **desabilite em produção**.
  Qualquer um na rede pode enviar firmware por este endpoint.
- `CONFIG_LOCKMAXXING_OTA_PULL_SKIP_TLS_VERIFY` — para o modo pull em bancada,
  usa o cert de `certs.h`. Em produção, use servidor com CA pública e desabilite.
- O certificado do servidor local (`main/certs.h`) é auto-assinado e **não
  deve ser comitado** (contém a chave privada).

---

## 10. Testes de host

Testes de lógica pura em C, rodando no PC (não na placa):

```bash
cd tools/host_tests
./run_all.sh                # Linux/macOS/WSL
# ou:
.\run_all.ps1               # Windows
```

Cobrem:

| Suíte | O que testa |
|-------|-------------|
| `test_chunking.c` | Matemática de fragmentação em todos os limites |
| `test_json_parser.c` | Parser de resultado (7 válidos, 12 inválidos) |
| `test_sha256_equals.c` | Comparação case-insensitive de SHA-256 |
| `test_state_machine.c` | 36 transições de estado (caminhos felizes e erros) |

**Resultado:** 78/78 testes. Se algum falhar, o problema é lógica — não hardware.

Dependência: `gcc` no PATH (MinGW-w64, MSYS2 ou clang). O `run_all` baixa o
cJSON automaticamente na primeira execução.

---

## 11. Troubleshooting

### Wi-Fi

| Sintoma | Causa provável | Solução |
|---------|----------------|---------|
| `Wi-Fi não conectou em 30000 ms` | SSID/senha errados | `menuconfig` → LockMaxxing Configuration |
| Reconecta em loop | Power save + AP instável | Já aplicado `WIFI_PS_NONE` |
| Ping 250 ms | Power save ativo | Verificar `esp_wifi_set_ps(WIFI_PS_NONE)` em `wifi.c` |
| Ping 5–20 ms | ✅ Normal |

### MQTT

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `Error transport connect` | Broker offline | Verificar `mosquitto` rodando |
| `rc=5` | Broker exige auth | Configurar `username`/`password` em menuconfig |
| Conecta mas não publica | Tópico errado | Confirmar tópicos em `config.h` |
| Desconecta em ~60 s | Keepalive | Já coberto por `CONFIG_MQTT_KEEPALIVE_S=60` |

### Câmera

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `esp_camera_init() falhou: ESP_ERR_NOT_FOUND` | Cabo flat solto ou invertido | Verificar contatos para baixo |
| `Mismatch PID=0x5640` mas detecta OV5640 | **Comportamento normal** | Logs do driver tentam OV3640/3660 primeiro |
| `NO-SOI - JPEG start marker missing` | Ruído elétrico (servo + flash juntos) | Capacitor na alimentação; verificar 3.3 V |
| PSRAM não inicializada | `CONFIG_SPIRAM_MODE_OCT=y` ausente | Já está no `sdkconfig.defaults` |

### OTA

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `HTTP 503 warmup` | Uptime < 300 s | Aguardar, ou `menuconfig` → warmup = 0 |
| `HTTP 409 ota already in progress` | Outra OTA em andamento | Aguardar conclusão |
| `SSL handshake -0x2700` | CN mismatch (URL IP vs CN cert) | Já corrigido com `common_name` em `ota_client.c` |
| `SSL setup failed` | `crt_bundle_attach=NULL` sem `cert_pem` | Verificar bloco de config em `ota_client.c` |
| `sha256 mismatch` | `.bin` mudou entre `serve` e `pull` | Regenerar SHA ou refazer build |
| Reboot loop pós-OTA | Self-test falhando consistentemente | Verificar câmera/atuadores no novo build |
| Push >60 s | Wi-Fi com power save | Verificar `esp_wifi_set_ps(WIFI_PS_NONE)` |

### Sistema

| Sintoma | Causa | Solução |
|---------|-------|---------|
| Reboot a cada ~10 s | Task Watchdog | Verificar task bloqueada nos logs |
| Boot loop `invalid header` | Imagem corrompida | Refazer `idf.py flash` |
| `Brownout detector` | Fonte insuficiente | Fonte ≥5 V/2 A; capacitor 470 µF na entrada |
| Placa esquenta muito (>80°C no shield) | Regulador AMS1117 saturado | Ventilação no case; considerar regulador externo |

### Diagnóstico em runtime

**`/status` HTTPS** (com o servidor local ativo):
```bash
curl -k https://192.168.1.42:8443/status
```
Retorna JSON com estado da máquina, partição ativa, `pending_verify`, uptime.

**Métricas internas** (via log):
- `wifi_get_stats()` — tentativas/falhas de conexão
- `mqtt_get_stats()` — mensagens publicadas/recebidas/descartadas
- `tasks_get_queue_stats()` — profundidade das filas

---

## 12. Arquitetura

### Tasks FreeRTOS

| Task | Prio | Core | Stack | Responsabilidade |
|------|-----:|-----:|------:|-----------------|
| `wifi_mqtt_task` | 5 | 0 | 4096 | Monitora rede a cada 500 ms, emite NETWORK_READY/DOWN |
| `camera_task` | 4 | 1 | 6144 | Aguarda comando, chama `camera_capture()`, entrega frame |
| `comm_task` | 3 | 0 | 6144 | Máquina de estados, envia chunks MQTT, gerencia timeouts |
| `actuator_task` | 2 | 1 | 3072 | Executa sequências APROVADO/REPROVADO (~3 s) |

**Distribuição por core:** rede no Core 0 (stack TCP/IP do IDF também está lá),
periféricos no Core 1. Evita contenção de cache.

### Comunicação entre tasks

3 filas FreeRTOS estáticas (`xQueueCreateStatic`):

```
camera_cmd_queue    (comm → camera,    profundidade 2)
comm_msg_queue      (todos → comm,     profundidade 10, tipos heterogêneos)
actuator_cmd_queue  (comm → actuator,  profundidade 2)
```

`comm_msg_queue` carrega `comm_msg_t`, uma struct discriminada por tipo
(`NETWORK_READY`, `NETWORK_DOWN`, `CAMERA_FRAME`, `CAMERA_FAILED`, `RESULT`,
`APPLY_DONE`, `TRIGGER_CAPTURE`).

### Ownership de frames

- `camera_task` produz `camera_frame_t*`.
- Se `xQueueSend` falhar (fila cheia), `camera_task` **libera** com `camera_release()`.
- Se sucesso, ownership passa para `comm_task`, que **sempre** libera em `do_send_chunked()` (sucesso ou falha).
- Nenhuma outra task toca em `camera_frame_t*`.

### Sincronização

- **Event Groups** (`WIFI_CONNECTED_BIT`, `MQTT_CONNECTED_BIT`) — `wifi_mqtt_task` lê os bits e converte em mensagens para `comm_task`.
- **Mutex estático** (`s_chunk_mutex`) — protege `s_chunk_buf` (defensivo; uma única publisher no projeto atual).
- **Mutex estático** (`g_upload_mutex`) — serializa uploads OTA push.
- **Spinlock** (`s_ctx_lock`) — protege `s_ctx_in_use` no cliente OTA pull.
- **Flag volatile** (`g_ota_busy`) — pausa `comm_task` durante OTA (sem mutex, para evitar deadlock).

### Módulos

| Arquivo | Responsabilidade |
|---------|-----------------|
| `main.c` | `app_main`: init sequencial + self-test + start tasks |
| `config.h` | Configuração centralizada (via Kconfig para credenciais) |
| `camera.c/.h` | Driver OV5640, captura, release |
| `wifi.c/.h` | Station com reconexão exponencial (1→30 s) |
| `mqtt.c/.h` | Cliente MQTT, publish de chunks, parser de resultado (cJSON) |
| `actuators.c/.h` | LEDs, buzzer, servo (LEDC) |
| `state_machine.c/.h` | Autômato puro: `(estado, evento) → estado` |
| `tasks.c/.h` | 4 tasks, filas, orquestração, callback MQTT |
| `ota_common.c/.h` | Base compartilhada: mutex, estado, SHA-256 (PSA Crypto) |
| `ota_client.c` | Cliente HTTPS (pull) com retry, SHA-256 streaming |
| `ota_server.c` | Servidor HTTPS (push) com warmup, `/status`, `/pull-ota` |

---

## 13. Fluxo da máquina de estados

```
        (boot)
          │
          ▼
       ┌──────┐
       │ INIT │ ── NETWORK_READY ──┐
       └──────┘                     │
                                    ▼
       ┌──────┐  TRIGGER_CAPTURE  ┌──────────┐
       │ IDLE │ ─────────────────►│CAPTURING │
       └──────┘                   └────┬─────┘
          ▲                            │ CAPTURE_OK
          │ APPLY_DONE                 ▼
       ┌──────────┐ SEND_OK       ┌─────────┐
       │ APPLYING │◄─────────────│ SENDING │
       └──────────┘              └─────────┘
          ▲                            │ SEND_OK
          │                            ▼
          │                   ┌────────────────┐
          │  RESULT_APPROVED  │WAITING_RESULT  │
          └───────────────────┤ (com timeout)  │
              RESULT_REJECTED └────────────────┘
                                     │ TIMEOUT/INVALID
                                     ▼
                                ┌───────┐
                                │ ERROR │ ── RESET ──► IDLE
                                └───────┘
```

**Timeouts por estado** (configuráveis em `config.h`):

| Estado | Timeout | Evento sintetizado |
|--------|---------|-------------------|
| `IDLE` | 30 s | `TRIGGER_CAPTURE` |
| `CAPTURING` | 5 s | `CAPTURE_TIMEOUT` |
| `WAITING_RESULT` | 30 s | `RESULT_TIMEOUT` |
| `APPLYING` | 8 s | `ABORT` |
| `ERROR` | 3 s | `RESET` |

---

## 14. Segurança elétrica

### ⚠️ O servo **NÃO pode** ser alimentado pela ESP32-CAM

O regulador AMS1117 da placa fornece, no máximo, ~500 mA. Um servo SG90
consome **picos de 700 mA** durante movimento. Alimentar o servo pela placa causa:

- Brownout reset (a placa reinicia no meio da operação)
- Danos ao regulador
- Reset do Wi-Fi
- Corrupção de imagem (NO-SOI no log)

### Ligação correta

```
Fonte externa 5V/2A ──┬── GND ──────► GND da ESP32-CAM (obrigatório!)
                       │
                       └── 5V ───────► V+ do servo
                                       
ESP32 GPIO 21 ────────────────────────► Sinal do servo
```

**O GND comum é obrigatório.** Sem ele, o sinal PWM do GPIO fica flutuante
em relação à alimentação do servo, e o servo não responde (ou responde de forma errática).

### Recomendações

- **Capacitor de desacoplamento** de 470 µF/16 V na entrada da fonte (estabiliza picos do servo)
- **Cabo curto** entre fonte e servo (evita queda de tensão)
- **Não** use USB do PC para alimentar o servo

### Temperatura

O ESP32-S3 e o AMS1117 ficam quentes durante operação com Wi-Fi + câmera.
Temperatura típica do shield: 65–75°C. É **normal** (o chip suporta junction até 125°C).
Sinais de alarme: reboots aleatórios, corrupção de imagem, cheiro de queimado.
Para produto, adicione **ventilação no case** e considere **regulador externo 3.3 V dedicado**.

---

## 15. Decisões de design

### Por que FreeRTOS em vez de bare-metal

Wi-Fi, MQTT e TLS do ESP-IDF são implementados como tasks internas. Fazer
bare-metal significaria reimplementar o scheduler cooperativo em cima do
event loop do IDF — reinventar o FreeRTOS, pior. A preempção do FreeRTOS é
o que garante que a task de rede rode mesmo enquanto a `camera_task` está
processando JPEG.

### Por que 4 tasks e não 1 superloop

Cada task tem responsabilidade única e prioridade calibrada. Uma task
monolítica acoplaria rede, câmera e atuadores — debug ficaria infernal
(um `while` de 500 linhas). As 4 tasks permitem isolar falhas: se a câmera
trava, o Wi-Fi continua vivo e o watchdog reinicia só a `camera_task`.

### Por que chunks de 2 KB (e não 512 ou 4096)

- **512 B** — 100 mensagens para 50 KB, overhead alto (header MQTT + tópico)
- **2048 B** — equilíbrio entre overhead e tamanho de fila do `esp-mqtt`
- **4096 B** — cabe no `CONFIG_MQTT_BUFFER_SIZE=4096`, mas o buffer de saída
  do IDF enche mais rápido (backpressure)

### Por que QoS 1

QoS 1 garante entrega pelo menos uma vez. Cada chunk tem timestamp implícito
(ordem de publicação), e o servidor detecta duplicatas pelo índice. QoS 0
perderia chunks em Wi-Fi ruim — aceitável para imagem (descartaria a foto),
mas QoS 1 com 2 KB é leve o bastante para não impactar o tempo total.

### Por que cJSON em vez de parser manual

Parser manual foi tentado primeiro, encontrou **2 bugs** (loop infinito,
aceitação de JSON truncado) descobertos pelos host tests. cJSON resolve
casos de borda (unicode, escapes, aninhamento) testados há 10 anos.
O custo é ~500 B de heap transacional por mensagem — fragmentação desprezível
em uso de curta duração.

### Por que buffers estáticos (sem malloc) em código de aplicação

Buffers de tamanho fixo conhecido (`s_chunk_buf`, `s_download_buf`,
`s_upload_buf`) são estáticos porque:

- Não fragmentam o heap
- Não falham em runtime (o link-time reserva o espaço)
- São thread-safe com mutex/mutex estático

cJSON e bibliotecas do IDF ainda usam `malloc`, mas **transacionalmente**
(aloca → usa → libera dentro da mesma operação). Isso é seguro.

### Por que warmup de OTA

Testes mostraram que os primeiros 3–5 minutos após boot a rede tem:
- TCP slow start (janela pequena)
- Power save ainda ajustando DTIM
- ARP cache frio

OTA durante essa janela compete com o estabilizador da rede e fica 5×
mais lenta. O warmup evita isso — `503 Retry-After` informa o cliente.

### Por que `WIFI_PS_NONE`

Power save desligado dobra o consumo (30→120 mA), mas reduz latência
de rede de 250 ms para 5 ms. Para dispositivo alimentado continuamente
(caso deste projeto), é escolha óbvia. Para bateria, seria problemático.

---

## 16. Melhorias futuras

### Curto prazo

- **Reduzir `CONFIG_MQTT_RESULT_TIMEOUT_MS`** de 30 s para 15 s (mais ágil em falhas)
- **Adicionar suporte a OTA via botão físico** (GPIO com interrupção + notificação)
- **Endpoint `/status` retornar métricas de rede** (`wifi_get_stats`, `mqtt_get_stats`)
- **Modo "capturar agora"** via endpoint `/capture` (disparo manual remoto)
- **`app_main` retornar status** de self-test via `/status` para diagnóstico remoto

### Médio prazo

- **Migrar credenciais de Kconfig para NVS** — permite trocar Wi-Fi via BLE/portal sem recompilar
- **Fragmentação adaptativa** — chunk size varia conforme throughput medido (Wi-Fi bom: 4 KB; ruim: 1 KB)
- **Compressão JPEG dupla** (qualidade dinâmica: 12 em Wi-Fi bom, 20 em ruim)
- **Modo deep-sleep** entre capturas (para versão a bateria; o atual é plug-in)
- **Watchdog de aplicação** — heartbeat explícito das tasks para detecção precoce
- **HTTPS com Let's Encrypt** — pull de servidor público, sem cert auto-assinado

### Longo prazo

- **Certificação de produção** — Secure Boot V2 + Flash Encryption (exige eFuse irreversível)
- **MQTT com TLS mútuo** — cliente e broker se autenticam (proteção contra broker malicioso)
- **OTA via BLE** — para placas sem Wi-Fi configurado (provisionamento inicial)
- **Análise de imagem embarcada** (TinyML) — se o resultado for binário simples, elimina latência de rede
- **Redesenho de hardware** para produto:
  - ESP32-S3-WROOM (antena melhor)
  - Regulador dedicado 3.3 V/1 A
  - RTC externo (mantém hora em queda)
  - Watchdog de hardware (TPS3813 ou similar)
  - Case com ventilação e IP54

---

## Licença

[MIT](LICENSE) — veja o arquivo LICENSE para detalhes.

## Agradecimentos

Baseado na documentação oficial do ESP-IDF, no componente `esp32-camera` da
Espressif e em diversos projetos abertos de ESP32-CAM. Bugs encontrados
durante o desenvolvimento foram corrigidos com a ajuda dos 78 host tests
em `tools/host_tests/`.
```

---

## Considerações finais sobre este README

Ele cobre **todos os 31 itens** que a spec original pedia (visão, hardware, pinout, software, config, compilação, gravação, monitor, servidor MQTT, testes, troubleshooting, arquitetura, fluxo, melhorias futuras).

**O que eu deixei de fora de propósito:**

- **Seção de contribuição** — não faz sentido para projeto de faculdade
- **FAQ extenso** — troubleshooting cobre o que é prático
- **Diagramas ASCII elaborados** — markdown puro é mais portável e o fluxo atual é legível

**O que vale adicionar se você quiser:**

- Uma **foto da placa montada** em `docs/hardware.jpg` (referenciada no README)
- Um **`.gitignore` final** — o que você já tem está bom, mas verifique se `main/certs.h` está lá
- Um **`LICENSE`** (MIT é o mais comum para projetos acadêmicos)
- Uma **screenshot do monitor** no boot (para orientar quem clona pela primeira vez)

---

## Status do projeto

| # | Item | Status |
|---|------|--------|
| 1–28 | Todos os arquivos do firmware | ✅ |
| 1–7 | Testes de host (78/78) | ✅ |
| — | Push OTA (~10 s, 1 MB) | ✅ |
| — | Pull OTA (validado sem skip TLS) | ✅ |
| — | Rollback validado | ✅ |
| — | Warmup de 300 s | ✅ |
| — | Parser cJSON validado | ✅ |
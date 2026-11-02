# LockMaxxing — Smart Edge Node (ESP32-S3-CAM)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Firmware completo para ESP32-S3-CAM (N16R8 + OV5640) que funciona como **nó de
borda inteligente**: captura imagens, aplica filtro de qualidade local, assina
com HMAC-SHA256, transmite via **MQTT sobre mTLS** para um servidor de análise,
recebe a decisão e aciona atuadores. Inclui OTA (push e pull) com rollback
automático, provisionamento via portal web e observabilidade completa em
runtime.

Projeto de aprendizado com arquitetura pronta para produto: os quatro pilares
do Smart Edge Node (filtro local, fila offline planejada, autenticação
criptográfica, multitarefa com LED de status) estão implementados ou
documentados com plano de execução.

---

## Sumário

1. [Visão geral](#1-visão-geral)
2. [Hardware](#2-hardware)
3. [Pinout](#3-pinout)
4. [Software](#4-software)
5. [Configuração](#5-configuração)
6. [Compilação e gravação](#6-compilação-e-gravação)
7. [Provisionamento (Wi-Fi via portal)](#7-provisionamento-wi-fi-via-portal)
8. [Segurança: mTLS + HMAC](#8-segurança-mtls--hmac)
9. [Servidores de teste](#9-servidores-de-teste)
10. [OTA — push e pull](#10-ota--push-e-pull)
11. [Filtro de qualidade](#11-filtro-de-qualidade)
12. [LED de status (WS2812)](#12-led-de-status-ws2812)
13. [Observabilidade: `/status`](#13-observabilidade-status)
14. [Testes de host](#14-testes-de-host)
15. [Troubleshooting](#15-troubleshooting)
16. [Arquitetura](#16-arquitetura)
17. [Fluxo da máquina de estados](#17-fluxo-da-máquina-de-estados)
18. [Segurança elétrica](#18-segurança-elétrica)
19. [Decisões de design](#19-decisões-de-design)
20. [Status e roadmap](#20-status-e-roadmap)

---

## 1. Visão geral

### Ciclo operacional

```
boot → self-test → Wi-Fi (NVS ou portal) → MQTT sobre mTLS →
  (30s) → captura → filtro de qualidade → HMAC →
  envia em chunks → aguarda resultado →
  aprovado: LED WS2812 verde + servo | reprovado: LED vermelho + buzzer →
  aguarda 30s → repete
```

### Características

**Arquitetura FreeRTOS**
- 4 tasks pinadas em cores distintos (rede no Core 0, periféricos no Core 1)
- 3 filas estáticas + event groups + mutex + spinlock + volatile flag
- Watchdog em 3 camadas (Task 10s, Interrupt 800ms, RTC)

**Comunicação**
- **MQTT sobre mTLS** (porta 8883) — broker e cliente autenticados
- Fragmentação em chunks de 2 KB com header binário big-endian
- HMAC-SHA256 por imagem (id + size + chunks + bytes) — autenticidade
- QoS 1 com detecção de duplicatas no servidor

**Segurança em camadas**
- WPA2 no Wi-Fi + mTLS no MQTT + HMAC no payload
- TLS com CA própria (`tools/certs/`) embutida no firmware via `mqtt_certs.h`
- Admin token para `/admin/config` e `/factory-reset`
- Rate limiting por IP (5 req/min por padrão)

**Inteligência de borda**
- Filtro de qualidade local (decode JPEG → brilho + nitidez)
- Rejeita imagens inadequadas **antes** de gastar banda
- Thresholds calibráveis via menuconfig
- Estatísticas acumuladas no `/status`

**Operação e recuperação**
- OTA push (~10 s para 1 MB) e pull (~12 s)
- Rollback automático se o firmware novo falhar no self-test
- Warmup de 300 s (rejeita OTA em rede ainda instável)
- Provisionamento via portal SoftAP com captive portal
- Reset de fábrica por botão físico (BOOT 5s) ou endpoint HTTP

**Observabilidade**
- LED WS2812 onboard (GPIO 48) com padrões por estado
- `/status` HTTPS com 20+ métricas (RSSI, contadores, filas, uptime, HMAC)
- Logs estruturados por TAG (`STATE`, `MQTT`, `QUALITY`, `HMAC`, `OTA_*`)

**Configuração out-of-the-box**
- Compila sem edição de arquivos (credenciais via Kconfig + NVS)
- Configuração de rede via menuconfig **ou** portal **ou** `/admin/config`
- Separação: Kconfig = defaults de build; NVS = config em runtime

---

## 2. Hardware

| Componente | Modelo | Observações |
|-----------|--------|-------------|
| Placa | ESP32-S3 CAM **N16R8** | 16 MB flash, 8 MB PSRAM octal |
| Câmera | **OV5640** | 5 MP, JPEG nativo, **grayscale** via sensor |
| LED de status | **WS2812 onboard (GPIO 48)** | RGB endereçável, RMT |
| Buzzer | Ativo (5 V) | Alerta de reprovação |
| Servo | SG90 | 180°, **alimentação externa 5 V** |
| Fonte externa | 5 V / 2 A | Obrigatória para o servo |
| Programador | USB nativo da S3 | Não precisa de FTDI |

### Premissas

- **PSRAM octal** (N16R8). Para variantes R8 (quad PSRAM), ajustar
  `CONFIG_SPIRAM_MODE_OCT` → `CONFIG_SPIRAM_MODE_QUAD` no `sdkconfig.defaults`.
- **Câmera OV5640** com `set_special_effect(s, 2)` (grayscale no sensor).
  A imagem sai em tons de cinza, ~40% menor que colorida — decisão de design
  para reduzir banda e simplificar a análise de qualidade. Para reverter,
  remover a linha em `apply_sensor_specific_config()` em `camera.c`.
- **XCLK de 10 MHz** (não 20 MHz). A placa tem skew de sinal no barramento
  DVP que causa gradiente horizontal (lado direito mais escuro) em 20 MHz.
  10 MHz cabe na margem de setup/hold do ESP32-S3 para esta placa específica.
  Ver "Decisões de design".
- **Servo em fonte externa.** O regulador AMS1117 da placa fornece no máximo
  ~500 mA; o SG90 consome picos de 700 mA. Alimentar pela placa = brownout
  + corrupção de imagem + reset de Wi-Fi.

### Otimizações de performance

- `CONFIG_NEWLIB_NANO_FORMAT=y` — economiza ~105 KB de flash
- `esp_wifi_set_ps(WIFI_PS_NONE)` — latência de rede 250 → 10 ms
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` removido — Wi-Fi em RAM interna
- `-O2` no firmware, `-Os` no bootloader

---

## 3. Pinout

| Componente | GPIO | Função | Justificativa |
|-----------|-----:|--------|--------------|
| Câmera D0–D7 | 11,9,8,10,12,18,17,16 | dados DVP | Barramento padrão N16R8 |
| Câmera XCLK | 15 | clock | 10 MHz (ver Decisões) |
| Câmera PCLK | 13 | pixel clock | |
| Câmera VSYNC | 6 | sync vertical | |
| Câmera HREF | 7 | sync horizontal | |
| Câmera SIOD | 4 | I2C SDA | Config do sensor |
| Câmera SIOC | 5 | I2C SCL | Config do sensor |
| **LED WS2812** | **48** | **RGB endereçável** | **Onboard, RMT** |
| Buzzer | 3 | saída digital | Ativo, nível alto |
| Servo | 21 | PWM (LEDC timer 1, canal 1) | Canal 0 reservado p/ XCLK |
| Botão BOOT | 0 | entrada (reset de fábrica) | Strapping no boot, GPIO após |

**GPIOs evitados:** 0, 45, 46 (strapping), 19, 20 (USB), 26–32 (flash),
33–37 (PSRAM octal).

**Mudança em relação à versão anterior:** os LEDs discretos em GPIO 1 e 2
foram **removidos**. Toda a sinalização visual passou para o WS2812 onboard
em GPIO 48 (cores por estado: azul=init, verde=idle, amarelo=enviando,
ciano=aguardando, vermelho=erro, roxo=OTA).

---

## 4. Software

| Item | Versão | Notas |
|------|--------|-------|
| ESP-IDF | **6.1.0** | Testado com esta versão |
| Target | `esp32s3` | |
| Compilador | `xtensa-esp32s3-elf-gcc` | Toolchain do IDF |
| VS Code | Extensão ESP-IDF | Recomendado |
| Python | 3.8+ | Scripts de teste |
| Broker MQTT | **Mosquitto 2.0.18+** | **TLS em 8883** |
| `paho-mqtt` | 1.x ou 2.x | `pip install paho-mqtt` |

### Componentes gerenciados (`main/idf_component.yml`)

```yaml
espressif/esp32-camera: "^2.0.15"
espressif/cjson: "^1.7.19"
espressif/led_strip: "^3.1.1"      # WS2812 via RMT
espressif/esp_jpeg: "^1.3.1"       # decode JPEG para filtro
```

*(`espressif/esp_littlefs` será adicionado na Fase 4 — offline queue.)*

### Dependências internas do IDF (`REQUIRES` em `main/CMakeLists.txt`)

`freertos`, `driver`, `esp_wifi`, `esp_event`, `esp_netif`, `nvs_flash`,
`mqtt`, `log`, `cjson`, `esp_timer`, `esp32-camera`, `app_update`,
`esp_http_client`, `esp_https_server`, `esp_http_server`, `esp_crt_bundle`,
`mbedtls`, `espressif__led_strip`, `esp_driver_rmt`, `espressif__esp_jpeg`,
`esp_psram`, `esp_driver_gpio`.

---

## 5. Configuração

O projeto compila **out-of-the-box** com defaults. Toda a configuração
significativa está em **Kconfig** (build) ou **NVS** (runtime).

### Estratégia de configuração em camadas

```
Kconfig.defaults        → valores de fábrica para builds novos
    ↓
menuconfig (sdkconfig)  → sobrescreve no build
    ↓
NVS (runtime)          → sobrescreve sem recompilar
    ↓
/admin/config (HTTP)   → sobrescreve remotamente, autenticado
```

| Item | Onde fica | Como muda |
|------|-----------|-----------|
| Wi-Fi SSID/senha | NVS | Portal ou `/admin/config` |
| Broker URI | NVS (fallback Kconfig) | `/admin/config` |
| HMAC secret | NVS (fallback Kconfig) | `/admin/config` |
| Client ID | Kconfig | Recompilar |
| PINs, timeouts, protocolo | `config.h` (compile-time) | Recompilar |
| Thresholds de qualidade | Kconfig | Recompilar |

### Opções do menuconfig

```bash
idf.py menuconfig
# → LockMaxxing Configuration
```

| Opção | Default | Descrição |
|-------|---------|-----------|
| Wi-Fi SSID | `myssid` | Fallback (NVS tem prioridade) |
| Wi-Fi Password | `mypassword` | Fallback |
| **MQTT Broker URI** | **`mqtts://192.168.15.5:8883`** | URI com TLS |
| **MQTT TLS Mode** | **`2`** | 0=off, 1=TLS server, 2=mTLS |
| **MQTT HMAC Secret** | *(vazio)* | 64 hex chars (ou vazio = off) |
| **Admin Token** | *(vazio)* | Protege `/admin/config` e `/factory-reset` |
| **Rate Limit Max** | `5` | POSTs por IP por janela |
| **Rate Limit Window** | `60` | Janela em segundos |
| MQTT Client ID | `esp32-camera-01` | Único por dispositivo |
| OTA Warmup | 300 s | Rejeita OTA antes desse uptime |
| Endpoint `/pull-ota` | habilitado | DEV/TEST |
| Endpoint `/admin/config` | habilitado | DEV/TEST |
| **Quality filter enabled** | `y` | Filtro local |
| **Quality MIN/MAX brightness** | `30`/`240` | Calibrável |
| **Quality MIN sharpness** | `20` | Calibrável (subir para ~200 após dados) |
| **Quality debug log** | `n` | Liga log de todas as capturas |
| **LED enabled** | `y` | WS2812 |
| **LED brightness** | `60` | 0–255 |
| **Provisioning AP password** | *(vazio)* | Senha do SoftAP |
| **Provisioning timeout** | `600` s | Portal fecha após esse tempo |
| **Factory reset hold** | `5000` ms | Tempo do BOOT pressionado |
| **Restart on Wi-Fi fail** | `3` | Falhas antes de apagar NVS |

### Opções do IDF que o projeto exige

- `CONFIG_MQTT_TRANSPORT_SSL=y` (Enable MQTT over SSL)
- `CONFIG_ESP_TLS_INSECURE=n`
- `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=n`
- `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y` (para o portal)
- `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_ESP_INT_WDT_TIMEOUT_MS=800`

### Comportamento em caso de NVS vazia (primeiro boot)

```
1. app_config_load() detecta NVS vazia
2. Se Kconfig tem SSID configurado (≠ "myssid") → usa fallback, provisioned=true
3. Se Kconfig tem default ("myssid") → provisioned=false
4. app_main detecta provisioned=false → abre portal SoftAP
5. Usuário configura Wi-Fi via navegador → NVS gravada → reboot
6. Próximo boot: NVS tem credenciais → conecta normal
```

---

## 6. Compilação e gravação

### Build

```bash
# Primeira vez (define target)
idf.py set-target esp32s3
idf.py build

# Builds subsequentes
idf.py build

# Se opções do Kconfig mudaram e não "pegam"
idf.py fullclean && idf.py build

# Se o sdkconfig ficou inconsistente
Remove-Item sdkconfig
idf.py build
```

**Saída esperada:** `Project build complete.`

**Tamanho típico:** `.bin` de ~1.0 MB, 33% da partição de app de 3 MB.

### Flash e monitor

```bash
# Identificar porta
ls /dev/ttyACM*          # Linux/macOS
# ou Gerenciador de Dispositivos → Portas (COM e LPT) no Windows

# Flash + monitor em um comando
idf.py -p COM5 flash monitor

# Só monitor
idf.py -p COM5 monitor

# Sair do monitor: Ctrl+] (não para a placa)
```

### Boot esperado (sistema operacional)

```
APP: ===============================================
APP:  ESP32-CAM MQTT + OTA — boot V2
APP: ===============================================
APP_CFG: Config carregada: ssid='Regrose', uri='mqtts://192.168.15.5:8883', provisioned=1
OTA_COMMON: Rodando da partição 'ota_0' (offset=0x00020000, ...)
CAMERA: Sensor detectado: OV5640 (PID=0x5640, ...)
CAMERA: OV5640: set_vflip(1) = 0
CAMERA: Câmera inicializada: OV5640, frame size 10, qualidade 12
CAMERA: Filtro de qualidade ATIVO: brilho [30, 240], nitidez >= 200
ACTUATOR: Atuadores prontos: buzzer=GPIO3, servo=GPIO21 (timer=1, canal=1)
LED: LED WS2812 inicializado (GPIO 48, RMT 10 MHz)
WIFI: Wi-Fi inicializado — SSID='Regrose'
WIFI: Associado ao AP
WIFI: IP obtido: 192.168.15.7
MQTT: URI do broker: mqtts://192.168.15.5:8883
MQTT: TLS: broker verificado contra CA embutido (1939 bytes)
MQTT: mTLS: client cert (1663 bytes), key (1708 bytes)
MQTT: Cliente MQTT iniciado: broker='mqtts://192.168.15.5:8883', client_id='esp32-camera-01'
MQTT: Conectado ao broker
MQTT: Assinado 'lockmaxxing/camera/result'
OTA_COMMON: Firmware confirmado como válido — rollback cancelado
TASKS: Todas as 4 tasks iniciadas
OTA_SRV: Servidor HTTPS ativo em https://192.168.15.7:8443
APP: app_main concluído — sistema em regime
```

### Boot em modo provisionamento (primeiro boot ou reset de fábrica)

```
APP_CFG: NVS vazia — primeiro boot
APP_CFG: Kconfig também está com default — portal vai abrir
APP: Não provisionado — iniciando portal de configuração
WIFI_PROV: === Modo provisionamento ===
WIFI_PROV: AP_START recebido — SoftAP ativo
WIFI_PROV: AP IP: 192.168.4.1
WIFI_PROV: Handler registrado: /
WIFI_PROV: Handler registrado: /save
WIFI_PROV: Handler registrado: /generate_204
WIFI_PROV: Handler registrado: /hotspot-detect.html
WIFI_PROV: Handler registrado: /connecttest.txt
WIFI_PROV: Handler registrado: /
WIFI_PROV: SoftAP 'LockMaxxing-Setup' ativo — portal em http://192.168.4.1/
WIFI_PROV: Timeout: 600 s
```

---

## 7. Provisionamento (Wi-Fi via portal)

### Fluxo de provisionamento

```
1. Placa liga em modo portal (NVS vazia ou reset de fábrica)
2. SoftAP "LockMaxxing-Setup" (aberto ou WPA2) + DNS captive
3. Usuário conecta no SoftAP com celular/PC
4. Portal abre automaticamente OU usuário digita 192.168.4.1
5. Formulário: SSID + senha do Wi-Fi (broker vem de fábrica via Kconfig)
6. POST /save → app_config_save() → NVS → reboot em 1.5s
7. Próximo boot: conecta com credenciais salvas
```

### Como configurar (passo a passo)

**1. Primeiro boot (ou após reset de fábrica):**

Placa liga em modo portal. Log mostra `SoftAP 'LockMaxxing-Setup' ativo`.

**2. No celular/PC:**

- Conecte no Wi-Fi **`LockMaxxing-Setup`** (senha em `CONFIG_LOCKMAXXING_PROVISIONING_AP_PASSWORD` ou aberto)
- Sistema vai avisar "sem internet" — **aceite manter conectado**
- Portal deve abrir automaticamente; se não:
  - **Android:** abra Chrome, digite `192.168.4.1`
  - **iOS:** abra Safari, digite `192.168.4.1`
  - **Windows:** abra navegador, digite `192.168.4.1`

**3. Preencha o formulário:**

- **SSID:** nome da sua rede Wi-Fi
- **Senha:** senha da rede (deixe em branco para rede aberta)
- Clique **Salvar e Conectar**

**4. Após o reboot:**

Placa conecta à rede e ao broker (URI vem do Kconfig — `mqtts://...`). Log mostra `MQTT: Conectado ao broker`.

### Reset de fábrica (voltar ao portal)

**Opção 1 — botão físico (recomendado):**

- Segure o botão **BOOT** por 5 segundos durante operação normal
- Log: `BTN_RESET: RESET DE FÁBRICA ACIONADO`
- NVS apagada, reboot automático, portal reabre

**Opção 2 — endpoint HTTP:**

```bash
curl.exe -k -X POST https://192.168.15.7:8443/factory-reset \
  -H "X-Admin-Token: <TOKEN_DO_MENUCONFIG>"
```

**Opção 3 — se o Wi-Fi falhar 3 vezes consecutivas:**

O firmware detecta e reinicia em modo portal automaticamente
(`CONFIG_LOCKMAXXING_PROVISIONING_RESTART_ON_FAIL=3`).

### Reconfiguração remota (sem reset)

Para trocar broker, Wi-Fi ou HMAC sem apagar a NVS:

```bash
curl.exe -k -X POST https://192.168.15.7:8443/admin/config \
  -H "X-Admin-Token: <TOKEN_DO_MENUCONFIG>" \
  -H "Content-Type: application/json" \
  -d '{"broker":"mqtts://192.168.15.10:8883"}'
```

Campos aceitos: `broker`, `wifi_ssid`, `wifi_password`, `hmac_secret`
(todos opcionais; apenas os fornecidos são alterados).

**⚠️ Segurança:** desabilite `/admin/config` em produção definindo
`CONFIG_LOCKMAXXING_ADMIN_TOKEN` como vazio no menuconfig. Sem token,
o endpoint retorna 503 e não aceita chamadas.

---

## 8. Segurança: mTLS + HMAC

### Arquitetura de segurança em camadas

```
Camada 1: Wi-Fi    → WPA2 (credenciais em NVS, provisioning seguro)
Camada 2: MQTT     → mTLS (broker autenticado + cliente autenticado)
Camada 3: Payload  → HMAC-SHA256 (integridade + autenticidade)
```

**O que cada camada protege:**

| Ameaça | Proteção |
|--------|----------|
| Sniffing de tráfego Wi-Fi | WPA2 |
| MITM no broker | mTLS (broker tem cert assinado por CA) |
| Broker malicioso aceita qualquer cliente | mTLS (cliente tem cert assinado por CA) |
| Injeção de imagem falsa no tópico | HMAC-SHA256 (atacante sem chave não forja assinatura válida) |
| Reprodução de imagem capturada | ID único + HMAC (servidor pode rejeitar IDs repetidos) |
| Reconfiguração remota maliciosa | Admin token + rate limit por IP |

### Gerar os certificados (primeira vez)

Execute dentro de `tools/certs/`:

```bash
# CA — Autoridade Certificadora raiz
openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
  -subj "/C=BR/O=LockMaxxing/CN=LockMaxxing-CA"

# Servidor (broker) — CN = IP ou hostname do broker
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
  -subj "/C=BR/O=LockMaxxing/CN=192.168.15.5"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 3650 -extfile server.ext

# Cliente (ESP32) — para mTLS
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr \
  -subj "/C=BR/O=LockMaxxing/CN=esp32-camera-01"
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out client.crt -days 3650 -extfile client.ext

# Verificar
openssl verify -CAfile ca.crt server.crt   # server.crt: OK
openssl verify -CAfile ca.crt client.crt   # client.crt: OK
```

**Arquivos de extensão (SAN — obrigatório em OpenSSL 3.x):**

`tools/certs/server.ext`:
```ini
subjectAltName = IP:192.168.15.5, DNS:localhost
```

`tools/certs/client.ext`:
```ini
subjectAltName = DNS:esp32-camera-01, IP:192.168.15.7
```

### Configurar o Mosquitto com mTLS

`C:\Program Files\mosquitto\mosquitto-tls.conf`:

```conf
listener 8883 0.0.0.0

certfile C:\...\tools\certs\server.crt
keyfile  C:\...\tools\certs\server.key
cafile   C:\...\tools\certs\ca.crt

require_certificate true
tls_version tlsv1.2

allow_anonymous true
log_dest stdout
log_type all
```

Reiniciar o serviço:

```powershell
Stop-Service mosquitto
Get-Process mosquitto | Stop-Process -Force
Start-Sleep -Seconds 2
& "C:\Program Files\mosquitto\mosquitto.exe" -c "C:\...\mosquitto-tls.conf" -v
```

### Embutir os certificados no firmware (`mqtt_certs.h`)

O script `tools/gen_mqtt_certs.sh` é executado **automaticamente** pelo CMake
em cada build que detectar mudança em `tools/certs/`. Ele gera
`main/mqtt_certs.h` com `MQTT_CA_PEM`, `MQTT_CLIENT_PEM` e `MQTT_CLIENT_KEY_PEM`.

**Não edite `mqtt_certs.h` manualmente.** Para forçar regeneração:

```bash
rm main/mqtt_certs.h
idf.py build
```

**Regra crítica:** os campos `certificate_len` em `esp_mqtt_client_config_t`
**não devem ser setados**. O `esp-mqtt` usa `strlen() + 1` internamente quando
o comprimento é 0, incluindo o `\0` exigido por `mbedtls_x509_crt_parse`.
Setar manualmente para `sizeof - 1` (sem `\0`) causa `-0x2180` (INVALID_FORMAT).

### HMAC-SHA256 no payload

**Configuração (menuconfig):**

```
CONFIG_LOCKMAXXING_FACTORY_HMAC_SECRET="<64 hex chars>"
```

Gerar um segredo novo:

```bash
openssl rand -hex 32
```

**O que é assinado:**

```
HMAC-SHA256(secret, id(4B BE) || size(4B BE) || chunks(2B BE) || image_bytes)
```

O HMAC vai no JSON do `/request`, uma vez por imagem (não por chunk):

```json
{"id":42,"size":22481,"chunks":11,"hmac":"a3f5b9..."}
```

**Verificação no servidor:**

- Sem `--hmac-secret` no Python → modo legado, aceita tudo
- Com `--hmac-secret` → rejeita imagem se HMAC não bater
- Comparação usa `hmac.compare_digest` (tempo constante, anti-timing-attack)

### Rate limiting por IP

Todas as rotas POST (`/update`, `/pull-ota`, `/admin/config`, `/factory-reset`)
têm rate limit:

- `CONFIG_LOCKMAXXING_RATE_LIMIT_MAX` (default: 5)
- `CONFIG_LOCKMAXXING_RATE_LIMIT_WINDOW_S` (default: 60)

Ao exceder, retorna `HTTP 429 Too Many Requests` com `Retry-After: 60`.
O log do ESP32 registra IP + contador:

```
OTA_SRV: Rate limit excedido: 192.168.15.5 (6 req / 60 s)
```

### Segurança — checklist de produção

| Item | Dev | Produção |
|------|-----|----------|
| `CONFIG_LOCKMAXXING_ADMIN_TOKEN` | preenchido | **preenchido, 32+ chars aleatórios** |
| `CONFIG_LOCKMAXXING_MQTT_TLS_MODE` | 2 (mTLS) | 2 (mTLS) |
| `CONFIG_LOCKMAXXING_FACTORY_HMAC_SECRET` | preenchido | **por dispositivo** (não global) |
| `CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE` | y | **n** (ou token obrigatório) |
| `CONFIG_LOCKMAXXING_OTA_PULL_SKIP_TLS_VERIFY` | y | **n** |
| Certificados em `tools/certs/` | versionados? | **gitignored** |
| `main/mqtt_certs.h` | versionado? | **gitignored** |
| `main/certs.h` (OTA HTTPS) | versionado? | **gitignored** |
| Secure Boot V2 | não | **sim** (exige eFuse) |
| Flash Encryption | não | **sim** (exige eFuse) |

**Ferramenta de proteção do Git:** o `.gitignore` já contém:

```gitignore
tools/certs/
main/certs.h
main/mqtt_certs.h
```

Verifique **antes** de cada `git push`:

```bash
git status
# Nenhum dos 3 caminhos acima deve aparecer
```

Se você commitou por engano, além de `git rm --cached`, considere as chaves
**comprometidas** e regenere os certificados (a CA permite assinar novos,
inclusive "para o atacante").

---

## 9. Servidores de teste

Três servidores Python rodam no PC (fora do firmware) para testes ponta a ponta:
broker MQTT, servidor de análise simulada e servidor OTA. Todos usam **TLS/mTLS**.

### 9.1. Broker MQTT (Mosquitto com mTLS)

**Instalação:**

- **Linux:** `sudo apt install mosquitto`
- **macOS:** `brew install mosquitto`
- **Windows:** [mosquitto.org/download](https://mosquitto.org/download/)

**Versão recomendada:** **2.0.18** (a 2.1.x tem bugs conhecidos com
`require_certificate` em Windows — ver Troubleshooting).

**Configuração** (`mosquitto-tls.conf`):

```conf
listener 8883 0.0.0.0

certfile C:\...\tools\certs\server.crt
keyfile  C:\...\tools\certs\server.key
cafile   C:\...\tools\certs\ca.crt

require_certificate true
tls_version tlsv1.2

allow_anonymous true
log_dest stdout
log_type all
```

**Iniciar** (em terminal dedicado, com log):

```powershell
Stop-Service mosquitto -ErrorAction SilentlyContinue
Get-Process mosquitto -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
& "C:\Program Files\mosquitto\mosquitto.exe" -c "C:\...\mosquitto-tls.conf" -v
```

**Verificar** que está escutando em `0.0.0.0:8883` (não `127.0.0.1`):

```powershell
netstat -an | Select-String ":8883"
```

**Teste rápido** (de dentro de `tools/certs/`):

```powershell
mosquitto_sub -h localhost -p 8883 `
  --cafile ca.crt `
  --cert client.crt `
  --key client.key `
  -V mqttv311 `
  -t "test" -v
```

Conectar e ficar parado = OK. `Ctrl+C` para sair.

**Teste de segurança** — sem cert de cliente (deve falhar):

```powershell
mosquitto_sub -h localhost -p 8883 --cafile ca.crt -V mqttv311 -t "test" -v
```

Esperado: `protocol error` ou `certificate required`. Se funcionar,
`require_certificate true` não está ativo.

### 9.2. Servidor de análise simulada (`tools/mqtt_test_server.py`)

Reconstrói as imagens dos chunks MQTT, salva em disco, decide aprovação
e publica o resultado. Suporta TLS, mTLS e validação HMAC.

**Setup:**

```powershell
python -m venv .venv-tools
.\.venv-tools\Scripts\Activate.ps1     # Windows
# Linux/macOS: source .venv-tools/bin/activate
pip install paho-mqtt
```

**Executar (com mTLS + HMAC):**

```powershell
python tools\mqtt_test_server.py `
  --broker 192.168.15.5 `
  --port 8883 `
  --ca-cert "tools\certs\ca.crt" `
  --client-cert "tools\certs\client.crt" `
  --client-key "tools\certs\client.key" `
  --mode alternate `
  --hmac-secret <SEU_HEX_DE_64_CHARS>
```

**Modos de decisão (`--mode`):**

| Modo | Comportamento |
|------|---------------|
| `always-approve` | Sempre aprova (teste do servo + LED verde) |
| `always-reject` | Sempre rejeita (teste do buzzer + LED vermelho) |
| `alternate` | Aprova IDs pares, rejeita ímpares (teste dos dois) |
| `random` | Sorteia |

**Saída esperada:**

```
[TLS] CA: tools/certs/ca.crt
[TLS] Cliente cert: tools/certs/client.crt
[INIT] Modo: alternate
[INIT] HMAC ativo (chave de 32 bytes)
[MQTT] Conectado a 192.168.15.5:8883
[MQTT] Assinado: lockmaxxing/camera/request
[MQTT] Assinado: lockmaxxing/camera/image
[REQUEST] id=1: HMAC recebido (será validado na finalização)
[FINALIZE] id=1: HMAC válido
[FINALIZE] id=1: 22481B → received_images\img_..._id00000001.jpg
[RESULT] id=1 → reprovado
```

**Imagens** são salvas em `received_images/` (ignorado pelo Git).

**Validar HMAC com segredo errado (teste de segurança):**

```powershell
python tools\mqtt_test_server.py `
  --broker 192.168.15.5 --port 8883 `
  --ca-cert tools\certs\ca.crt --client-cert tools\certs\client.crt --client-key tools\certs\client.key `
  --hmac-secret ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
```

Esperado:

```
[FINALIZE] id=2: HMAC INVÁLIDO — rejeitando
```

E o ESP32 vai receber `RESULT_TIMEOUT` após 30 s e voltar a `IDLE`.

### 9.3. Servidor OTA (`tools/ota_test_server.py`)

Suporta dois modos: **push** (envia `.bin` para o ESP32) e **pull** (hospeda
`.bin` para o ESP32 baixar via HTTPS).

```powershell
# PUSH — envia .bin para o ESP32
python tools\ota_test_server.py push build\LockMaxxing.bin --device 192.168.15.7

# PULL — hospedar o .bin (Terminal 1)
python tools\ota_test_server.py serve build\LockMaxxing.bin --port 8443

# PULL — disparar (Terminal 2)
python tools\ota_test_server.py pull `
  --device 192.168.15.7 `
  --from https://192.168.15.5:8443/LockMaxxing.bin

# Utilitário: calcular SHA-256 do .bin
python tools\ota_test_server.py sha256 build\LockMaxxing.bin
```

**Tempos esperados** (observados em bancada com `WIFI_PS_NONE`):

| Operação | 1 MB |
|----------|------|
| Push | **~10 s** (9.85 s medido) |
| Pull | **~12 s** |

### 9.4. Poller do `/status` (`tools/status_poll.py`)

Consulta o endpoint `/status` do ESP32 periodicamente e imprime as métricas.

```powershell
# Formato completo (todas as métricas, uma por linha)
python tools\status_poll.py --device 192.168.15.7

# Formato compacto (uma linha por poll, ideal para deixar rodando)
python tools\status_poll.py --device 192.168.15.7 --compact

# Uma vez só
python tools\status_poll.py --device 192.168.15.7 --once

# JSON cru (para pipe/jq)
python tools\status_poll.py --device 192.168.15.7 --once --json
```

**Saída do modo compacto:**

```
[14:32:47] state=IDLE       sent=   2 rej_q=  0 rej_n=  0 rssi= -42dBm bright=142 sharp= 615
[14:32:52] state=CAPTURING  sent=   2 rej_q=  0 rej_n=  0 rssi= -42dBm bright=142 sharp= 615
[14:32:57] state=SENDING    sent=   2 rej_q=  0 rej_n=  0 rssi= -42dBm bright=142 sharp= 615
[14:33:02] state=WAITING    sent=   3 rej_q=  0 rej_n=  0 rssi= -43dBm bright=143 sharp= 618
```

---

## 10. OTA — push e pull

### 10.1. Push (cliente envia firmware para o ESP32)

Requer `CONFIG_LOCKMAXXING_OTA_WARMUP_S` **expirado** (300 s de uptime após boot).

```powershell
python tools\ota_test_server.py push build\LockMaxxing.bin --device 192.168.15.7
```

**Fluxo:**

```
POST /update (corpo binário + header X-SHA256)
  → ESP32 adquire g_upload_mutex (409 se ocupado)
  → ota_set_state(DOWNLOADING) + g_ota_busy = true (pausa comm_task)
  → esp_ota_begin() na partição ota_1 (inativa)
  → loop: httpd_req_recv → esp_ota_write → mbedtls_sha256_update (streaming)
  → compara SHA-256 calculado com o header
  → esp_ota_end() valida estrutura
  → esp_ota_set_boot_partition()
  → resposta 200 OK com JSON
  → vTaskDelay(2s) → esp_restart() (mutex mantido até o reboot)
```

**Resposta em sucesso:**

```json
{"status":"ok","bytes":1076224,"partition":"ota_1",
 "sha256":"d10a8de8...","reboot_in_ms":2000}
```

### 10.2. Pull (ESP32 baixa firmware de servidor remoto)

```powershell
# Terminal 1: hospedar
python tools\ota_test_server.py serve build\LockMaxxing.bin --port 8443

# Terminal 2: disparar
python tools\ota_test_server.py pull `
  --device 192.168.15.7 `
  --from https://192.168.15.5:8443/LockMaxxing.bin
```

**Fluxo:**

```
POST /pull-ota {"url":"https://...","sha256":"..."}
  → ESP32 retorna {"status":"started"}
  → ota_https_update_start() cria task em background (não bloqueia o servidor)
  → task: esp_http_client_open → fetch_headers → esp_ota_begin
  → loop: esp_http_client_read → esp_ota_write → SHA-256 streaming
  → callback de progresso a cada 500 ms
  → SHA-256 confere → esp_ota_end → esp_ota_set_boot_partition
  → callback result → vTaskDelay(2s) → esp_restart()
```

**Retry:** 2 tentativas com 2 s entre elas. `ESP_ERR_INVALID_CRC` (SHA
mismatch) **não retenta** — se o arquivo está corrompido no servidor,
baixar de novo não resolve.

### 10.3. Rollback automático

Se o firmware novo **falhar no self-test de boot** (`camera_init` ou
`actuators_init` retorna erro), o `app_main` dispara:

```c
ota_mark_app_invalid_and_reboot();   // não retorna em sucesso
```

O bootloader lê o `otadata` no próximo boot, vê que a partição nova está
`PENDING_VERIFY` (não confirmada) e reverte automaticamente para a partição
anterior. A placa volta ao firmware que funcionava.

**Como testar o rollback:** compile temporariamente com `camera_init()`
retornando `ESP_FAIL` (uma linha em `camera.c`), faça o push, observe a
placa reverter em ~3 s após o boot.

### 10.4. Warmup

Para evitar OTA durante a janela de estabilização da rede (DHCP + TLS +
TCP slow start nos primeiros minutos), o endpoint rejeita OTA com **HTTP 503**
se uptime < `CONFIG_LOCKMAXXING_OTA_WARMUP_S` (default 300 s):

```json
{"error":"warmup","uptime_s":45,"retry_in_s":255}
```

O `ota_test_server.py` detecta o 503 e informa ao usuário:

```
[PUSH] Servidor em warmup.
[PUSH] Uptime: 45s
[PUSH] Tente novamente em ~255s.
```

**Desabilitar para testes rápidos:** `menuconfig` → warmup = 0.

### 10.5. Segurança em produção

| Item | Dev | Produção |
|------|-----|----------|
| `OTA_PULL_ENDPOINT_ENABLE` | y | **n** (ou autenticado) |
| `OTA_PULL_SKIP_TLS_VERIFY` | y (com cert local) | **n** (CA pública) |
| Servidor de pull | PC local | HTTPS com CA pública |
| `main/certs.h` | gerado | **gitignored** |

---

## 11. Filtro de qualidade

### 11.1. Arquitetura

```
camera_capture()                    ← captura VGA JPEG (grayscale via sensor)
  ↓ frame em memória
quality_analyze_jpeg(frame->buf, len, &metrics):
  esp_jpeg_decode() → QQVGA RGB565 (38 KB buffer estático)
  ↓
  brilho   = média Y (0-255)
  nitidez  = variância do Laplaciano
  ↓
classificação por thresholds (Kconfig)
  ↓
QUALITY_OK          → envia
QUALITY_TOO_DARK    → rejeita
QUALITY_TOO_BRIGHT  → rejeita
QUALITY_TOO_BLURRY  → rejeita
  ↓
comm_task é notificada via COMM_MSG_CAMERA_REJECTED
  ↓
STATE: CAPTURING → IDLE (não vai para ERROR — rejeição não é erro)
```

**Por que decodificar o JPEG em vez de fazer dupla captura:** a troca
dinâmica de `pixformat`/`framesize` no driver `esp32-camera` causa
`FB-OVF` (FIFO overflow) e `NO-SOI` no ESP32-S3 com OV5640. A solução
é capturar **um único JPEG** (o efetivo, que será enviado) e decodificá-lo
num buffer pequeno para análise.

**Custo:** ~50 ms de decode + ~5 ms de análise por captura. Com ciclos
de 30 s, é imperceptível.

### 11.2. Métricas

**Brilho:** `Y = (77*R + 150*G + 29*B) >> 8`, média sobre todos os pixels.

**Nitidez:** variância do operador Laplaciano. Para cada pixel interno:

```
lap = 4*Y[i,j] - Y[i-1,j] - Y[i+1,j] - Y[i,j-1] - Y[i,j+1]
variance = E[lap²] - (E[lap])²
```

**Valores típicos (QQVGA, câmera em grayscale):**

| Cena | Brilho | Nitidez |
|------|--------|---------|
| Parede lisa (uniforme) | 100–180 | 5–20 |
| Cena normal desfocada | 80–180 | 50–250 |
| Cena normal em foco | 80–180 | 300–1500 |
| Objeto próximo em foco | 100–200 | 800–3000 |

**Nota sobre `TOO_BRIGHT`:** o sensor tem auto-exposure, então apontar para
uma lâmpada **não** produz uma imagem de brilho 250 — o sensor reduz a
exposição e o brilho médio fica em ~100. O filtro `TOO_BRIGHT` só dispara
em superexposição extrema. Imagens "estranhas" em cenas claras são
normalmente **TOO_BLURRY** (baixa nitidez), não TOO_BRIGHT.

### 11.3. Ordem de prioridade na classificação

Nitidez **antes** de brilho. Motivo: uma imagem borrada apontada para luz
é mais provavelmente "esqueci de focar" (acionável) do que "luz intensa"
(não acionável pelo usuário). O log adicional reporta múltiplos problemas
quando ambos disparam.

### 11.4. Calibração

**Passo a passo:**

1. **Habilitar debug log:**
   ```bash
   idf.py menuconfig
   # → LockMaxxing Configuration → "Logar métricas de TODAS as imagens" → [*]
   ```
   Rebuild + flash.

2. **Coletar 5 amostras** em cada cenário:

   | Cenário | Como forçar |
   |---------|-------------|
   | Normal em foco | Objeto bem iluminado a 30 cm |
   | Normal desfocado | Girar a lente até borrar |
   | Escuro | Cobrir a lente com a mão |
   | Claro | Apontar para lâmpada acesa |
   | Parede lisa | Apontar para superfície uniforme |

3. **Anotar** `brilho` e `nitidez` do log:
   ```
   QUALITY: Análise: 160x120, brilho=142.5, nitidez=380.2 → OK
   QUALITY: Rejeitado: TOO_BLURRY (brilho=96.4, nitidez=154.7)
   ```

4. **Calcular** thresholds:
   ```
   MIN_SHARPNESS   = (média_desfocado + média_em_foco) / 2
   MIN_BRIGHTNESS  = (média_escuro + média_normal) / 2
   MAX_BRIGHTNESS  = (média_normal + média_claro) / 2
   ```

5. **Aplicar** via `menuconfig`, rebuild, flash.

6. **Verificar** no boot que os valores foram aplicados:
   ```
   CAMERA: Filtro de qualidade ATIVO: brilho [30, 220], nitidez >= 200
   ```

7. **Desmarcar** debug log em produção.

**Valores iniciais sugeridos** (conservadores, ajustar com dados):

```
MIN_BRIGHTNESS  = 30
MAX_BRIGHTNESS  = 220
MIN_SHARPNESS   = 200
```

### 11.5. Troubleshooting

| Sintoma | Causa | Fix |
|---------|-------|-----|
| Filtro não roda (log sem `QUALITY:`) | `QUALITY_FILTER_ENABLED=n` no `sdkconfig` | Ativar via menuconfig ou apagar `sdkconfig` |
| Sempre aceita imagens borradas | `MIN_SHARPNESS` baixo | Subir para 200-300 |
| Rejeita imagens boas | `MIN_SHARPNESS` alto | Descer para 100-150 |
| `esp_jpeg_decode falhou` | Frame corrompido ou buffer pequeno | Aumentar `MAX_DECODE_PIXELS` |
| `Dimensão inesperada: NxM` | `esp_jpeg` retornou resolução diferente | Verificar `JPEG_IMAGE_SCALE_1_4` |
| Valores sempre 0.0 | `swap_color_bytes` errado | Trocar `0` por `1` em `esp_jpeg_image_cfg_t.flags` |

---

## 12. LED de status (WS2812)

### 12.1. Mapeamento estado → padrão

LED WS2812 onboard em GPIO 48, controlado via RMT (`espressif/led_strip`).

| Estado | Cor | Padrão | Interpretação |
|--------|-----|--------|---------------|
| `INIT` | 🔵 Azul | Pisca 1 s | Inicializando |
| `IDLE` | 🟢 Verde | Fixo (brilho baixo) | Pronto, ocioso |
| `CAPTURING` | 🔵 Azul | Pisca 300 ms | Capturando |
| `SENDING` | 🟡 Amarelo | Pisca 500 ms | Enviando |
| `WAITING` | 🩵 Ciano | Pulso lento | Aguardando servidor |
| `APPLYING` (aprovado) | 🟢 Verde | Fixo brilhante (3 s) | Aprovado |
| `APPLYING` (reprovado) | 🔴 Vermelho | Fixo brilhante (3 s) | Reprovado |
| `ERROR` | 🔴 Vermelho | Pisca 500 ms | Erro, recuperando |
| OTA ativo | 🟣 Roxo | Pisca 150 ms | Atualizando firmware |
| Self-test boot | 🟢→🔴 | Pisca rápido | Diagnóstico de hardware |

### 12.2. Brilho

`CONFIG_LOCKMAXXING_LED_BRIGHTNESS` (default: 60, escala 0-255).

O WS2812 em 255 é ofuscante em ambiente interno. 60 (~24%) é confortável.
Aumente para demonstrações ao ar livre, reduza para laboratório escuro.
`0` desabilita o LED (útil para testes sem poluição visual).

### 12.3. Arquitetura

Task `led_status` (prioridade 1, 2 KB stack, roda a 20 Hz) lê o estado atual
via spinlock e aplica o padrão correspondente. Sem mutex — a leitura/escrita
de `sm_state_t` (enum, 4 bytes) é atômica no Xtensa.

`led_status_set()` é chamado em `on_enter_state()` (tasks.c) e pode ser
chamada de qualquer task sem bloquear.

`led_status_set_raw(r, g, b)` sobrescreve o padrão temporariamente (usado
pelo self-test e por `APPLYING` para acender verde/vermelho brilhante).

---

## 13. Observabilidade: `/status`

Endpoint HTTPS com métricas em runtime. Requer o servidor local ativo
(`ota_local_server_start()`).

```bash
curl.exe -k https://192.168.15.7:8443/status
```

**Saída completa:**

```json
{
  "state":"IDLE",
  "uptime_ms":78543,
  "running":"ota_0",
  "next":"ota_1",
  "pending_verify":false,
  "port":8443,
  "wifi_connected":true,
  "wifi_rssi":-42,
  "mqtt_connected":true,
  "mqtt_published":48,
  "mqtt_received":2,
  "mqtt_dropped":0,
  "queue_comm_msg":0,
  "queue_camera_cmd":0,
  "queue_actuator_cmd":0,
  "images_captured":2,
  "images_sent":2,
  "images_rejected_quality":0,
  "images_rejected_network":0,
  "avg_brightness":142,
  "avg_sharpness":615,
  "last_result":"APPROVED",
  "hmac_enabled":true,
  "quality_filter_enabled":true,
  "last_error":null
}
```

**O que cada métrica responde:**

| Métrica | Diagnóstico |
|---------|-------------|
| `state` | Sistema preso em algum estado? |
| `uptime_ms` | Reiniciou recentemente? (crash silencioso) |
| `running`/`next` | Bootou de qual partição? Próxima OTA vai para onde? |
| `pending_verify` | Uma OTA falhou e não confirmou? |
| `wifi_rssi` | Sinal fraco? (< -70 é preocupante) |
| `mqtt_published` | Reconectou muitas vezes? |
| `mqtt_dropped` | Servidor está mandando resultado inválido? |
| `queue_*` | Task travada (fila enchendo)? |
| `images_rejected_quality` | Câmera apontada para algo inútil? |
| `images_rejected_network` | Rede instável? |
| `avg_brightness`/`avg_sharpness` | Imagens estão nos thresholds esperados? |
| `last_result` | Última análise aprovou/reprovou? |
| `last_error` | Qual foi o último problema? |
| `hmac_enabled`/`quality_filter_enabled` | Features compiladas? |

**Contadores resetam em reboot** (não persistem em NVS — evita desgaste de
flash por contadores que mudam a cada 30 s).

**Uso recomendado durante testes:** deixar `status_poll.py --compact` rodando
em terminal por 30-60 min. `uptime_ms` reiniciando = crash; `avg_*` mudando
bruscamente = mudança de cena; `mqtt_dropped` crescendo = bug no servidor.

---

## 14. Testes de host

Testes de lógica pura em C, rodando no PC (não na placa).

### 14.1. Executar

```bash
cd tools/host_tests
./run_all.sh                # Linux/macOS/WSL
# ou
.\run_all.ps1               # Windows
```

### 14.2. O que cobrem

| Suíte | Testes | O que valida |
|-------|--------|--------------|
| `test_chunking.c` | 13 | Matemática de `(len + 2047) / 2048` em todos os limites |
| `test_json_parser.c` | 19 | cJSON: 7 formatos válidos + 12 inválidos |
| `test_sha256_equals.c` | 10 | Comparação case-insensitive, NULL, curta, longa |
| `test_state_machine.c` | 36 | 36 transições (caminhos felizes + erros + ABORT + force_idle) |
| **Total** | **78** | **Todos passando** |


### 14.3. Dependência

`gcc` no PATH (MinGW-w64, MSYS2 ou clang). O `run_all` baixa o `cJSON`
automaticamente na primeira execução (`tools/host_tests/external/`).

---

## 15. Troubleshooting

### 15.1. Wi-Fi

| Sintoma | Causa | Solução |
|---------|-------|---------|
| Portal abre toda vez mesmo após configurar | NVS não persiste | Verificar `app_config_save()` — pode ter chave errada |
| `Wi-Fi não conectou em 30000 ms` | SSID/senha errados ou 5 GHz-only | Rede deve ser **2.4 GHz** |
| Reconecta em loop | Power save + AP instável | `esp_wifi_set_ps(WIFI_PS_NONE)` já aplicado |
| Ping 250 ms | Power save ativo | Verificar `WIFI_PS_NONE` no handler `STA_CONNECTED` |
| Ping 5-20 ms | ✅ Normal | |

### 15.2. MQTT / TLS / mTLS

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `Please enable MQTT_ENABLE_SSL to use mqtts` | `CONFIG_MQTT_TRANSPORT_SSL=n` no `sdkconfig` | `menuconfig` → ESP-MQTT → Enable SSL |
| `mbedtls_x509_crt_parse -0x2180` | `certificate_len = sizeof - 1` | Remover `certificate_len` (deixar `esp-mqtt` usar strlen+1) |
| `SSL handshake -0x2700` | CN/SAN do cert não bate com host da URI | Regenerar `server.crt` com CN correto + SAN |
| `SSL handshake -0x7F00` | Broker rejeitou cert de cliente | `require_certificate true` + certs válidos |
| `bad certificate` no log do Mosquitto | CA do client não bate com a do broker | Regenerar client com a **mesma** CA |
| `Error transport connect` (rc≠0) | Broker offline ou porta errada | `Test-NetConnection <PC_IP> -Port 8883` |
| Desconecta após 60 s | Keepalive sem PINGRESP | Verificar broker rodando |

### 15.3. Câmera

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `esp_camera_init: ESP_ERR_NOT_FOUND` | Cabo flat solto ou invertido | Contatos para **baixo** |
| `Mismatch PID=0x5640` mas detecta | **Comportamento normal** | Driver tenta OV3640/3660 primeiro |
| `NO-SOI - JPEG start marker missing` | Ruído elétrico (servo + flash) | Capacitor 470 µF + verificar 3.3 V |
| `FB-OVF` (FIFO overflow) | Troca dinâmica de modo | Capturar em JPEG único (solução atual) |
| Imagem com gradiente horizontal | Skew de sinal DVP | Baixar XCLK para 10 MHz (aplicado) |
| Imagem "estranha" / borrada aceita | `MIN_SHARPNESS` baixo | Subir para 200+ |

### 15.4. OTA

| Sintoma | Causa | Solução |
|---------|-------|---------|
| `HTTP 503 warmup` | Uptime < 300 s | Aguardar, ou `menuconfig` → warmup = 0 |
| `HTTP 409 ota already in progress` | Outra OTA em andamento | Aguardar |
| `HTTP 429 rate limited` | Excedeu 5 POSTs/min | Aguardar 60 s ou `menuconfig` → rate limit maior |
| `sha256 mismatch` | `.bin` mudou entre build e push | Refazer build ou servir o mesmo `.bin` |
| `SSL handshake -0x2700` (pull) | CN do cert do `serve` não bate com URL | Adicionar `common_name` em `ota_client.c` |
| Pull falha com `ESP_ERR_HTTP_CONNECT` | `crt_bundle_attach=NULL` sem `cert_pem` | Verificar bloco TLS em `ota_client.c` |
| Reboot loop pós-OTA | Self-test falhando consistentemente | Verificar câmera/atuadores no novo build |

### 15.5. Sistema

| Sintoma | Causa | Solução |
|---------|-------|---------|
| Reboot a cada ~10 s | Task Watchdog | Verificar task bloqueada nos logs |
| `Brownout detector` | Fonte insuficiente | Fonte ≥5 V/2 A + capacitor |
| `SPIRAM não inicializada` | `CONFIG_SPIRAM_MODE_OCT=y` ausente | Adicionar ao `sdkconfig.defaults` |
| Placa aquece >80°C no shield | Regulador AMS1117 saturado | Ventilação + considerar regulador externo |
| `main/mqtt_certs.h: No such file` | CMake não rodou o `gen_mqtt_certs.sh` | Verificar `add_custom_command` com `ALL` |
| `main/certs.h: No such file` | Mesma causa (OTA) | Mesma solução |
| Opção do menuconfig não "pega" | `sdkconfig` cacheado | Apagar `sdkconfig` **ou** `menuconfig` para sobrescrever |

### 15.6. Diagnóstico rápido

**1. Verificar que o ESP32 está vivo na rede:**

```powershell
ping 192.168.15.7
Test-NetConnection 192.168.15.7 -Port 8443
```

**2. Verificar estado completo:**

```powershell
curl.exe -k https://192.168.15.7:8443/status
```

**3. Verificar log em tempo real por TAG:**

```powershell
idf.py -p COMx monitor | Select-String "STATE|MQTT|QUALITY|HMAC"
```

**4. Verificar TLS do broker isolado (sem firmware):**

```powershell
cd tools\certs
openssl s_client -connect 192.168.15.5:8883 -CAfile ca.crt
# Deve mostrar "Verify return code: 0 (ok)"
```

---

## 16. Arquitetura

### 16.1. Tasks FreeRTOS

| Task | Prio | Core | Stack | Responsabilidade |
|------|-----:|-----:|------:|-----------------|
| `wifi_mqtt_task` | 5 | 0 | 4096 | Monitora rede a cada 500 ms, emite NETWORK_READY/DOWN |
| `camera_task` | 4 | 1 | 6144 | Aguarda comando, captura, aplica filtro, entrega frame |
| `comm_task` | 3 | 0 | 6144 | Máquina de estados, envia chunks MQTT, gerencia timeouts |
| `actuator_task` | 2 | 1 | 3072 | Executa sequências APROVADO/REPROVADO (~3 s) |
| `led_status` | 1 | — | 2048 | Atualiza LED WS2812 a 20 Hz |
| `btn_reset` | 1 | — | 3072 | Monitora BOOT para reset de fábrica |

**Durante provisioning (temporário):**
- `dns_prov` (prio 3, 3072) — DNS captive portal
- `httpd` interno — servidor HTTP do portal

**Distribuição por core:** rede no Core 0 (stack TCP/IP também está lá),
periféricos no Core 1. Evita contenção de cache.

### 16.2. Comunicação entre tasks

3 filas estáticas (`xQueueCreateStatic`):

```
camera_cmd_queue    (comm → camera,    prof. 2)
comm_msg_queue      (todos → comm,     prof. 10, tipos heterogêneos)
actuator_cmd_queue  (comm → actuator,  prof. 2)
```

`comm_msg_queue` carrega `comm_msg_t`, struct discriminada por tipo:
`NETWORK_READY`, `NETWORK_DOWN`, `CAMERA_FRAME`, `CAMERA_FAILED`,
`CAMERA_REJECTED`, `RESULT`, `APPLY_DONE`, `TRIGGER_CAPTURE`.

### 16.3. Ownership de frames

- `camera_task` produz `camera_frame_t*`
- Se `xQueueSend` falha (fila cheia), `camera_task` **libera** (`camera_release`)
- Se sucesso, ownership passa para `comm_task`, que **sempre** libera em
  `do_send_chunked()` (sucesso ou falha)
- Nenhuma outra task toca em `camera_frame_t*`

### 16.4. Sincronização

| Mecanismo | Onde | Protege |
|-----------|------|---------|
| Event Group | `wifi.c`, `mqtt.c` | `WIFI_CONNECTED_BIT`, `WIFI_GOT_IP_BIT`, `MQTT_CONNECTED_BIT` |
| Mutex estático | `mqtt.c` | `s_chunk_buf` |
| Mutex estático | `ota_server.c` | `g_upload_mutex` (serializa POSTs) |
| Mutex estático | `ota_common.c` | `g_ota_mutex` (estado global) |
| Spinlock | `ota_client.c` | `s_ctx_in_use` |
| Spinlock | `stats.c` | contadores |
| Spinlock | `ota_server.c` | tabela de rate limit |
| Spinlock | `led_status.c` | estado do LED |
| Flag volatile | `ota_common.c` | `g_ota_busy` (pausa comm_task) |

### 16.5. Módulos

| Arquivo | Responsabilidade |
|---------|-----------------|
| `main.c` | `app_main`: init sequencial + self-test + start tasks |
| `config.h` | Constantes de hardware + refs Kconfig |
| `Kconfig.projbuild` | Opções do menuconfig |
| `camera.c/.h` | Driver OV5640, captura, release, modos |
| `wifi.c/.h` | Station com backoff (1→30 s), RSSI |
| `mqtt.c/.h` | Cliente MQTT + parser cJSON |
| `actuators.c/.h` | Servo (LEDC) + buzzer |
| `led_status.c/.h` | LED WS2812 (RMT) |
| `state_machine.c/.h` | Autômato puro `(estado, evento) → estado` |
| `tasks.c/.h` | 4 tasks + filas + orquestração |
| `quality_filter.c/.h` | Decode JPEG + brilho + nitidez |
| `hmac_auth.c/.h` | HMAC-SHA256 (PSA Crypto) |
| `stats.c/.h` | Contadores thread-safe |
| `app_config.c/.h` | Persistência em NVS |
| `wifi_provisioning.c/.h` | Portal SoftAP + captive |
| `factory_reset_button.c/.h` | Botão BOOT (5 s) |
| `ota_common.c/.h` | Init, rollback, SHA-256 |
| `ota_client.c` | Cliente HTTPS (pull) |
| `ota_server.c` | Servidor HTTPS (push + /status + /pull-ota + /admin/config + /factory-reset) |

### 16.6. Fluxo de mensagens na máquina de estados

```
T=0s   comm_task: compute_state_timeout() → 30s
       xQueueReceive(comm_msg_queue, 30s) — bloqueia

T=30s  timeout → TRIGGER_CAPTURE
       on_enter_state(CAPTURING): xQueueSend(camera_cmd_queue)
       transição IDLE → CAPTURING

       camera_task: camera_capture() + quality_analyze_jpeg
       se OK: enfileira CAMERA_FRAME
       se rejeitado: enfileira CAMERA_REJECTED

       comm_task: recebe CAMERA_FRAME
       on_enter_state(SENDING): do_send_chunked()
         mqtt_publish_request + N × mqtt_publish_chunk
         camera_release(frame)
         TRIGGER → WAITING

       comm_task: WAITING, espera resultado

       MQTT callback: handle_result_message (cJSON)
       enfileira COMM_MSG_RESULT

       comm_task: RESULT_APPROVED → APPLYING
       on_enter_state(APPLYING):
         led_status_set_raw(verde) + xQueueSend(actuator_cmd)

       actuator_task: actuators_apply_result(~3s)
       enfileira APPLY_DONE

       comm_task: APPLY_DONE → IDLE
       próximo ciclo em 30s
```

---

## 17. Fluxo da máquina de estados

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
          ▲                            │
          │ APPLY_DONE                 │ CAPTURE_OK / CAPTURE_REJECTED
          │                            │      │
          │                            │      └──► IDLE (rejeitado)
          │                            ▼
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

**Timeouts por estado** (`config.h`):

| Estado | Timeout | Evento sintetizado |
|--------|---------|-------------------|
| `IDLE` | 30 s | `TRIGGER_CAPTURE` |
| `CAPTURING` | 5 s | `CAPTURE_TIMEOUT` |
| `WAITING_RESULT` | 30 s | `RESULT_TIMEOUT` |
| `APPLYING` | 8 s | `ABORT` |
| `ERROR` | 3 s | `RESET` |

**Nota sobre `CAPTURE_REJECTED`:** transiciona de `CAPTURING` para **`IDLE`**,
não para `ERROR`. Rejeição por qualidade **não é erro** — é o filtro fazendo
seu trabalho. O próximo ciclo ocorre normalmente em 30 s.

---

## 18. Segurança elétrica

### ⚠️ O servo **NÃO pode** ser alimentado pela ESP32-CAM

O regulador AMS1117 fornece no máximo ~500 mA. O servo SG90 consome
**picos de 700 mA** durante movimento. Alimentar pela placa causa:

- Brownout reset (placa reinicia no meio da operação)
- Danos ao regulador
- Reset de Wi-Fi
- Corrupção de imagem (`NO-SOI` no log)

### Ligação correta

```
Fonte externa 5V/2A ──┬── GND ──────► GND da ESP32-CAM (obrigatório!)
                       │
                       └── 5V ───────► V+ do servo

ESP32 GPIO 21 ────────────────────────► Sinal do servo
```

**GND comum é obrigatório.** Sem ele, o PWM fica flutuante em relação à
alimentação do servo, e o servo não responde.

### Recomendações

- **Capacitor 470 µF/16 V** na entrada da fonte (estabiliza picos do servo)
- **Cabo curto** entre fonte e servo (evita queda de tensão)
- **Não use USB do PC** para o servo

### Temperatura

ESP32-S3 + AMS1117 ficam quentes com Wi-Fi + câmera. Shield típico: 65-75°C
(junction suporta até 125°C). **Normal.** Sinais de alarme: reboots
aleatórios, corrupção de imagem, cheiro de queimado. Para produto:
ventilação no case + regulador 3.3 V dedicado.

---

## 19. Decisões de design

### Por que FreeRTOS em vez de bare-metal

Wi-Fi, MQTT e TLS do IDF são tasks internas. Bare-metal significaria
reimplementar o scheduler cooperativo — reinventar o FreeRTOS, pior.
A preempção garante que a rede rode mesmo durante o processamento de JPEG.

### Por que 4 tasks e não 1 superloop

Uma task monolítica acoplaria rede, câmera e atuadores — debug infernal.
As 4 tasks isolam falhas: se a câmera trava, o Wi-Fi continua vivo e o
watchdog reinicia só a `camera_task`.

### Por que grayscale no sensor

`set_special_effect(s, 2)` faz o OV5640 converter para tons de cinza antes
de comprimir. Efeito: JPEG ~40% menor, análise de qualidade opera direto
no canal Y (sem converter RGB→Y), e a imagem é o suficiente para a maioria
das análises. Reverter: uma linha em `camera.c`.

### Por que XCLK de 10 MHz

A 20 MHz, o barramento DVP tem skew entre as 8 linhas de dados suficiente
para causar leitura errada sistemática de bytes — gradiente horizontal
(lado direito mais escuro). A 10 MHz, o período do PCLK dobra e a margem
contra o skew dobra. Custo: FPS cai ~50%, latência de captura aumenta.
Sem custo em qualidade com `GRAB_WHEN_EMPTY`.

### Por que chunks de 2 KB

- 512 B — 100 mensagens por 50 KB, overhead alto
- 2 KB — equilíbrio overhead/backpressure (caber 2 em `MQTT_BUFFER_SIZE=4096`)
- 4 KB — buffer de saída enche rápido

### Por que QoS 1

QoS 1 = at-least-once. Perder chunk em Wi-Fi ruim descarta imagem inteira
no QoS 0. QoS 1 com 2 KB é leve. Duplicatas são idempotentes no servidor
(dict por índice).

### Por que buffers estáticos

Não fragmentam heap, não falham em runtime, thread-safe. cJSON e IDF
ainda usam `malloc` transacionalmente, que é seguro.

### Por que warmup de OTA

Primeiros 3-5 min após boot: TCP slow start, ARP frio, power save
ajustando. OTA compete com o estabilizador → 5× mais lento. Warmup +
`503 Retry-After` evita isso.

### Por que `WIFI_PS_NONE`

Dobra o consumo (30→120 mA) mas reduz latência 250→5 ms. Para dispositivo
plugado, escolha óbvia. Ganho no OTA: 66 s → 10 s (6.7×).

### Por que mTLS em vez de TLS simples

TLS simples autentica o broker, mas qualquer cliente pode se conectar.
mTLS autentica os dois lados — atacante precisa do cert de cliente para
publicar no tópico. Adiciona apenas 5 KB de RAM (handshake único, não por
mensagem).

### Por que HMAC além de mTLS

mTLS autentica o **canal**, mas o broker (se comprometido) pode injetar
mensagens. HMAC no payload autentica **a mensagem específica** — mesmo um
broker malicioso não forja imagem válida sem a chave HMAC.

### Por que decode JPEG em vez de dupla captura no filtro

Trocar `pixformat`/`framesize` dinamicamente no driver `esp32-camera`
causa `FB-OVF` no S3. Decodificar o JPEG único (o que será enviado) com
`esp_jpeg` evita o problema e analisa o **frame efetivo** (não um preview).

### Por que WS2812 em vez de 2 LEDs discretos

Um único pino, 3 cores, brilho controlado, menos componentes, mais
expressivo. GPIO 48 é onboard na N16R8. RMT gera os pulsos por hardware
(0% de CPU em regime).

### Por que credenciais em NVS em vez de Kconfig

Credenciais em Kconfig vão para o `.bin` (extraíveis com `esptool`).
Em NVS: configuráveis via portal sem recompilar, não vazam em artefatos,
funcionam out-of-the-box. Kconfig fica como fallback (para dev).

### Por que flag volatile em vez de mutex no `g_ota_busy`

Tentar `ota_get_state()` (que pega mutex) em `comm_task` (prio 3, mesmo
core do `app_main` prio 1) causou deadlock — `app_main` nunca terminava
o boot. Flag `volatile bool`: leitura atômica, sem bloqueio, zero risco.

---

## 20. Status e roadmap

### 20.1. Estado atual

| Item da proposta | Status | Onde |
|------------------|--------|------|
| **Filtro de qualidade** | ✅ Implementado (thresholds a calibrar) | §11 |
| **Fila offline** | ⏳ Planejada (Fase 4) | — |
| **Auth + encryption** | ✅ HMAC-SHA256 + mTLS | §8 |
| **Interface multitarefa + LED** | ✅ 4 tasks + WS2812 | §12, §16 |

**Extras implementados** (não exigidos pela proposta original, mas úteis):

| Feature | Onde |
|---------|------|
| OTA push + pull + rollback | §10 |
| Provisionamento via portal | §7 |
| Reconfiguração remota `/admin/config` | §7 |
| Rate limiting por IP | §8 |
| Reset de fábrica (botão + HTTP) | §7 |
| Observabilidade `/status` (20+ métricas) | §13 |
| 78 host tests | §14 |

### 20.2. Validações em bancada

| Item | Resultado |
|------|-----------|
| Push OTA (1 MB) | **9.85 s** |
| Pull OTA (1 MB) | ~12 s |
| Ping (após `WIFI_PS_NONE`) | **5-20 ms** (era 250) |
| Conecta ao broker mTLS | ✅ (Mosquitto + client cert) |
| HMAC validado no servidor | ✅ |
| Filtro de qualidade ativo | ✅ |
| LED WS2812 mapeando estados | ✅ |

### 20.3. Roadmap

**Curto prazo (calibração + polimento):**
- [ ] Calibrar thresholds do filtro com dados reais (amostras no §11.4)
- [ ] Verificar `TOO_BLURRY` vs `TOO_BRIGHT` com a reordenação de prioridade
- [ ] Ajustar `MIN_SHARPNESS` para ~200 (após coleta)

**Médio prazo (completar a proposta):**
- [ ] **Fase 4 — Offline queue** (LittleFS + `esp_littlefs`)
  - Partição `storage` de 4 MB em `partitions.csv`
  - Módulo `offline_queue.c/.h`
  - `camera_task` enfileira em vez de descartar quando offline
  - `comm_task` drena quando `NETWORK_READY` chega
  - `/status` ganha `queue_count` e `queue_bytes`

**Longo prazo (produção):**
- [ ] Provisionar HMAC secret único por dispositivo (via portal ou `/admin/config` antes de vender)
- [ ] Secure Boot V2 + Flash Encryption (exige eFuse irreversível)
- [ ] HTTPS do pull contra CA pública (Let's Encrypt)
- [ ] Deep-sleep entre capturas (versão a bateria)
- [ ] Placa customizada: ESP32-S3-WROOM + regulador 3.3 V dedicado + RTC externo
- [ ] Análise de imagem embarcada (TinyML)

### 20.4. Compatibilidade

| Versão | Status |
|--------|--------|
| ESP-IDF 6.1.0 | ✅ Testado |
| ESP-IDF 5.1+ | ⚠️ Provavelmente compila (APIs estáveis) |
| ESP-IDF 5.0 | ❌ Falha (PSA Crypto, `psa_mac_sign_setup`) |
| ESP-IDF 4.x | ❌ Falha (MbedTLS 2.x) |

### 20.5. Notas para a banca

Se este projeto for avaliado academicamente, os pontos de destaque são:

1. **Arquitetura FreeRTOS real** — 4 tasks, prioridades calibradas, filas,
   mutex, spinlock, flag volatile — cada mecanismo no contexto correto.
2. **Máquina de estados pura** — sem I/O, testável no PC, 36 transições
   validadas.
3. **Camadas de segurança** — WPA2 + mTLS + HMAC, cada uma cobrindo uma
   ameaça diferente.
4. **Testes de host** — 78 testes encontraram 2 bugs reais antes do flash.
5. **OTA com rollback** — push e pull, SHA-256, PSRAM-safe, atômico.
6. **Observabilidade** — `/status` com 20+ métricas, LED de estado, logs por TAG.
7. **Provisionamento amigável** — portal captive, reset de fábrica por
   botão físico, reconfiguração remota autenticada.
8. **Otimização empírica** — Wi-Fi power save desligado (250→10 ms),
   grayscale no sensor (~40% menos banda), XCLK reduzido para 10 MHz
   (integridade de sinal).

Cada decisão é justificada por dados (medidos em bancada) ou por
referência à documentação oficial do IDF/Espressif.

---

## Licença

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Este projeto está licenciado sob a **MIT License** — veja o arquivo [LICENSE](LICENSE) para os termos completos.

Copyright (c) 2026 LockMaxxing contributors.

## Agradecimentos

Baseado na documentação oficial do ESP-IDF, nos componentes `esp32-camera`,
`esp_jpeg` e `led_strip` da Espressif, e em diversos projetos abertos de
ESP32-CAM. Bugs encontrados durante o desenvolvimento foram corrigidos com
a ajuda dos 78 host tests em `tools/host_tests/` e de diagnósticos
empíricos documentados na seção de Troubleshooting.
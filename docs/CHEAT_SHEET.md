# LockMaxxing — Cheat Sheet

Referência rápida de todos os comandos usados no desenvolvimento e teste
do firmware ESP32-S3-CAM. Organizada por tarefa.

> **Convenção:**
> - `COMx` = porta serial da placa (ex: `COM5` no Windows, `/dev/ttyACM0` no Linux/macOS)
> - `<PC_IP>` = IP do PC que roda o broker (ex: `192.168.15.5`)
> - `<ESP_IP>` = IP do ESP32 (ex: `192.168.15.7`)
> - Todos os comandos assumem que o terminal está na raiz do projeto
> - Todos os comandos são realizados no PowerShell

---

## 1. Build & Flash

```powershell
# Primeira vez / após mudar sdkconfig.defaults
idf.py fullclean
idf.py build

# Builds subsequentes (só o que mudou)
idf.py build

# Flash
idf.py -p COMx flash

# Flash + monitor em sequência (o mais usado no dia a dia)
idf.py -p COMx flash monitor

# Só monitorar (placa já flashada)
idf.py -p COMx monitor

# Sair do monitor: Ctrl+] (fecha o monitor, placa continua rodando)

# Ver ocupação de flash e RAM
idf.py size
idf.py size-components

# Reconfigurar opções
idf.py menuconfig

# Regenerar sdkconfig a partir de sdkconfig.defaults (mantém config atual)
idf.py reconfigure

# Apagar tudo (firmware + NVS + partitions)
idf.py -p COMx erase-flash

# Apagar build e sdkconfig (reset completo do estado de build)
idf.py fullclean
Remove-Item sdkconfig
idf.py build
```

---

## 2. Verificação rápida do estado da placa

```powershell
# Ping (confirma que a placa está viva na rede)
ping <ESP_IP>

# Porta do servidor HTTPS de OTA
Test-NetConnection <ESP_IP> -Port 8443

# Estado completo (state, uptime, métricas, filas, RSSI, HMAC, etc.)
curl.exe -k https://<ESP_IP>:8443/status

# Status formatado (poller do projeto)
python tools\status_poll.py --device <ESP_IP>

# Poll compacto (uma linha por poll, útil para deixar rodando)
python tools\status_poll.py --device <ESP_IP> --compact

# Poll único (verificação pontual)
python tools\status_poll.py --device <ESP_IP> --once

# Poll em JSON (para pipe/jq/análise)
python tools\status_poll.py --device <ESP_IP> --once --json
```

---

## 3. Servidor MQTT de teste (análise simulada)

```powershell
# Terminal 1 — broker (deixar rodando)
# Windows: mosquitto rodando como serviço (já sobe no boot)
# Verificar:
Get-Service mosquitto
# Status deve ser "Running"

# Se não estiver rodando:
Start-Service mosquitto

# Terminal 2 — servidor de análise
python tools\mqtt_test_server.py `
  --broker <PC_IP> `
  --mode always-approve

# Modos de decisão:
#   always-approve  — sempre aprova (teste do servo + LED verde)
#   always-reject   — sempre rejeita (teste do buzzer + LED vermelho)
#   alternate       — aprova IDs pares, rejeita ímpares (teste de ambos)
#   random          — sorteia

# Com validação HMAC (mesmo segredo do menuconfig):
python tools\mqtt_test_server.py `
  --broker <PC_IP> `
  --mode alternate `
  --hmac-secret <SEU_HEX_DE_64_CHARS>

# Imagens recebidas são salvas em:
received_images\
```

---

## 4. OTA — Teste de atualização

```powershell
# PUSH (envia .bin para a placa)
python tools\ota_test_server.py push build\LockMaxxing.bin `
  --device <ESP_IP>

# PULL (placa baixa .bin de um servidor HTTPS)
# Terminal 1 — hospedar o .bin:
python tools\ota_test_server.py serve build\LockMaxxing.bin --port 8443

# Terminal 2 — disparar o pull:
python tools\ota_test_server.py pull `
  --device <ESP_IP> `
  --from https://<PC_IP>:8443/LockMaxxing.bin

# Utilitário: calcular SHA-256 de um .bin
python tools\ota_test_server.py sha256 build\LockMaxxing.bin

# Reset de fábrica via HTTP (apaga NVS, próximo boot vai pro portal)
curl.exe -k -X POST https://<ESP_IP>:8443/factory-reset `
  -H "X-Admin-Token: <TOKEN_DO_MENUCONFIG>"

# Reconfigurar URI do broker via HTTP (sem reflashar)
curl.exe -k -X POST https://<ESP_IP>:8443/admin/config `
  -H "X-Admin-Token: <TOKEN_DO_MENUCONFIG>" `
  -H "Content-Type: application/json" `
  -d '{\"broker\":\"mqtts://192.168.15.10:8883\"}'
```

---

## 5. Certificados TLS / mTLS

### Regenerar certificados (após mudar IP ou CN)

```powershell
cd tools\certs

# CA (autoridade certificadora) — só se estiver criando do zero
openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt `
  -subj "/C=BR/O=LockMaxxing/CN=LockMaxxing-CA"

# Servidor (CN = IP ou hostname do broker)
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr `
  -subj "/C=BR/O=LockMaxxing/CN=192.168.15.5"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key `
  -CAcreateserial -out server.crt -days 3650 `
  -extfile server.ext

# Cliente (ESP32, para mTLS)
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr `
  -subj "/C=BR/O=LockMaxxing/CN=esp32-camera-01"
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key `
  -CAcreateserial -out client.crt -days 3650 `
  -extfile client.ext

cd ..\..
```

### Arquivos de extensão (SAN)

**`tools/certs/server.ext`:**
```ini
subjectAltName = IP:<PC_IP>, DNS:localhost
```

**`tools/certs/client.ext`:**
```ini
subjectAltName = DNS:esp32-camera-01, IP:<ESP_IP>
```

### Verificar certificados

```powershell
cd tools\certs

# Cadeia de confiança
openssl verify -CAfile ca.crt server.crt
openssl verify -CAfile ca.crt client.crt

# Inspecionar
openssl x509 -in server.crt -noout -text

# Verificar SAN
openssl x509 -in server.crt -noout -text | Select-String "Subject Alternative Name" -Context 0,1
```

### Testar TLS do broker (linha de comando)

```powershell
cd tools\certs

# Teste completo de mTLS
mosquitto_sub -h <PC_IP> -p 8883 `
  --cafile ca.crt `
  --cert client.crt `
  --key client.key `
  -V mqttv311 `
  -t "test" -v

# Se conectar e ficar parado aguardando mensagens = OK
# Ctrl+C para sair

# Testar handshake TLS puro (sem MQTT)
openssl s_client -connect <PC_IP>:8883 -CAfile ca.crt
# Deve mostrar "Verify return code: 0 (ok)"
```

### Regenerar `mqtt_certs.h` (forçar)

```powershell
# O build gera automaticamente se detectar mudança em tools/certs/
# Para forçar:
Remove-Item main\mqtt_certs.h
idf.py build
```

---

## 6. Reset & Recuperação

```powershell
# Reset de fábrica via botão físico:
#   Segurar BOOT (GPIO0) por 5s durante operação normal
#   Apaga NVS e reinicia em modo portal

# Reset de fábrica via HTTP:
curl.exe -k -X POST https://<ESP_IP>:8443/factory-reset `
  -H "X-Admin-Token: <TOKEN>"

# Apagar tudo (NVS + firmware + OTA partitions):
idf.py -p COMx erase-flash
idf.py -p COMx flash

# Apagar apenas a NVS (mantém firmware):
# Use o botão físico (BOOT 5s) — mais prático

# Entrar no portal de provisionamento:
#   1. Reset de fábrica (BOOT 5s ou HTTP)
#   2. Conectar no Wi-Fi "LockMaxxing-Setup"
#   3. Abrir http://192.168.4.1
#   4. Configurar SSID + senha
#   5. Placa reinicia e conecta
```

---

## 7. Calibração do filtro de qualidade

```powershell
# 1. Habilitar debug log
idf.py menuconfig
# → LockMaxxing Configuration
# → "Logar métricas de TODAS as imagens" → [*]
# Salvar e sair

# 2. Rebuild + flash
idf.py build
idf.py -p COMx flash monitor

# 3. Capturar 5 amostras em cada cenário e anotar brilho/nitidez:
#    - Cena normal em foco (30-50 cm)
#    - Cena normal desfocada
#    - Cena escura (cobrir lente)
#    - Cena clara (apontar para luz)
#    - Parede lisa (sem textura)

# Log será:
#   QUALITY: Análise: 160x120, brilho=142.5, nitidez=380.2 → OK
#   QUALITY: Rejeitado: TOO_BLURRY (brilho=96.4, nitidez=154.7)

# 4. Calcular thresholds:
#    MIN_SHARPNESS   = (média_desfocado + média_em_foco) / 2
#    MIN_BRIGHTNESS  = (média_escuro + média_normal) / 2
#    MAX_BRIGHTNESS  = (média_normal + média_claro) / 2

# 5. Aplicar via menuconfig
idf.py menuconfig
# → LockMaxxing Configuration → ajustar os 3 thresholds

# 6. Rebuild + flash
idf.py build
idf.py -p COMx flash monitor

# 7. Verificar no boot que os valores foram aplicados:
#    CAMERA: Filtro de qualidade ATIVO: brilho [30, 220], nitidez >= 200

# 8. Desabilitar debug log em produção
idf.py menuconfig
# → desmarcar "Logar métricas de TODAS as imagens"
```

### Valores típicos de referência

| Cena | Brilho | Nitidez |
|------|--------|---------|
| Parede lisa (uniforme) | 100–180 | 5–20 |
| Cena normal desfocada | 80–180 | 50–250 |
| Cena normal em foco | 80–180 | 300–1500 |
| Objeto próximo em foco | 100–200 | 800–3000 |

---

## 8. Logs e diagnóstico

```powershell
# Filtrar log do monitor por tag (rodar em outro terminal durante monitor)
# Exemplo: só mensagens de STATE
idf.py -p COMx monitor | Select-String "STATE"

# Ou salvar em arquivo para análise:
idf.py -p COMx monitor | Tee-Object -FilePath log.txt

# Ver logs de um módulo específico (tags do projeto):
#   APP, CAMERA, WIFI, MQTT, ACTUATOR, STATE, TASKS,
#   OTA_COMMON, OTA_CLIENT, OTA_SRV, WIFI_PROV, APP_CFG,
#   QUALITY, HMAC, LED, BTN_RESET, STATS

# Padrões importantes no log:
#   STATE: TRANSITION: X --[EVENT]--> Y   — mudança de estado
#   QUALITY: Análise: ... → OK            — filtro aprovou
#   QUALITY: Rejeitado: TOO_XXX           — filtro rejeitou
#   MQTT: Resultado recebido: id=N, APROVADO
#   OTA_CLIENT: SHA-256 confere com o esperado
#   OTA_SRV: OTA aplicada — reboot em 2000 ms
#   BTN_RESET: RESET DE FÁBRICA ACIONADO
```

---

## 9. Sobre o menuconfig

No terminal do ESP-IDF:

```powershell
idf.py menuconfig
```

**Localização das opções do projeto:**
- Menu raiz → **LockMaxxing Configuration**
  - Wi-Fi SSID / Password
  - MQTT Broker URI (`mqtts://IP:8883`)
  - MQTT Client ID
  - MQTT TLS Mode (0/1/2)
  - MQTT HMAC Secret
  - OTA Warmup (segundos)
  - Admin Token (`/admin/config`)
  - Rate Limit
  - Filtro de qualidade (enable + 3 thresholds + debug log)
  - LED WS2812 (enable + brightness)
  - Reset de fábrica (hold time)

**Opções do IDF que o projeto usa:**
- Component config → ESP-MQTT → **Enable MQTT over SSL** (deve estar `y`)
- Component config → ESP-TLS → **Allow insecure options** (deve estar `n`)
- Component config → ESP System Settings → CPU frequency (240 MHz para performance)
- Component config → FreeRTOS → Tick rate (1000 Hz)
- Component config → ESP PSRAM → Octal mode (obrigatório para a câmera)

**Depois de mudar**: `idf.py build && idf.py flash`

**Se uma mudança não pegar**: `idf.py fullclean && idf.py build` (o `sdkconfig` é regenerado do zero).

---

## 10. Troubleshooting — erros comuns

| Erro | Causa | Fix |
|------|-------|-----|
| `fatal error: mqtt_certs.h: No such file` | CMake não gerou o header | `idf.py fullclean && idf.py build` |
| `Please enable MQTT_ENABLE_SSL to use mqtts` | `CONFIG_MQTT_TRANSPORT_SSL=n` | `menuconfig` → ESP-MQTT → Enable MQTT over SSL |
| `mbedtls_x509_crt_parse returned -0x2180` | Comprimento do PEM sem `\0` | Remover `certificate_len` (deixar `esp-mqtt` calcular) |
| `SSL handshake -0x2700` | CN/SAN do cert não bate com host da URI | Regenerar `server.crt` com CN correto |
| `SSL handshake -0x7F00` | Broker rejeitou cert de cliente | Verificar `require_certificate true` + certs válidos |
| `httpd_req_recv failed` no OTA push | Wi-Fi lento (power save) | Confirmar `esp_wifi_set_ps(WIFI_PS_NONE)` |
| `NO-SOI - JPEG start marker missing` | Ruído elétrico na alimentação | Capacitor + verificar servo em fonte externa |
| `Wi-Fi não conectou em 30000 ms` | Credenciais erradas | Reset de fábrica + reprovisionar |
| `MQTT: Desconectado do broker` em loop | Broker offline ou URI errada | `Test-NetConnection <PC_IP> -Port 8883` |
| Portal fecha após 5min sem configurar | `PROVISIONING_TIMEOUT_S=300` | Reabrir com reset de fábrica |
| `Frame rejeitado: TOO_BLURRY` mas imagem boa | `MIN_SHARPNESS` alto demais | Baixar threshold |
| `Frame aprovado` mas imagem borrada | `MIN_SHARPNESS` baixo demais | Subir threshold |
| LED WS2812 não acende | `CONFIG_LOCKMAXXING_LED_ENABLED=n` | `menuconfig` → Enable LED |
| HMAC sempre inválido no servidor | Segredos diferentes | Conferir `--hmac-secret` no Python vs menuconfig |

---

## 11. Atalhos para desenvolvimento

```powershell
# Rebuild rápido + flash + monitor em um comando
idf.py build; if ($?) { idf.py -p COMx flash monitor }

# Só monitorar (placa já flashada)
idf.py -p COMx monitor

# Verificar o que mudou no git (antes de commitar)
git status
git diff

# Commit e push (após validar)
git add -A
git commit -m "descrição da mudança"
git push

# Verificar que credenciais NÃO serão commitadas
git status
# main/certs.h, main/mqtt_certs.h, tools/certs/, sdkconfig
# NÃO devem aparecer na lista
```

---

## 12. Estrutura do projeto

```
LockMaxxing/
├── main/
│   ├── main.c              — app_main, init sequencial
│   ├── config.h            — constantes de hardware + refs Kconfig
│   ├── Kconfig.projbuild   — opções do menuconfig
│   ├── CMakeLists.txt      — SRCS, REQUIRES, geração de headers
│   ├── camera.c/.h         — OV5640
│   ├── wifi.c/.h           — station com backoff
│   ├── mqtt.c/.h           — cliente MQTT + parser resultado
│   ├── actuators.c/.h      — servo + buzzer (LED via WS2812)
│   ├── led_status.c/.h     — LED WS2812 de status
│   ├── state_machine.c/.h  — autômato puro
│   ├── tasks.c/.h          — 4 tasks + orquestração
│   ├── quality_filter.c/.h — filtro de qualidade (decode JPEG)
│   ├── hmac_auth.c/.h      — HMAC-SHA256 (PSA Crypto)
│   ├── stats.c/.h          — contadores para /status
│   ├── app_config.c/.h     — persistência NVS
│   ├── wifi_provisioning.c/.h — portal SoftAP
│   ├── factory_reset_button.c/.h — botão BOOT
│   ├── ota_common.c/.h     — núcleo OTA
│   ├── ota_client.c        — cliente HTTPS (pull)
│   ├── ota_server.c        — servidor HTTPS (push + /status + /admin)
│   └── *.h                 — headers
├── tools/
│   ├── gen_cert.sh         — cert auto-assinado (HTTPS local)
│   ├── gen_mqtt_certs.sh   — certs mTLS (MQTT)
│   ├── mqtt_test_server.py — servidor de teste MQTT
│   ├── ota_test_server.py  — servidor de teste OTA
│   ├── status_poll.py      — poller do /status
│   ├── certs/              — certificados (GITIGNORED)
│   └── host_tests/         — 78 testes de lógica pura
├── partitions.csv          — nvs, otadata, ota_0, ota_1
├── sdkconfig.defaults      — config do IDF (target, PSRAM, MQTT)
├── README.md               — documentação principal
├── docs/
│   └── cheat_sheet.md      — este arquivo
└── .gitignore              — build/, sdkconfig, certs/, mqtt_certs.h
```

---

## 13. Workflow típico de desenvolvimento

```powershell
# 1. Editar código
code main\algum_modulo.c

# 2. Build
idf.py build

# 3. Flash
idf.py -p COMx flash monitor

# 4. Testar
#    - Verificar boot no monitor
#    - Aguardar ciclo completo (~30s)
#    - Conferir via /status

# 5. Se OK, commit
git add -A
git commit -m "feat: descrição"
git push

# 6. Se falhou, ver logs
#    - Monitor com filtro (Select-String)
#    - /status para métricas
#    - Log do Mosquitto (terminal onde roda mosquitto.exe -v)
```

---

## 14. Recuperação de desastres

### Placa não responde / boot loop

```powershell
# 1. Tentar entrar em download mode manualmente
#    - Segurar BOOT
#    - Apertar RESET (ou desconectar/reconectar USB mantendo BOOT)
#    - Soltar RESET, depois BOOT
# 2. Apagar tudo
idf.py -p COMx erase-flash
# 3. Reflashar
idf.py -p COMx flash monitor
```

### Corrupção de NVS (Wi-Fi não persiste)

```powershell
# Botão BOOT por 5s (mais fácil)
# Ou erase-flash completo:
idf.py -p COMx erase-flash
idf.py -p COMx flash monitor
```

### Broker MQTT não aceita mais o cliente

```powershell
# 1. Verificar certificados
cd tools\certs
openssl verify -CAfile ca.crt client.crt

# 2. Regenerar certs do cliente
# (rodar os comandos de "Regenerar certificados" acima)

# 3. Rebuild para regenerar mqtt_certs.h
cd ..\..
Remove-Item main\mqtt_certs.h
idf.py build
idf.py -p COMx flash monitor
```
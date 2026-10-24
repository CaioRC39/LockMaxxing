# =============================================================================
# CMakeLists.txt — Arquivo raiz do projeto ESP-IDF
# =============================================================================
#
# Este arquivo é o PONTO DE ENTRADA do sistema de build.
# Ele NÃO lista arquivos .c, NÃO define pinos, NÃO configura Wi-Fi.
# Ele apenas:
#   1. Exige uma versão mínima do CMake
#   2. Importa o sistema de build do ESP-IDF
#   3. Declara o nome do projeto
#
# Tudo o mais (target esp32s3, PSRAM, componentes, partições) é configurado
# em sdkconfig.defaults e sdkconfig, não aqui.
# =============================================================================

# Versão mínima do CMake exigida pelo ESP-IDF v5.x.
# Se seu CMake for mais antigo, o build falha com mensagem clara.
cmake_minimum_required(VERSION 3.16)

# Importa o "project.cmake" do ESP-IDF.
# A variável de ambiente IDF_PATH é definida pela extensão do VS Code
# ou pelo script export.sh / export.bat do ESP-IDF.
# Esse include carrega:
#   - a toolchain correta (xtensa-esp32s3-elf-gcc, se target = esp32s3)
#   - o sistema de descoberta de componentes (main/, components/, managed_components/)
#   - as regras para gerar bootloader, partition table e app .bin
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Declara o projeto. O nome "LockMaxxing" vira o nome do .bin final
# (ex: build/LockMaxxing.bin) e do .elf de debug.
# Não é o nome da pasta — pode ser diferente, mas manter igual é mais limpo.
project(LockMaxxing)
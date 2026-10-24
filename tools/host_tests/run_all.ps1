# =============================================================================
# tools/host_tests/run_all.ps1 — Compila e roda todos os testes de host
# =============================================================================

$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

try {
    $cc = if ($env:CC) { $env:CC } else { "gcc" }
    $cjsonTag = "v1.7.18"
    $cjsonDir = "external/cJSON"

    # Baixa cJSON na primeira execução
    if (-not (Test-Path "$cjsonDir/cJSON.c")) {
        Write-Host "[setup] Baixando cJSON $cjsonTag..."
        New-Item -ItemType Directory -Force -Path external | Out-Null

        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git) {
            git clone --depth 1 --branch $cjsonTag https://github.com/DaveGamble/cJSON.git $cjsonDir
        } else {
            New-Item -ItemType Directory -Force -Path $cjsonDir | Out-Null
            Invoke-WebRequest -Uri "https://raw.githubusercontent.com/DaveGamble/cJSON/$cjsonTag/cJSON.c" -OutFile "$cjsonDir/cJSON.c"
            Invoke-WebRequest -Uri "https://raw.githubusercontent.com/DaveGamble/cJSON/$cjsonTag/cJSON.h" -OutFile "$cjsonDir/cJSON.h"
        }
        Write-Host "[setup] cJSON pronto em $cjsonDir/"
    }

    $cflags = @("-Wall", "-Wextra", "-std=c11", "-O2",
                "-I../../main", "-I", "mocks", "-I", "external/cJSON")

    New-Item -ItemType Directory -Force -Path build | Out-Null

    $pass = 0
    $fail = 0
    $failedTests = @()

    Get-ChildItem test_*.c | ForEach-Object {
        $src = $_.FullName
        $name = $_.BaseName
        $exe = "build/$name.exe"

        Write-Host ""
        Write-Host "============================================================"
        Write-Host " Compilando: $name"
        Write-Host "============================================================"

        & $cc @cflags $src "mocks/mock_esp_err.c" "$cjsonDir/cJSON.c" -o $exe
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [ERRO] Compilação falhou: $name"
            $script:fail++
            $script:failedTests += "$name (compilação)"
            return
        }

        & $exe
        if ($LASTEXITCODE -eq 0) {
            $script:pass++
        } else {
            $script:fail++
            $script:failedTests += $name
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host " RESUMO: $pass passed, $fail failed"
    Write-Host "============================================================"

    if ($fail -gt 0) {
        $failedTests | ForEach-Object { Write-Host "  [FAIL] $_" }
        exit 1
    }
} finally {
    Pop-Location
}
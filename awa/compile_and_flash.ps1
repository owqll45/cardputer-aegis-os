# Compile and Flash Script for legacy_app
# Usage: ./compile_and_flash.ps1 [PORT]
# Example: ./compile_and_flash.ps1 COM3

param (
    [string]$Port = ""
)

Write-Host ">>> legacy_app Build System v4.2 <<<" -ForegroundColor Cyan

# Check for ESP-IDF
if (!(Get-Command "idf.py" -ErrorAction SilentlyContinue)) {
    Write-Host "Error: 'idf.py' not found in PATH." -ForegroundColor Red
    Write-Host "Please ensure ESP-IDF is installed and environment variables are set."
    Write-Host "Try running 'export.ps1' in your ESP-IDF directory first."
    exit 1
}

# Set Target
Write-Host ">>> Setting target to esp32s3..." -ForegroundColor Yellow
idf.py set-target esp32s3
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Build
Write-Host ">>> Building Firmware..." -ForegroundColor Yellow
idf.py build
if ($LASTEXITCODE -ne 0) { 
    Write-Host "Build Failed!" -ForegroundColor Red
    exit $LASTEXITCODE 
}

# Flash
if ($Port -eq "") {
    Write-Host ">>> Build Successful! To flash, run:" -ForegroundColor Green
    Write-Host "idf.py -p COMx flash monitor"
} else {
    Write-Host ">>> Flashing to $Port..." -ForegroundColor Yellow
    idf.py -p $Port flash monitor
}

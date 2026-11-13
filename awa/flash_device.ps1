$env:IDF_COMPONENT_MANAGER="0"
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.1"
& "$env:IDF_PATH\export.ps1"

Write-Host "Removing build directory..."
if (Test-Path "build") { Remove-Item -Path "build" -Recurse -Force }

if (!(Test-Path "managed_components")) { New-Item -ItemType Directory -Force -Path "managed_components" }

Write-Host "Setting target to esp32s3..."
idf.py set-target esp32s3
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building and Flashing..."
idf.py flash
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }                         
                
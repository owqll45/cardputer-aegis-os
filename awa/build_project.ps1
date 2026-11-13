# Android-like Crash Handling - 自动编译脚本
# 激活ESP-IDF环境并编译项目

Write-Host "=== Android-like Crash Handling - 自动编译脚本 ===" -ForegroundColor Cyan
Write-Host ""

# 设置项目根目录
$PROJECT_ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
Write-Host "项目根目录: $PROJECT_ROOT"
Write-Host ""

# 设置构建目录
$BUILD_DIR = Join-Path $PROJECT_ROOT "build_v3"
Write-Host "构建目录: $BUILD_DIR"
Write-Host ""

# 检查构建目录是否存在，不存在则创建
if (-not (Test-Path $BUILD_DIR)) {
    New-Item -ItemType Directory -Path $BUILD_DIR | Out-Null
    Write-Host "已创建构建目录"
    Write-Host ""
}

# 切换到构建目录
Set-Location $BUILD_DIR
Write-Host "当前目录: $(Get-Location)"
Write-Host ""

# 激活ESP-IDF环境
Write-Host "[1/4] 激活ESP-IDF环境..." -ForegroundColor Yellow
& "C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1"
Write-Host "✓ ESP-IDF环境已激活" -ForegroundColor Green
Write-Host ""

# 设置目标为esp32s3
Write-Host "[2/4] 设置目标为esp32s3..." -ForegroundColor Yellow
idf.py set-target esp32s3
Write-Host "✓ 目标已设置为esp32s3" -ForegroundColor Green
Write-Host ""

# 执行编译
Write-Host "[3/4] 执行编译..." -ForegroundColor Yellow
Write-Host "开始编译，请耐心等待..."
Write-Host ""
idf.py build

# 检查编译结果
if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "✓ 编译成功!" -ForegroundColor Green
    Write-Host "固件已生成在: $BUILD_DIR" -ForegroundColor Green
    Write-Host ""
    Write-Host "使用以下命令刷写固件:" -ForegroundColor Cyan
    Write-Host "  idf.py -p COMx flash monitor" -ForegroundColor Cyan
    Write-Host ""
} else {
    Write-Host ""
    Write-Host "✗ 编译失败" -ForegroundColor Red
    Write-Host "请检查编译日志，解决问题后重新运行此脚本" -ForegroundColor Red
    Write-Host ""
}

# 等待用户按任意键退出
Write-Host "按任意键退出..." -ForegroundColor Gray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

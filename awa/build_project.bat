@echo off
REM Android-like Crash Handling - 自动编译脚本
REM 激活ESP-IDF环境并编译项目

echo === Android-like Crash Handling - 自动编译脚本 ===
echo.

REM 设置项目根目录
set PROJECT_ROOT=%~dp0
echo 项目根目录: %PROJECT_ROOT%
echo.

REM 设置构建目录
set BUILD_DIR=%PROJECT_ROOT%build_v3
echo 构建目录: %BUILD_DIR%
echo.

REM 激活ESP-IDF环境
echo [1/4] 激活ESP-IDF环境...
call "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
echo ✓ ESP-IDF环境已激活
echo.

REM 切换到构建目录
echo [2/4] 切换到构建目录...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"
echo ✓ 当前目录: %CD%
echo.

REM 设置目标为esp32s3
echo [3/4] 设置目标为esp32s3...
idf.py set-target esp32s3
echo ✓ 目标已设置为esp32s3
echo.

REM 执行编译
echo [4/4] 执行编译...
echo 开始编译，请耐心等待...
echo.
idf.py build

REM 检查编译结果
if %ERRORLEVEL% == 0 (
    echo.
    echo ✓ 编译成功!
    echo 固件已生成在: %BUILD_DIR%
    echo.
    echo 使用以下命令刷写固件:
    echo   idf.py -p COMx flash monitor
    echo.
) else (
    echo.
    echo ✗ 编译失败
    echo 请检查编译日志，解决问题后重新运行此脚本
    echo.
)

pause

#!/usr/bin/env python3
"""
固件修复与编译脚本 - Android-like Crash Handling
1. 验证代码完整性
2. 配置 ESP-IDF 环境
3. 执行编译
"""

import os
import shutil
import subprocess
import sys

# 项目根目录
PROJECT_ROOT = os.path.abspath(".")

# ESP-IDF 配置（使用正确的双反斜杠路径）
ESP_IDF_PATH = "C:\\Espressif\\frameworks\\esp-idf-v5.5.1"
EXPORT_SCRIPT = os.path.join(ESP_IDF_PATH, "export.ps1")

# 构建目录
BUILD_DIR = os.path.join(PROJECT_ROOT, "build_v3")

def verify_code_integrity():
    """验证代码完整性，确保所有必要的文件都存在"""
    print("=== 验证代码完整性 ===")
    
    # 必要的文件列表
    required_files = [
        "main/hal/hal.h",
        "main/main.cpp",
        "main/crash_handler.h",
        "main/crash_handler.cpp",
        "main/apps/app_crash_test/app_crash_test.h",
        "main/apps/app_crash_test/app_crash_test.cpp"
    ]
    
    # 验证文件是否存在
    all_files_exist = True
    for file_path in required_files:
        full_path = os.path.join(PROJECT_ROOT, file_path)
        if os.path.exists(full_path):
            print(f"✓ {file_path} 存在")
        else:
            print(f"✗ {file_path} 不存在")
            all_files_exist = False
    
    print("=== 代码完整性验证完成 ===")
    return all_files_exist

def fix_python_dependencies():
    """修复 Python 依赖问题"""
    print("\n=== 修复 Python 依赖 ===")
    
    try:
        # 升级 pip
        subprocess.run([
            sys.executable, "-m", "pip", "install", "--upgrade", "pip"
        ], check=True, capture_output=True, text=True)
        
        # 安装必要的包
        subprocess.run([
            sys.executable, "-m", "pip", "install", "click", "esptool~=4.11.dev1", "esp-idf-diag"
        ], check=True, capture_output=True, text=True)
        
        print("✓ Python 依赖修复完成")
        return True
    except subprocess.CalledProcessError as e:
        print(f"✗ Python 依赖修复失败: {e}")
        print("继续编译，可能会遇到问题")
        return False

def compile_firmware():
    """配置 ESP-IDF 环境并执行编译"""
    print("\n=== 开始编译固件 ===")
    
    # 确保构建目录存在
    os.makedirs(BUILD_DIR, exist_ok=True)
    
    # 构建 PowerShell 命令链：先配置环境，再执行编译
    compile_script = f"""
    Write-Host '>>> 配置 ESP-IDF 环境...' -ForegroundColor Yellow
    $env:IDF_PATH = '{ESP_IDF_PATH}'
    
    Write-Host '>>> legacy_app Build System v4.2 <<<' -ForegroundColor Cyan
    
    # 切换到构建目录
    Set-Location -Path '{BUILD_DIR}'
    
    # 执行 cmake 配置
    Write-Host '>>> Running CMake...' -ForegroundColor Yellow
    cmake -G Ninja -DPYTHON_DEPS_CHECKED=1 ..
    if ($LASTEXITCODE -ne 0) {{ exit $LASTEXITCODE }}
    
    # 执行编译
    Write-Host '>>> Building Firmware...' -ForegroundColor Yellow
    ninja
    if ($LASTEXITCODE -ne 0) {{ 
        Write-Host 'Build Failed!' -ForegroundColor Red
        exit $LASTEXITCODE 
    }}
    
    Write-Host '>>> Build Successful!' -ForegroundColor Green
    Write-Host '>>> To flash, run: idf.py -p COMx flash monitor' -ForegroundColor Green
    """
    
    # 保存为临时 PowerShell 脚本
    temp_script = "temp_compile.ps1"
    with open(temp_script, 'w', encoding='utf-8') as f:
        f.write(compile_script)
    
    # 执行编译
    result = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", temp_script], 
                          cwd=PROJECT_ROOT, 
                          capture_output=True, 
                          text=True)
    
    # 清理临时脚本
    os.remove(temp_script)
    
    # 输出结果
    print(result.stdout)
    if result.stderr:
        print("错误输出:")
        print(result.stderr)
    
    if result.returncode == 0:
        print("\n=== 编译成功! ===")
        return True
    else:
        print(f"\n=== 编译失败，返回码: {result.returncode} ===")
        print("编译失败可能是由于 ESP-IDF 环境配置问题")
        print("请尝试手动配置 ESP-IDF 环境后重新编译")
        return False

def main():
    print("=== Android-like Crash Handling 编译工具 ===")
    print("本工具用于编译带有崩溃处理系统的固件\n")
    
    # 1. 验证代码完整性
    if not verify_code_integrity():
        print("\n❌ 代码完整性验证失败，部分文件缺失")
        return 1
    
    # 2. 修复 Python 依赖
    fix_python_dependencies()
    
    # 3. 执行编译
    if compile_firmware():
        print("\n✅ 编译成功! 固件已生成在 build_v3 目录")
        return 0
    else:
        print("\n❌ 编译失败")
        print("请检查 ESP-IDF 环境配置或手动编译")
        return 1

if __name__ == "__main__":
    sys.exit(main())

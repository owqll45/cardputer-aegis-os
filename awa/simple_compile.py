#!/usr/bin/env python3
"""
简单编译脚本 - Android-like Crash Handling
直接使用ESP-IDF的cmake和ninja命令编译项目
"""

import os
import subprocess
import sys

# 项目根目录
PROJECT_ROOT = os.path.abspath(".")

# ESP-IDF 配置
ESP_IDF_PATH = "C:\\Espressif\\frameworks\\esp-idf-v5.5.1"

# 构建目录
BUILD_DIR = os.path.join(PROJECT_ROOT, "build_v3")

def main():
    print("=== 简单编译脚本 - Android-like Crash Handling ===")
    print("直接使用ESP-IDF的cmake和ninja命令编译项目\n")
    
    # 1. 设置环境变量
    print("[1/5] 设置环境变量...")
    os.environ["IDF_PATH"] = ESP_IDF_PATH
    os.environ["PYTHON_DEPS_CHECKED"] = "1"  # 跳过Python依赖检查
    
    # 取消设置IDF_PYTHON_ENV_PATH，解决Python环境版本不匹配问题
    if "IDF_PYTHON_ENV_PATH" in os.environ:
        del os.environ["IDF_PYTHON_ENV_PATH"]
        print("✓ 已取消设置IDF_PYTHON_ENV_PATH环境变量")
    
    # 使用新版本的工具链
    new_toolchain_path = "C:\\Espressif\\tools\\xtensa-esp-elf\\esp-14.2.0_20241119\\xtensa-esp-elf\\bin"
    if os.path.exists(new_toolchain_path):
        # 将新版本工具链添加到PATH最前面
        os.environ["PATH"] = new_toolchain_path + ";" + os.environ["PATH"]
        print(f"✓ 将新版本工具链添加到PATH: {new_toolchain_path}")
    
    print(f"✓ 设置IDF_PATH: {ESP_IDF_PATH}")
    
    # 2. 创建构建目录（如果不存在）
    print("\n[2/5] 创建构建目录...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    print(f"✓ 构建目录: {BUILD_DIR}")
    
    # 3. 执行cmake配置
    print("\n[3/5] 执行cmake配置...")
    cmake_cmd = [
        "cmake", 
        "-G", "Ninja", 
        "-DIDF_TARGET=esp32s3", 
        "-DPYTHON_DEPS_CHECKED=1", 
        ".."
    ]
    
    try:
        result = subprocess.run(cmake_cmd, 
                              cwd=BUILD_DIR, 
                              capture_output=True, 
                              text=True, 
                              encoding="utf-8")
        
        if result.returncode == 0:
            print("✓ CMake配置成功")
        else:
            print("✗ CMake配置失败")
            print("错误输出:")
            print(result.stderr)
            print("标准输出:")
            print(result.stdout)
            return 1
    except Exception as e:
        print(f"✗ CMake配置失败: {e}")
        return 1
    
    # 4. 执行ninja编译
    print("\n[4/5] 执行ninja编译...")
    ninja_cmd = ["ninja"]
    
    try:
        result = subprocess.run(ninja_cmd, 
                              cwd=BUILD_DIR, 
                              capture_output=False,  # 实时输出编译日志
                              text=True, 
                              encoding="utf-8")
        
        if result.returncode == 0:
            print("✓ Ninja编译成功")
        else:
            print(f"✗ Ninja编译失败，返回码: {result.returncode}")
            return 1
    except Exception as e:
        print(f"✗ Ninja编译失败: {e}")
        return 1
    
    # 5. 验证编译结果
    print("\n[5/5] 验证编译结果...")
    
    # 检查关键文件是否生成
    key_files = [
        "mooncake.elf",
        "mooncake.bin",
        "bootloader/bootloader.bin",
        "partition-table.bin"
    ]
    
    all_files_exist = True
    for file_path in key_files:
        full_path = os.path.join(BUILD_DIR, file_path)
        if os.path.exists(full_path):
            print(f"✓ {file_path} 已生成")
        else:
            print(f"✗ {file_path} 未生成")
            all_files_exist = False
    
    if all_files_exist:
        print("\n✅ 编译成功! 固件已生成在 build_v3 目录")
        print("\n使用以下命令刷写固件:")
        print("  idf.py -p COMx flash monitor")
        print("或者使用M5Stack官方工具刷写 mooncake.bin 文件")
        return 0
    else:
        print("\n❌ 编译失败，关键文件未生成")
        return 1

if __name__ == "__main__":
    sys.exit(main())

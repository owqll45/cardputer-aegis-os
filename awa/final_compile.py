#!/usr/bin/env python3
"""
最终编译脚本 - Android-like Crash Handling
使用ESP-IDF自带的Python环境和完整的ESP-IDF工具链
"""

import os
import subprocess
import sys

# 项目根目录
PROJECT_ROOT = os.path.abspath(".")

# ESP-IDF 配置
ESP_IDF_PATH = "C:\\Espressif\\frameworks\\esp-idf-v5.5.1"
IDF_PYTHON_PATH = "C:\\Espressif\\tools\\idf-python\\3.11.2\\python.exe"

# 构建目录
BUILD_DIR = os.path.join(PROJECT_ROOT, "build_v3")

def main():
    print("=== 最终编译脚本 - Android-like Crash Handling ===")
    print("使用ESP-IDF自带的Python环境和完整的ESP-IDF工具链\n")
    
    # 1. 检查ESP-IDF Python环境是否存在
    print("[1/6] 检查ESP-IDF Python环境...")
    if not os.path.exists(IDF_PYTHON_PATH):
        print(f"✗ ESP-IDF Python环境不存在: {IDF_PYTHON_PATH}")
        return 1
    print(f"✓ ESP-IDF Python环境: {IDF_PYTHON_PATH}")
    
    # 2. 检查ESP-IDF路径是否存在
    print("\n[2/6] 检查ESP-IDF路径...")
    if not os.path.exists(ESP_IDF_PATH):
        print(f"✗ ESP-IDF路径不存在: {ESP_IDF_PATH}")
        return 1
    print(f"✓ ESP-IDF路径: {ESP_IDF_PATH}")
    
    # 3. 设置环境变量
    print("\n[3/6] 设置环境变量...")
    os.environ["IDF_PATH"] = ESP_IDF_PATH
    os.environ["PYTHON"] = IDF_PYTHON_PATH
    
    # 取消设置可能导致问题的环境变量
    if "IDF_PYTHON_ENV_PATH" in os.environ:
        del os.environ["IDF_PYTHON_ENV_PATH"]
        print("✓ 已取消设置IDF_PYTHON_ENV_PATH环境变量")
    
    if "VIRTUAL_ENV" in os.environ:
        del os.environ["VIRTUAL_ENV"]
        print("✓ 已取消设置VIRTUAL_ENV环境变量")
    
    print(f"✓ 设置IDF_PATH: {ESP_IDF_PATH}")
    print(f"✓ 设置PYTHON: {IDF_PYTHON_PATH}")
    
    # 4. 创建构建目录（如果不存在）
    print("\n[4/6] 创建构建目录...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    print(f"✓ 构建目录: {BUILD_DIR}")
    
    # 5. 安装ESP-IDF Python依赖
    print("\n[5/6] 安装ESP-IDF Python依赖...")
    install_deps_cmd = [
        IDF_PYTHON_PATH, "-m", "pip", "install", "--upgrade", "pip"
    ]
    
    try:
        subprocess.run(install_deps_cmd, 
                      cwd=PROJECT_ROOT, 
                      capture_output=True, 
                      text=True, 
                      encoding="utf-8")
        
        install_idf_deps_cmd = [
            IDF_PYTHON_PATH, "-m", "pip", "install", "-r", 
            os.path.join(ESP_IDF_PATH, "tools", "requirements", "requirements.core.txt")
        ]
        
        subprocess.run(install_idf_deps_cmd, 
                      cwd=PROJECT_ROOT, 
                      capture_output=True, 
                      text=True, 
                      encoding="utf-8")
        
        print("✓ ESP-IDF Python依赖安装完成")
    except Exception as e:
        print(f"✗ ESP-IDF Python依赖安装失败: {e}")
        print("继续编译，可能会遇到问题")
    
    # 6. 使用IDF-PYTHON运行idf.py build
    print("\n[6/6] 执行idf.py build...")
    idf_build_cmd = [
        IDF_PYTHON_PATH, 
        os.path.join(ESP_IDF_PATH, "tools", "idf.py"), 
        "build"
    ]
    
    try:
        result = subprocess.run(idf_build_cmd, 
                              cwd=BUILD_DIR, 
                              capture_output=False,  # 实时输出编译日志
                              text=True, 
                              encoding="utf-8")
        
        if result.returncode == 0:
            print("\n✓ 编译成功!")
            print(f"\n固件已生成在: {BUILD_DIR}")
            print("\n使用以下命令刷写固件:")
            print(f"  {IDF_PYTHON_PATH} {os.path.join(ESP_IDF_PATH, 'tools', 'idf.py')} -p COMx flash monitor")
            return 0
        else:
            print(f"\n✗ 编译失败，返回码: {result.returncode}")
            print("\n编译失败可能是由于ESP-IDF环境配置问题")
            print("请尝试以下解决方案:")
            print("1. 确保ESP-IDF工具链已正确安装")
            print("2. 运行ESP-IDF的install.bat脚本重新安装工具链")
            print("3. 检查网络连接，确保可以访问ESP-IDF组件仓库")
            return 1
    except Exception as e:
        print(f"\n✗ 编译失败: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())

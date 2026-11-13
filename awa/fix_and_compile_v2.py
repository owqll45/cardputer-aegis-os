#!/usr/bin/env python3
"""
固件修复与编译脚本
1. 备份原始文件
2. 替换为修复后的版本
3. 执行编译
"""

import os
import shutil
import subprocess

# 项目根目录
PROJECT_ROOT = os.path.abspath(".")

def backup_and_replace():
    """备份原始文件并替换为修复后的版本"""
    
    # 修复文件的位置
    fixed_files = {
        "app_launcher.h": "main/apps/app_launcher/app_launcher_fixed.h",
        "app_launcher.cpp": "main/apps/app_launcher/app_launcher_fixed.cpp",
        "system_bar.cpp": "main/apps/app_launcher/view/system_bar/system_bar_fixed.cpp",
        "menu.cpp": "main/apps/app_launcher/view/menu/menu_fixed.cpp",
        "keyboard_bar.cpp": "main/apps/app_launcher/view/keyboard_bar/keyboard_bar_fixed.cpp"
    }
    
    # 目标文件的位置
    target_files = {
        "app_launcher.h": "main/apps/app_launcher/app_launcher.h",
        "app_launcher.cpp": "main/apps/app_launcher/app_launcher.cpp",
        "system_bar.cpp": "main/apps/app_launcher/view/system_bar/system_bar.cpp",
        "menu.cpp": "main/apps/app_launcher/view/menu/menu.cpp",
        "keyboard_bar.cpp": "main/apps/app_launcher/view/keyboard_bar/keyboard_bar.cpp"
    }
    
    for name, fixed_path in fixed_files.items():
        fixed_full = os.path.join(PROJECT_ROOT, fixed_path)
        target_full = os.path.join(PROJECT_ROOT, target_files[name])
        
        # 检查修复后的文件是否存在
        if os.path.exists(fixed_full):
            # 备份原始文件
            backup_path = target_full + ".bak"
            if not os.path.exists(backup_path):
                shutil.copy2(target_full, backup_path)
                print(f"已备份: {target_files[name]} -> {target_files[name]}.bak")
            
            # 替换为修复后的文件
            shutil.copy2(fixed_full, target_full)
            print(f"已替换: {fixed_path} -> {target_files[name]}")
        else:
            print(f"跳过: 修复文件不存在 {fixed_path}")

def check_esp_idf():
    """检查 ESP-IDF 环境"""
    print("\n=== 检查 ESP-IDF 环境 ===")
    result = subprocess.run(["idf.py", "--version"], 
                          capture_output=True, 
                          text=True)
    
    if result.returncode == 0:
        print(f"ESP-IDF 版本: {result.stdout.strip()}")
        return True
    else:
        print("Error: 'idf.py' 未找到或 ESP-IDF 环境未配置")
        print("请确保 ESP-IDF 已安装并配置好环境变量")
        print("尝试运行 ESP-IDF 目录下的 'export.ps1' 或 'export.bat'")
        return False

def compile_firmware():
    """执行编译命令"""
    print("\n=== 开始编译固件 ===")
    
    # 检查 ESP-IDF 环境
    if not check_esp_idf():
        print("\n=== 跳过编译，环境未配置 ===")
        return False
    
    # 执行编译脚本
    result = subprocess.run(["powershell", "-File", "compile_and_flash.ps1"], 
                          cwd=".", 
                          capture_output=True, 
                          text=True)
    
    print(result.stdout)
    if result.stderr:
        print("错误输出:")
        print(result.stderr)
    
    if result.returncode == 0:
        print("\n=== 编译成功! ===")
        return True
    else:
        print(f"\n=== 编译失败，返回码: {result.returncode} ===")
        return False

def main():
    print("=== 固件修复与编译工具 ===")
    backup_and_replace()
    compile_firmware()

if __name__ == "__main__":
    main()

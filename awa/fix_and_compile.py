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

def backup_and_replace():
    """备份原始文件并替换为修复后的版本"""
    files_to_replace = [
        "main/apps/app_launcher/app_launcher.h",
        "main/apps/app_launcher/app_launcher.cpp",
        "main/apps/app_launcher/view/system_bar/system_bar.cpp",
        "main/apps/app_launcher/view/menu/menu.cpp",
        "main/apps/app_launcher/view/keyboard_bar/keyboard_bar.cpp"
    ]
    
    for file_path in files_to_replace:
        # 检查修复后的文件是否存在
        fixed_path = file_path + "_fixed"
        if os.path.exists(fixed_path):
            # 备份原始文件
            backup_path = file_path + ".bak"
            if not os.path.exists(backup_path):
                shutil.copy2(file_path, backup_path)
                print(f"已备份: {file_path} -> {backup_path}")
            
            # 替换为修复后的文件
            shutil.copy2(fixed_path, file_path)
            print(f"已替换: {fixed_path} -> {file_path}")
        else:
            print(f"跳过: 修复文件不存在 {fixed_path}")

def compile_firmware():
    """执行编译命令"""
    print("\n=== 开始编译固件 ===")
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
    else:
        print(f"\n=== 编译失败，返回码: {result.returncode} ===")

def main():
    print("=== 固件修复与编译工具 ===")
    backup_and_replace()
    compile_firmware()

if __name__ == "__main__":
    main()

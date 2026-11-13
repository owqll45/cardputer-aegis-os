#!/usr/bin/env python3

import os
import sys
import argparse
import subprocess
import serial.tools.list_ports
import time

# 固件文件路径 (假设脚本在 awa 目录下运行)
FIRMWARE_DIR = os.path.join(os.getcwd(), "build")
BOOTLOADER = os.path.join(FIRMWARE_DIR, "bootloader", "bootloader.bin")
PARTITION_TABLE = os.path.join(FIRMWARE_DIR, "partition_table", "partition-table.bin")
OTA_DATA = os.path.join(FIRMWARE_DIR, "ota_data_initial.bin")
APP_FIRMWARE = os.path.join(FIRMWARE_DIR, "cardputer-adv.bin")

# Flash地址映射
FLASH_ADDRESSES = {
    "bootloader": "0x0",
    "partition": "0x8000",
    "ota": "0xd000",
    "app": "0x10000"
}

def find_serial_port():
    print("[检测] 正在搜索可用串口...")
    M5_VID_PID = "303A:1001"
    
    ports = list(serial.tools.list_ports.comports())
    
    print(f"[调试] 找到 {len(ports)} 个可用端口:")
    for port in ports:
        print(f"[调试] 端口: {port.device}, 描述: {port.description}, HWID: {port.hwid}")
    
    for port in ports:
        if M5_VID_PID in port.hwid:
            print(f"[检测] 找到M5Stack设备端口: {port.device} (VID:PID匹配)")
            return port.device
    
    for port in ports:
        if "USB" in port.description or "USB" in port.hwid:
            print(f"[检测] 找到USB串口: {port.device}")
            return port.device
            
    if ports:
        print(f"[检测] 返回第一个可用端口: {ports[0].device}")
        return ports[0].device
    
    return None

def check_firmware_files():
    required_files = [
        (BOOTLOADER, "bootloader.bin"),
        (PARTITION_TABLE, "partition-table.bin"),
        (OTA_DATA, "ota_data_initial.bin"),
        (APP_FIRMWARE, "cardputer-adv.bin")
    ]
    
    missing = False
    for f, name in required_files:
        if not os.path.exists(f):
            print(f"[错误] 缺少文件: {name} ({f})")
            missing = True
    
    return not missing

def kill_blocking_processes(port):
    print(f"[警告] 端口 {port} 被占用，尝试终止占用进程...")
    # 尝试查找并终止可能占用端口的 python 进程 (主要是之前的 flash.py 或 miniterm)
    # 注意：这可能会误杀其他 python 进程，但在开发环境中通常可以接受
    try:
        # PowerShell 命令查找包含 miniterm 或 flash.py 的进程
        ps_cmd = "Get-WmiObject Win32_Process | Where-Object { ($_.CommandLine -like '*miniterm*') -or ($_.CommandLine -like '*flash.py*') } | Stop-Process -Force"
        subprocess.run(["powershell", "-Command", ps_cmd], capture_output=True)
        time.sleep(1) # 等待释放
    except Exception as e:
        print(f"[错误] 无法终止进程: {e}")

def run_esptool(args):
    # 使用 sys.executable 确保使用当前环境的 python
    cmd = [sys.executable, "-m", "esptool"] + args
    print(f"[执行] {' '.join(cmd)}")
    try:
        subprocess.run(cmd, check=True)
        return True
    except subprocess.CalledProcessError:
        print("[错误] esptool 执行失败")
        # 检查是否是因为端口占用
        return False

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["all", "flash", "monitor"])
    parser.add_argument("--port")
    args = parser.parse_args()
    
    if not check_firmware_files():
        sys.exit(1)
        
    port = args.port
    if not port:
        port = find_serial_port()
        
    if not port:
        print("[错误] 未找到串口")
        sys.exit(1)

    # 预检查端口是否被占用
    try:
        s = serial.Serial(port)
        s.close()
    except serial.SerialException:
        kill_blocking_processes(port)
        
    if args.command in ["flash", "all"]:
        print(f"[刷写] 目标端口: {port}")
        flash_args = [
            "--chip", "esp32s3",
            "--port", port,
            "--baud", "460800",
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash", "-z",
            "--flash_mode", "dio",
            "--flash_freq", "80m",
            "--flash_size", "8MB",
            FLASH_ADDRESSES["bootloader"], BOOTLOADER,
            FLASH_ADDRESSES["partition"], PARTITION_TABLE,
            FLASH_ADDRESSES["ota"], OTA_DATA,
            FLASH_ADDRESSES["app"], APP_FIRMWARE
        ]
        if not run_esptool(flash_args):
            sys.exit(1)
            
    if args.command in ["monitor", "all"]:
        print("[监视] 启动监视器 (Ctrl+C 退出)...")
        # 简单调用 miniterm
        try:
            subprocess.run([sys.executable, "-m", "serial.tools.miniterm", port, "115200"], check=True)
        except KeyboardInterrupt:
            pass
        except Exception as e:
            print(f"[错误] 监视失败: {e}")

if __name__ == "__main__":
    main()
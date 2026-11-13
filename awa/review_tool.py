#!/usr/bin/env python3
"""
代码审查工具 - 使用 gemini_bridge.py 进行代码审查
"""

import sys
import os

# 添加当前目录到 Python 路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 导入 gemini_bridge 模块
from gemini_bridge import ask_gemini

def main():
    # 读取要审查的代码
    with open('system_bar_code.txt', 'r', encoding='utf-8') as f:
        code = f.read()
    
    # 构建审查提示
    prompt = f"请帮我审查以下系统状态栏代码，检查是否有 bug、内存泄漏、逻辑错误或其他问题：\n\n{code}\n\n请详细审查这段代码，指出所有可能的问题、bug 和优化建议。"
    
    # 调用 gemini_bridge 进行审查
    result = ask_gemini(prompt)
    
    if result:
        print("=== 代码审查结果 ===")
        print(result)
    else:
        print("错误：未能获取审查结果")
        sys.exit(1)

if __name__ == "__main__":
    main()

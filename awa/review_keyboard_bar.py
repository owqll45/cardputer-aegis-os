#!/usr/bin/env python3
"""
代码审查工具 - 使用 gemini_bridge.py 进行代码审查
"""

import sys
import os

# 添加当前目录到 Python 路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 导入 gemini_bridge 模块
from gemini_bridge import review_code_with_gemini

def main():
    # 读取要审查的代码
    with open('keyboard_bar_code.txt', 'r', encoding='utf-8') as f:
        code = f.read()
    
    # 调用专门的代码审查函数
    result = review_code_with_gemini(code, "键盘栏代码")
    
    if result:
        print("=== 代码审查结果 ===")
        print(result)
    else:
        print("错误：未能获取审查结果")
        sys.exit(1)

if __name__ == "__main__":
    main()

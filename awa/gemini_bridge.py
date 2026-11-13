#!/usr/bin/env python3
"""
Gemini Bridge - Trae 与 Gemini 的通信桥梁
"""

import google.generativeai as genai
import sys
import json

def init_gemini():
    """初始化 Gemini 连接"""
    api_key = "REDACTED_API_KEY"
    api_endpoint = "http://127.0.0.1:8045"
    
    try:
        genai.configure(
            api_key=api_key,
            transport='rest',
            client_options={'api_endpoint': api_endpoint}
        )
        model = genai.GenerativeModel('gemini-3-pro-high')
        return model
    except Exception as e:
        print(f"ERROR: 初始化失败: {e}", file=sys.stderr)
        return None

def ask_gemini(prompt):
    """向 Gemini 提问并获取回答"""
    model = init_gemini()
    if not model:
        return None
    
    try:
        response = model.generate_content(prompt)
        return response.text
    except Exception as e:
        print(f"ERROR: 请求失败: {e}", file=sys.stderr)
        return None

def review_code_with_gemini(code, context=""):
    """让 Gemini 审查代码"""
    model = init_gemini()
    if not model:
        return None
    
    prompt = f"""请审查以下代码：

{context}

代码：
```python
{code}
```

审查要求：
1. 检查代码是否有语法错误
2. 检查代码是否有逻辑错误或潜在问题
3. 评估代码的可读性和可维护性
4. 提供优化建议（如果适用）
5. 给出整体评价和改进方向

如果代码有问题，请明确指出需要修改的地方。
如果代码没有问题，请明确说明"代码审查通过，没有问题"。
"""
    
    try:
        response = model.generate_content(prompt)
        return response.text
    except Exception as e:
        print(f"ERROR: 审查失败: {e}", file=sys.stderr)
        return None

if __name__ == "__main__":
    # 检查是否是代码审查模式
    if len(sys.argv) >= 3 and sys.argv[1] == "--review":
        # 读取文件内容进行审查
        file_path = sys.argv[2]
        with open(file_path, 'r', encoding='utf-8') as f:
            code = f.read()
        
        prompt = f"请帮我审查以下代码，检查是否有 bug、内存泄漏、逻辑错误或其他问题：\n\n{code}\n\n请详细审查这段代码，指出所有可能的问题、bug 和优化建议。"
    else:
        # 普通模式：直接使用命令行参数作为 prompt
        if len(sys.argv) < 2:
            print("用法: python gemini_bridge.py <prompt> 或 python gemini_bridge.py --review <file_path>", file=sys.stderr)
            sys.exit(1)
        
        prompt = ' '.join(sys.argv[1:])
    
    result = ask_gemini(prompt)
    
    if result:
        print(result)
    else:
        print("ERROR: 未能获取响应", file=sys.stderr)
        sys.exit(1)

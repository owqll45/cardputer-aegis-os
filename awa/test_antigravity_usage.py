#!/usr/bin/env python3
"""
Antigravity Manager 使用测试脚本
"""

# 需要安装: pip install google-generativeai 
import google.generativeai as genai 

# 使用 Antigravity 代理地址 (推荐 127.0.0.1) 
genai.configure( 
    api_key="REDACTED_API_KEY", 
    transport='rest', 
    client_options={'api_endpoint': 'http://127.0.0.1:8045'} 
) 

model = genai.GenerativeModel('gemini-3-pro-high') 
response = model.generate_content("Hello") 
print(response.text)

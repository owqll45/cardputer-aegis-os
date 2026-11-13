#!/usr/bin/env python3
"""
Antigravity Manager 使用脚本
用于在 TRAECN 应用中使用 Gemini Pro 服务
"""

import google.generativeai as genai

def configure_gemini():
    """配置 Gemini 客户端"""
    print("正在配置 Gemini 客户端...")
    
    try:
        # 使用 Antigravity 代理配置
        genai.configure(
            api_key="REDACTED_API_KEY",
            transport='rest',
            client_options={'api_endpoint': 'http://127.0.0.1:8045'}
        )
        
        print("✅ 配置完成！")
        return True
    except Exception as e:
        print(f"❌ 配置失败: {e}")
        return False

def test_connection():
    """测试连接是否正常"""
    print("正在测试连接...")
    
    try:
        # 创建模型实例
        model = genai.GenerativeModel('gemini-3-flash')
        
        # 发送测试请求
        response = model.generate_content("你好，测试连接")
        
        print("✅ 连接成功！")
        print(f"响应: {response.text}")
        return True
    except Exception as e:
        print(f"❌ 连接失败: {e}")
        return False

def generate_content(prompt):
    """生成内容"""
    print(f"正在生成内容: {prompt}")
    
    try:
        # 创建模型实例
        model = genai.GenerativeModel('gemini-3-flash')
        
        # 发送请求
        response = model.generate_content(prompt)
        
        print("✅ 生成成功！")
        print(f"响应: {response.text}")
        return response.text
    except Exception as e:
        print(f"❌ 生成失败: {e}")
        return None

def main():
    """主函数"""
    print("=== Antigravity Manager 使用脚本 ===")
    print("=" * 50)
    
    # 配置客户端
    if configure_gemini():
        # 测试连接
        if test_connection():
            # 示例使用
            print("\n=== 示例使用 ===")
            print("=" * 50)
            
            # 示例 1: 简单对话
            print("\n1. 简单对话测试:")
            generate_content("介绍一下你自己")
            
            # 示例 2: 代码生成
            print("\n2. 代码生成测试:")
            generate_content("写一个 Python 函数，计算阶乘")
            
            # 示例 3: 创意写作
            print("\n3. 创意写作测试:")
            generate_content("写一首关于人工智能的短诗")
            
    print("\n=== 使用完成 ===")
    print("=" * 50)

if __name__ == "__main__":
    main()
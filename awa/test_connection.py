#!/usr/bin/env python3
"""
Antigravity Manager 连接测试脚本
"""

import google.generativeai as genai

def test_connection():
    """测试连接"""
    print("=== Antigravity Manager 连接测试 ===")
    
    # 配置
    api_key = "REDACTED_API_KEY"
    api_endpoint = "http://127.0.0.1:8045"
    
    print(f"API 端点: {api_endpoint}")
    print(f"API 密钥: {api_key[:10]}...")
    
    try:
        # 配置客户端
        genai.configure(
            api_key=api_key,
            transport='rest',
            client_options={'api_endpoint': api_endpoint}
        )
        print("✅ 客户端配置成功！")
        
        # 测试模型列表
        print("正在获取模型列表...")
        models = genai.list_models()
        print(f"✅ 成功获取 {len(models)} 个模型")
        
        # 显示可用模型
        print("\n可用模型:")
        for model in models[:5]:  # 只显示前5个
            print(f"  - {model.name}")
        
        # 测试生成
        print("\n正在测试生成能力...")
        model = genai.GenerativeModel('gemini-3-flash')
        response = model.generate_content("你好，测试 Antigravity Manager 连接")
        print("✅ 生成成功！")
        print(f"响应: {response.text}")
        
        return True
    except Exception as e:
        print(f"❌ 测试失败: {e}")
        return False

def main():
    """主函数"""
    success = test_connection()
    
    print("\n=== 测试结果分析 ===")
    if success:
        print("✅ 连接正常！系统可以正常使用。")
        print("\n建议：")
        print("1. 在 TRAECN 应用中使用以下配置：")
        print("   - API 端点: http://127.0.0.1:8045")
        print("   - API 密钥: REDACTED_API_KEY")
        print("   - 模型名称: gemini-3-flash")
    else:
        print("❌ 连接失败，建议：")
        print("1. 检查账号状态，尝试重新授权红色状态的账号")
        print("2. 点击账号右侧的测试按钮，查看具体错误")
        print("3. 尝试删除并重新添加账号")
        print("4. 检查网络连接是否正常")

if __name__ == "__main__":
    main()

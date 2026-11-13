#!/usr/bin/env python3
"""
Antigravity Manager 诊断脚本
"""

import google.generativeai as genai
import requests

def check_antigravity_status():
    """检查 Antigravity Manager 服务状态"""
    print("=== 检查 Antigravity Manager 服务状态 ===")
    
    try:
        # 尝试访问服务
        response = requests.get("http://127.0.0.1:8045", timeout=5)
        print(f" 服务运行中，状态码: {response.status_code}")
        return True
    except Exception as e:
        print(f" 服务未运行: {e}")
        return False

def test_connection():
    """测试连接"""
    print("=== 测试 Antigravity Manager 连接 ===")
    
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
        print(" 客户端配置成功！")
        
        # 测试模型列表
        print("正在获取模型列表...")
        models = genai.list_models()
        print(f" 成功获取模型列表")
        
        # 测试生成
        print("正在测试生成能力...")
        model = genai.GenerativeModel('gemini-3-flash')
        response = model.generate_content("Hello")
        print(" 生成成功！")
        print(f"响应: {response.text}")
        
        return True
    except Exception as e:
        print(f" 测试失败: {e}")
        return False

def main():
    """主函数"""
    print("=== Antigravity Manager 诊断工具 ===")
    
    # 检查服务状态
    if check_antigravity_status():
        # 测试连接
        test_connection()
    else:
        print("请先启动 Antigravity Manager 应用！")
    
    print("\n=== 诊断完成 ===")
    print("\n错误分析:")
    print("'Token error: Token pool is empty' 意味着 Antigravity Manager 没有可用的令牌")
    print("\n可能的原因:")
    print("1. 所有账号状态为红色（错误状态）")
    print("2. 账号未正确授权")
    print("3. 令牌已过期")
    print("4. 服务配置错误")
    
    print("\n解决方案:")
    print("1. 打开 Antigravity Manager 应用")
    print("2. 进入账号列表，检查账号状态")
    print("3. 对于红色状态的账号，点击'重新授权'")
    print("4. 按照提示在浏览器中完成授权流程")
    print("5. 等待账号状态变为绿色")
    print("6. 重启 Antigravity Manager 服务")
    print("7. 再次运行测试脚本")

if __name__ == "__main__":
    main()

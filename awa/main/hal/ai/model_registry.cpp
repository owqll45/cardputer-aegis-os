#include "model_registry.h"
#include <mooncake_log.h>
#include <map>
#include <set>

static const std::string TAG = "ModelRegistry";

ModelRegistry& ModelRegistry::getInstance() {
    static ModelRegistry instance;
    return instance;
}

void ModelRegistry::init() { registerBuiltin(); }

void ModelRegistry::registerBuiltin() {
    _models.clear();

    _models.push_back({"deepseek-chat", "DeepSeek Chat", "deepseek", "llm", 64000, 8192, "通用对话模型，性价比高"});
    _models.push_back({"deepseek-reasoner", "DeepSeek Reasoner", "deepseek", "llm", 64000, 8192, "深度推理模型"});
    _models.push_back({"deepseek-coder", "DeepSeek Coder", "deepseek", "llm", 64000, 8192, "代码生成专用"});

    _models.push_back({"gpt-4o", "GPT-4o", "openai", "llm", 128000, 16384, "OpenAI最新多模态"});
    _models.push_back({"gpt-4o-mini", "GPT-4o Mini", "openai", "llm", 128000, 16384, "轻量快速版"});
    _models.push_back({"o1-preview", "o1 Preview", "openai", "llm", 200000, 100000, "OpenAI推理模型"});
    _models.push_back({"o3-mini", "o3 Mini", "openai", "llm", 200000, 100000, "轻量推理模型"});

    _models.push_back({"claude-sonnet-4-20250514", "Claude Sonnet 4", "anthropic", "llm", 200000, 8192, "Anthropic平衡型"});
    _models.push_back({"claude-opus-4-20250514", "Claude Opus 4", "anthropic", "llm", 200000, 8192, "Anthropic最强模型"});

    _models.push_back({"glm-4-plus", "GLM-4 Plus", "zhipu", "llm", 128000, 4096, "智谱AI旗舰"});
    _models.push_back({"glm-4-flash", "GLM-4 Flash", "zhipu", "llm", 128000, 4096, "智谱AI免费快速"});

    _models.push_back({"qwen-turbo", "Qwen Turbo", "qwen", "llm", 131072, 8192, "通义千问高速版"});
    _models.push_back({"qwen-plus", "Qwen Plus", "qwen", "llm", 131072, 8192, "通义千问增强版"});
    _models.push_back({"qwen-max", "Qwen Max", "qwen", "llm", 131072, 8192, "通义千问最强版"});

    _models.push_back({"custom", "自定义模型ID", "custom", "llm", 0, 0, "手动输入任意模型ID"});

    _models.push_back({"whisper-1", "Whisper v1", "openai", "stt", 0, 0, "OpenAI语音转文字"});
    _models.push_back({"tts-1", "TTS v1", "openai", "tts", 0, 0, "OpenAI文字转语音"});
    _models.push_back({"tts-1-hd", "TTS v1 HD", "openai", "tts", 0, 0, "OpenAI高清语音"});
    _models.push_back({"custom-stt", "自定义STT", "custom", "stt", 0, 0, "自定义STT模型"});
    _models.push_back({"custom-tts", "自定义TTS", "custom", "tts", 0, 0, "自定义TTS模型"});

    mclog::tagInfo(TAG, "Registered {} models", _models.size());
}

const std::vector<ModelEntry>& ModelRegistry::getModelsByType(const std::string& type) const {
    static std::vector<ModelEntry> empty;
    static std::map<std::string, std::vector<ModelEntry>> cache;
    if (cache.find(type) == cache.end()) {
        for (auto& m : _models) if (m.type == type) cache[type].push_back(m);
    }
    auto it = cache.find(type);
    return it != cache.end() ? it->second : empty;
}

const std::vector<ModelEntry>& ModelRegistry::getModelsByProvider(const std::string& provider) const {
    static std::vector<ModelEntry> empty;
    static std::map<std::string, std::vector<ModelEntry>> cache;
    if (cache.find(provider) == cache.end()) {
        for (auto& m : _models) if (m.provider == provider) cache[provider].push_back(m);
    }
    auto it = cache.find(provider);
    return it != cache.end() ? it->second : empty;
}

ModelEntry* ModelRegistry::findModel(const std::string& id) {
    for (auto& m : _models) if (m.id == id) return &m;
    return nullptr;
}

bool ModelRegistry::isCustomModel(const std::string& id) const {
    return id == "custom" || id == "custom-stt" || id == "custom-tts";
}

std::vector<std::string> ModelRegistry::getProviderList() const {
    std::set<std::string> providers;
    for (auto& m : _models) providers.insert(m.provider);
    return std::vector<std::string>(providers.begin(), providers.end());
}

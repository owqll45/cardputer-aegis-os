#pragma once
#include <string>
#include <vector>

struct AiModelInfo {
    std::string name;
    std::string type;
    std::string endpoint;
    std::string api_key;
    bool enabled = false;
};

class AiManager {
public:
    static AiManager& getInstance();

    void init();
    void update();

    bool addModel(const AiModelInfo& model);
    bool removeModel(const std::string& name);
    std::vector<AiModelInfo> getModels() const { return _models; }

    std::string chat(const std::string& prompt, const std::string* model_name = nullptr);

private:
    AiManager() = default;
    ~AiManager() = default;
    AiManager(const AiManager&) = delete;

    std::vector<AiModelInfo> _models;
    bool callOpenAICompatible(const std::string& prompt, const AiModelInfo* model, std::string& response);
};

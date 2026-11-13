#pragma once
#include <string>
#include <vector>

struct ModelEntry {
    std::string id;
    std::string name;
    std::string provider;
    std::string type;
    int context_window = 0;
    int max_output = 0;
    std::string description;
};

class ModelRegistry {
public:
    static ModelRegistry& getInstance();

    void init();
    const std::vector<ModelEntry>& getModels() const { return _models; }
    const std::vector<ModelEntry>& getModelsByType(const std::string& type) const;
    const std::vector<ModelEntry>& getModelsByProvider(const std::string& provider) const;

    ModelEntry* findModel(const std::string& id);
    bool isCustomModel(const std::string& id) const;

    std::vector<std::string> getProviderList() const;

private:
    ModelRegistry() = default;
    void registerBuiltin();
    std::vector<ModelEntry> _models;
};

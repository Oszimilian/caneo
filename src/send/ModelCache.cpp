#include "ModelCache.hpp"

void ModelCache::set_interfaces(const std::vector<InterfaceConfig>& interfaces)
{
    iface_dbc_.clear();
    for (const auto& i : interfaces)
        iface_dbc_[i.name] = i.dbc;
}

std::shared_ptr<const SendModel> ModelCache::model_for(const std::string& interface)
{
    const auto it = iface_dbc_.find(interface);
    if (it == iface_dbc_.end() || it->second.empty())
        return nullptr;
    const std::string& path = it->second;

    if (auto m = models_by_path_.find(path); m != models_by_path_.end())
        return m->second;
    if (load_errors_.contains(path))
        return nullptr;

    try {
        auto model = std::make_shared<const SendModel>(expand_path(path).string());
        models_by_path_[path] = model;
        return model;
    } catch (const std::exception& e) {
        load_errors_[path] = e.what();
        return nullptr;
    }
}

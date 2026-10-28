#pragma once

#include "config/Config.hpp"
#include "send/SendModel.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

// DBC models per interface, loaded lazily and cached per path. The models are never modified
// after loading, so flows may encode with them on the asio thread. Not thread-safe itself:
// use it from the GUI / main thread.
class ModelCache {
public:
    // Interface name -> DBC mapping.
    void set_interfaces(const std::vector<InterfaceConfig>& interfaces);

    // nullptr if the interface has no (loadable) DBC.
    std::shared_ptr<const SendModel> model_for(const std::string& interface);

private:
    std::map<std::string, std::string>                      iface_dbc_;
    std::map<std::string, std::shared_ptr<const SendModel>> models_by_path_;
    std::map<std::string, std::string>                      load_errors_;
};

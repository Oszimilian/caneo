#pragma once

#include "ActionHandler.hpp"
#include "LuaAction.hpp"
#include "config/Config.hpp"
#include "send/SendModel.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

class SignalStore;

// Turns ActionSpecs (as stored in caneo.yaml) into running actions.
//
// DBC files are loaded lazily and cached per path. The cached SendModels are
// never modified after loading, so actions may encode with them on the asio
// thread while the factory is used from the GUI thread.
// Not thread-safe itself: use it from one thread (the GUI / main thread).
class ActionFactory {
public:
    ActionFactory(ActionHandler& handler, const SignalStore* store = nullptr);

    // Interface name -> DBC mapping used to resolve specs.
    void set_interfaces(const std::vector<InterfaceConfig>& interfaces);

    // DBC model of an interface; nullptr if it has no (loadable) DBC.
    std::shared_ptr<const SendModel> model_for(const std::string& interface);

    // Throws std::runtime_error if the spec cannot be resolved (unknown
    // message, missing DBC, ...). Lua script errors do not throw: the action
    // is created in an error state so it can still be edited and saved.
    std::unique_ptr<Action> create(const ActionSpec& spec);

    struct LoadResult {
        std::vector<std::string> errors;
        std::vector<ActionSpec>  failed;   // specs that could not be created
    };

    // Creates and adds all actions of all interfaces (paused unless the spec
    // says otherwise).
    LoadResult add_all(const std::vector<InterfaceConfig>& interfaces);

private:
    ActionHandler&                                          handler_;
    const SignalStore*                                      store_;
    std::map<std::string, std::string>                      iface_dbc_;
    std::map<std::string, std::shared_ptr<const SendModel>> models_by_path_;
    std::map<std::string, std::string>                      load_errors_;
};

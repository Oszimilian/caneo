#include "ActionFactory.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

ActionFactory::ActionFactory(ActionHandler& handler, const SignalStore* store)
    : handler_(handler), store_(store)
{}

void ActionFactory::set_interfaces(const std::vector<InterfaceConfig>& interfaces)
{
    iface_dbc_.clear();
    for (const auto& cfg : interfaces)
        iface_dbc_[cfg.name] = cfg.dbc;
}

std::shared_ptr<const SendModel> ActionFactory::model_for(const std::string& interface)
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

// Physical values for every signal of msg_idx: spec values, 0 otherwise.
static std::vector<double> spec_values(const SendModel& model, int msg_idx, const ActionSpec& spec)
{
    const auto& msg = model.messages()[msg_idx];
    std::vector<double> values(msg.signals.size(), 0.0);
    for (const auto& [name, value] : spec.signals) {
        const int k = model.find_signal(msg_idx, name);
        if (k < 0)
            throw std::runtime_error(std::format("message '{}' has no signal '{}'", spec.msg, name));
        values[k] = value;
    }
    return values;
}

std::unique_ptr<Action> ActionFactory::create(const ActionSpec& spec)
{
    std::unique_ptr<Action> action;
    const auto period = std::chrono::milliseconds(std::max(1, spec.period_ms));

    if (spec.type == "lua") {
        if (spec.script.empty())
            throw std::runtime_error("lua action without script");
        LuaAction::ModelMap models;
        for (const auto& [iface, dbc] : iface_dbc_)
            models[iface] = model_for(iface);
        action = std::make_unique<LuaAction>(
            handler_.io_ref(), spec.interface, handler_.send_fn_ref(), period,
            expand_path(spec.script), std::move(models), store_);
    } else {
        auto model = model_for(spec.interface);
        if (!model)
            throw std::runtime_error(std::format("no DBC loaded for interface '{}'", spec.interface));
        const int msg_idx = model->find_message(spec.msg);
        if (msg_idx < 0)
            throw std::runtime_error(std::format("unknown message '{}' on {}", spec.msg, spec.interface));
        const auto& msg    = model->messages()[msg_idx];
        auto        values = spec_values(*model, msg_idx, spec);

        if (spec.type == "single") {
            action = std::make_unique<SingleAction>(
                handler_.io_ref(), spec.interface, msg.id, msg.name,
                model->encode_values(msg_idx, values), handler_.send_fn_ref());
        } else if (spec.type == "periodic") {
            action = std::make_unique<PeriodicAction>(
                handler_.io_ref(), spec.interface, msg.id, msg.name,
                model->encode_values(msg_idx, values), handler_.send_fn_ref(), period);
        } else if (spec.type == "sin") {
            const int sig_idx = model->find_signal(msg_idx, spec.signal);
            if (sig_idx < 0)
                throw std::runtime_error(std::format("message '{}' has no signal '{}'", spec.msg, spec.signal));
            const double lo = msg.signals[sig_idx].min_val;
            const double hi = msg.signals[sig_idx].max_val;

            // Runs on the asio thread; captures the model to keep it alive.
            SinPeriodicAction::EncodeWithValueFn encode_fn =
                [model, msg_idx, sig_idx, values, lo, hi](double v) mutable {
                    if (lo < hi)
                        v = std::clamp(v, lo, hi);
                    values[sig_idx] = v;
                    return model->encode_values(msg_idx, values);
                };
            action = std::make_unique<SinPeriodicAction>(
                handler_.io_ref(), spec.interface, msg.id, msg.name,
                handler_.send_fn_ref(), period, std::move(encode_fn),
                spec.amplitude, std::chrono::milliseconds(std::max(1, spec.sin_period_ms)),
                spec.offset);
        } else {
            throw std::runtime_error(std::format("unknown action type '{}'", spec.type));
        }
    }

    action->set_spec(spec);
    return action;
}

ActionFactory::LoadResult ActionFactory::add_all(const std::vector<InterfaceConfig>& interfaces)
{
    LoadResult result;
    for (const auto& cfg : interfaces) {
        for (auto spec : cfg.actions) {
            spec.interface = cfg.name;
            try {
                auto action = create(spec);
                if (spec.paused) handler_.add_action_paused(std::move(action));
                else             handler_.add_action(std::move(action));
            } catch (const std::exception& e) {
                result.errors.push_back(std::format("{}: {}", spec, e.what()));
                result.failed.push_back(std::move(spec));
            }
        }
    }
    return result;
}

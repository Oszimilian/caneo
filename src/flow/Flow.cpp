#include "Flow.hpp"

#include "model/SignalStore.hpp"

#include <algorithm>
#include <complex>
#include <map>
#include <numeric>
#include <cmath>
#include <format>
#include <optional>
#include <numbers>
#include <sol/sol.hpp>

const char* Flow::LUA_POSITION_TEMPLATE = R"(-- Position from vx, vy (vehicle frame) and yaw rate: rotate first, then integrate.
--   psi = integral(yaw_rate dt)                  (yaw_rate in rad/s)
--   vX = vx cos(psi) - vy sin(psi)
--   vY = vx sin(psi) + vy cos(psi)
--   X = integral(vX dt), Y = integral(vY dt)
-- Outputs: X, Y, psi (rad), vX, vY. t = seconds since the flow started.

local psi, X, Y, last = 0, 0, 0, nil

function step(vx, vy, yaw_rate, t)
    local dt = last and (t - last) or 0
    last = t
    psi = psi + yaw_rate * dt
    local vX = vx * math.cos(psi) - vy * math.sin(psi)
    local vY = vx * math.sin(psi) + vy * math.cos(psi)
    X = X + vX * dt
    Y = Y + vY * dt
    return { X = X, Y = Y, psi = psi, vX = vX, vY = vY }
end
)";

const char* Flow::LUA_TEMPLATE = R"(-- Lua block: one input u, one output y.
-- step(u, t) runs every flow step. t = seconds since the flow started.
-- Variables outside of step() keep their value between steps.
--
--   get_signal(iface, msg, signal)   last received value of any signal (0 if none)
--   print(...)                       flow log

local y = 0

function step(u, t)
    y = y + 0.1 * (u - y)   -- first order low-pass
    return y
end
)";

// ─── ports / generator ──────────────────────────────────────────────────────

int flow_in_ports(const FlowNodeSpec& n)
{
    if (n.type == "can_out") return static_cast<int>(n.signals.size());
    if (n.type == "lua") return std::max<int>(1, static_cast<int>(n.inputs.size()));
    if (n.type == "scope" || n.type == "diff" || n.type == "movavg" || n.type == "fft" ||
        n.type == "gain" || n.type == "integrator" || n.type == "sin" || n.type == "cos") return 1;
    if (n.type == "add" || n.type == "mul" || n.type == "compare" || n.type == "rs") return 2;
    if (n.type == "switch") return 3;
    if (n.type == "csv") return static_cast<int>(n.signals.size());
    return 0;
}

int flow_out_ports(const FlowNodeSpec& n)
{
    if (n.type == "can_in") return static_cast<int>(n.signals.size());
    if (n.type == "lua") return std::max<int>(1, static_cast<int>(n.signals.size()));
    if (n.type == "generator" || n.type == "const" || n.type == "add" || n.type == "mul" || n.type == "diff" ||
        n.type == "compare" || n.type == "movavg" || n.type == "gain" || n.type == "integrator" || n.type == "sin" || n.type == "cos" || n.type == "slider" || n.type == "button" || n.type == "switch")
        return 1;
    if (n.type == "fft" || n.type == "rs") return 2;
    return 0;
}

bool is_math_block(const std::string& type)
{
    return type == "add" || type == "mul" || type == "diff" || type == "compare" || type == "movavg" || type == "fft" ||
           type == "rs" || type == "switch" || type == "gain" || type == "integrator" || type == "sin" || type == "cos";
}

// In-place radix-2 FFT, n = power of two.
void fft_inplace(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

namespace {

std::size_t fft_size(int window)
{
    std::size_t n = 8;
    while (n < static_cast<std::size_t>(std::clamp(window, 8, 8192))) n <<= 1;
    return n;
}

} // namespace

void math_block_step(const FlowNodeSpec& n, MathState& st, const std::vector<double>& in,
                     std::vector<double>& out, double dt)
{
    if (n.type == "add") {
        out[0] = in[0] + in[1];
    } else if (n.type == "mul") {
        out[0] = in[0] * in[1];
    } else if (n.type == "sin" || n.type == "cos") {
        const double x = n.degrees ? in[0] * std::numbers::pi / 180.0 : in[0];
        out[0]         = n.type == "sin" ? std::sin(x) : std::cos(x);
    } else if (n.type == "gain") {
        out[0] = in[0] * n.gain;
    } else if (n.type == "integrator") {
        if (!st.have_prev) { st.prev = n.offset; st.have_prev = true; }
        st.prev += in[0] * dt;
        out[0]   = st.prev;
    } else if (n.type == "rs") {
        const bool s = in[0] > 0.5, r = in[1] > 0.5;
        bool       q = st.prev > 0.5;
        if (n.op == "S") { if (r) q = false; if (s) q = true; }    // set dominant
        else             { if (s) q = true;  if (r) q = false; }   // reset dominant
        st.prev = q ? 1.0 : 0.0;
        out[0]  = st.prev;
        out[1]  = q ? 0.0 : 1.0;
    } else if (n.type == "switch") {
        out[0] = in[2] > 0.5 ? in[1] : in[0];
    } else if (n.type == "diff") {
        const double d = st.have_prev ? in[0] - st.prev : 0.0;
        out[0]         = n.derivative && dt > 0.0 ? d / dt : d;
        st.prev        = in[0];
        st.have_prev   = true;
    } else if (n.type == "compare") {
        const double a = in[0], b = in[1];
        bool r = false;
        if (n.op == "<")       r = a < b;
        else if (n.op == ">")  r = a > b;
        else if (n.op == "<=") r = a <= b;
        else if (n.op == ">=") r = a >= b;
        else if (n.op == "==") r = std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
        out[0] = r ? 1.0 : 0.0;
    } else if (n.type == "movavg") {
        const auto len = static_cast<std::size_t>(std::max(1, n.window));
        if (st.buf.size() != len) {
            st.buf.assign(len, 0.0);
            st.pos = st.filled = 0;
            st.sum = 0.0;
        }
        st.sum += in[0] - st.buf[st.pos];
        st.buf[st.pos] = in[0];
        st.pos         = (st.pos + 1) % len;
        st.filled      = std::min(st.filled + 1, len);
        // recompute now and then: the running sum accumulates rounding errors
        if (st.pos == 0) st.sum = std::accumulate(st.buf.begin(), st.buf.end(), 0.0);
        out[0] = st.sum / static_cast<double>(st.filled);
    } else if (n.type == "fft") {
        const std::size_t len = fft_size(n.window);
        if (st.buf.size() != len) {
            st.buf.assign(len, 0.0);
            st.pos = st.filled = st.since_fft = 0;
            st.out.assign(2, 0.0);
        }
        st.buf[st.pos] = in[0];
        st.pos         = (st.pos + 1) % len;
        st.filled      = std::min(st.filled + 1, len);
        if (st.filled == len && ++st.since_fft >= std::max<std::size_t>(1, len / 8)) {
            st.since_fft = 0;
            double mean = std::accumulate(st.buf.begin(), st.buf.end(), 0.0) / static_cast<double>(len);
            std::vector<std::complex<double>> a(len);
            double wsum = 0.0;
            for (std::size_t i = 0; i < len; ++i) {   // oldest sample first, Hann window
                const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(len));
                a[i]  = (st.buf[(st.pos + i) % len] - mean) * w;
                wsum += w;
            }
            fft_inplace(a);
            std::size_t best = 1;
            for (std::size_t k = 2; k < len / 2; ++k)
                if (std::abs(a[k]) > std::abs(a[best])) best = k;
            st.out[0] = static_cast<double>(best) / (static_cast<double>(len) * dt);
            st.out[1] = 2.0 * std::abs(a[best]) / wsum;
        }
        out[0] = st.out[0];
        out[1] = st.out[1];
    }
}

double generator_value(const FlowNodeSpec& n, double t, std::mt19937& rng)
{
    constexpr double TWO_PI = 2.0 * std::numbers::pi;
    const double     cycles = n.frequency * t + n.phase / 360.0;
    const double     frac   = cycles - std::floor(cycles);   // 0..1 within the period

    if (n.wave == "sin")      return n.offset + n.amplitude * std::sin(TWO_PI * cycles);
    if (n.wave == "pulse")    return n.offset + (frac < n.duty ? n.amplitude : 0.0);
    if (n.wave == "sawtooth") return n.offset + n.amplitude * frac;
    if (n.wave == "triangle") return n.offset + n.amplitude * (frac < 0.5 ? 2.0 * frac : 2.0 - 2.0 * frac);
    if (n.wave == "ramp")     return n.offset + n.amplitude * std::clamp(cycles, 0.0, 1.0);   // one-shot
    if (n.wave == "noise") {
        std::uniform_real_distribution<double> d(-1.0, 1.0);
        return n.offset + n.amplitude * d(rng);
    }
    return n.offset + n.amplitude;   // const
}

// ─── FlowState ──────────────────────────────────────────────────────────────

void FlowState::set(Values values)
{
    std::lock_guard lock(mutex_);
    values_ = std::move(values);
}

FlowState::Values FlowState::values() const
{
    std::lock_guard lock(mutex_);
    return values_;
}

// ─── Flow ───────────────────────────────────────────────────────────────────

struct Flow::Node {
    const FlowNodeSpec*              spec = nullptr;
    std::vector<double>              in;
    std::vector<double>              out;
    std::vector<std::pair<int, int>> sources;   // per input port: (node index, port) or (-1, -1)

    // can_out
    std::shared_ptr<const SendModel> model;
    int                              msg_idx = -1;
    std::vector<int>                 sig_idx;   // per input port: signal index in the message
    std::vector<double>              values;    // physical value per message signal
    std::chrono::steady_clock::time_point last_sent{};
    bool                             has_consts = false;
    bool                             sent_once  = false;
    bool                             force      = false;   // send now (trigger)

    // slider / button
    double                           control = 0.0;
    bool                             pulse   = false;

    // lua send(): last values per (interface, message index)
    std::map<std::pair<std::string, int>, std::vector<double>> send_values;

    MathState math;   // add, gain, integrator, diff, compare, movavg, fft

    // lua
    std::unique_ptr<sol::state> lua;
    sol::protected_function     step;
};

static std::optional<double> to_double(const sol::object& obj)
{
    if (obj.is<bool>())      return obj.as<bool>() ? 1.0 : 0.0;
    if (obj.is<double>())    return obj.as<double>();
    if (obj.is<long long>()) return static_cast<double>(obj.as<long long>());
    return std::nullopt;
}

Flow::Flow(boost::asio::io_context& io, FlowSpec spec, const SignalStore* store, SendFn send_fn,
           OutputFn out_fn, const ModelFn& models)
    : models_(models), timer_(io), spec_(std::move(spec)), store_(store)
    , send_fn_(std::move(send_fn)), out_fn_(std::move(out_fn))
{
    spec_.step_ms = std::max(1, spec_.step_ms);
    loaded_ = build(models);
}

Flow::~Flow() = default;

bool Flow::build(const ModelFn& models)
{
    const auto  n_nodes = spec_.nodes.size();
    auto        err     = [this](const std::string& what) { state_->log.set_error(what); return false; };

    nodes_.clear();
    for (const auto& ns : spec_.nodes) {
        auto node = std::make_unique<Node>();
        node->spec = &ns;
        node->in.assign(static_cast<std::size_t>(flow_in_ports(ns)), 0.0);
        node->out.assign(static_cast<std::size_t>(flow_out_ports(ns)), 0.0);
        node->sources.assign(node->in.size(), {-1, -1});
        node->control = ns.value;
        nodes_.push_back(std::move(node));
    }

    auto index_of = [this](int id) -> int {
        for (std::size_t i = 0; i < spec_.nodes.size(); ++i)
            if (spec_.nodes[i].id == id) return static_cast<int>(i);
        return -1;
    };

    // links -> per-input sources, and the dependency graph for ordering
    std::vector<std::vector<std::size_t>> deps(n_nodes);   // deps[i] = nodes i reads from
    for (const auto& l : spec_.links) {
        const int from = index_of(l.from_node), to = index_of(l.to_node);
        if (from < 0 || to < 0) continue;
        auto& dst = *nodes_[static_cast<std::size_t>(to)];
        if (l.to_port < 0 || l.to_port >= static_cast<int>(dst.in.size())) continue;
        if (l.from_port < 0 || l.from_port >= static_cast<int>(nodes_[static_cast<std::size_t>(from)]->out.size())) continue;
        dst.sources[static_cast<std::size_t>(l.to_port)] = {from, l.from_port};
        deps[static_cast<std::size_t>(to)].push_back(static_cast<std::size_t>(from));
    }

    // DFS topological order; back edges (loops) just read the previous step.
    order_.clear();
    std::vector<int> mark(n_nodes, 0);   // 0 new, 1 in progress, 2 done
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (mark[i]) return;
        mark[i] = 1;
        for (auto d : deps[i]) visit(d);
        mark[i] = 2;
        order_.push_back(i);
    };
    for (std::size_t i = 0; i < n_nodes; ++i) visit(i);

    for (std::size_t i = 0; i < n_nodes; ++i) {
        Node&               node = *nodes_[i];
        const FlowNodeSpec& ns   = spec_.nodes[i];
        const std::string   who  = std::format("block '{}': ", ns.name.empty() ? ns.type : ns.name);

        if (ns.type == "can_out") {
            node.model = models ? models(ns.interface) : nullptr;
            if (!node.model)
                return err(who + std::format("no DBC for interface '{}'", ns.interface));
            node.msg_idx = node.model->find_message(ns.msg);
            if (node.msg_idx < 0)
                return err(who + std::format("unknown message '{}'", ns.msg));
            const auto& msg = node.model->messages()[static_cast<std::size_t>(node.msg_idx)];
            for (const auto& s : msg.signals) node.values.push_back(s.value);
            for (const auto& sig : ns.signals) {
                const int si = node.model->find_signal(static_cast<std::size_t>(node.msg_idx), sig);
                if (si < 0) return err(who + std::format("unknown signal '{}'", sig));
                node.sig_idx.push_back(si);
            }
            for (std::size_t p = 0; p < ns.sig_values.size() && p < node.sig_idx.size(); ++p)
                node.values[static_cast<std::size_t>(node.sig_idx[p])] = ns.sig_values[p];
            node.has_consts = !ns.sig_values.empty() || ns.signals.empty();   // no ports: sends the DBC defaults
        } else if (ns.type == "lua") {
            node.lua = std::make_unique<sol::state>();
            node.lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table,
                                     sol::lib::string, sol::lib::os);
            node.lua->set_function("print", [this, &node](sol::variadic_args args) {
                std::string line;
                sol::function tostring = (*node.lua)["tostring"];
                for (std::size_t k = 0; k < args.size(); ++k) {
                    if (k) line += '\t';
                    line += tostring(args[k]).get<std::string>();
                }
                state_->log.append(std::format("[{}] {}", node.spec->name, line));
            });
            node.lua->set_function("get_signal",
                [this](const std::string& iface, const std::string& msg, const std::string& sig) -> double {
                    return store_ ? store_->get(iface, msg, sig).value_or(0.0) : 0.0;
                });
            (*node.lua)["interface"] = ns.interface;
            node.lua->set_function("send",
                [this, &node, nsp = &ns](const std::string& msg, sol::optional<sol::table> sigs,
                                         sol::optional<std::string> iface_opt) {
                    const std::string iface = iface_opt.value_or(nsp->interface);
                    const auto model = models_ ? models_(iface) : nullptr;
                    if (!model) throw std::runtime_error(std::format("send: no DBC loaded for interface '{}'", iface));
                    const int msg_idx = model->find_message(msg);
                    if (msg_idx < 0) throw std::runtime_error(std::format("send: unknown message '{}' on {}", msg, iface));
                    const auto& m      = model->messages()[static_cast<std::size_t>(msg_idx)];
                    auto&       values = node.send_values[{iface, msg_idx}];
                    values.resize(m.signals.size(), 0.0);
                    if (sigs)
                        for (const auto& [key, val] : *sigs) {
                            if (!key.is<std::string>()) continue;
                            const auto name = key.as<std::string>();
                            const int  si   = model->find_signal(static_cast<std::size_t>(msg_idx), name);
                            if (si < 0) throw std::runtime_error(std::format("send: message '{}' has no signal '{}'", msg, name));
                            const auto v = to_double(val);
                            if (!v) throw std::runtime_error(std::format("send: value of '{}' is not a number", name));
                            values[static_cast<std::size_t>(si)] = *v;
                        }
                    if (send_fn_) send_fn_(iface, m.id, model->encode_values(static_cast<std::size_t>(msg_idx), values));
                });
            node.lua->set_function("send_raw",
                [this, nsp = &ns](long long id, sol::table bytes, sol::optional<std::string> iface_opt) {
                    std::vector<uint8_t> data;
                    for (std::size_t k = 1; k <= bytes.size() && k <= 64; ++k)
                        data.push_back(static_cast<uint8_t>(bytes.get<long long>(k) & 0xFF));
                    if (send_fn_) send_fn_(iface_opt.value_or(nsp->interface), static_cast<uint64_t>(id), data);
                });
            const auto r = node.lua->safe_script(ns.code, sol::script_pass_on_error);
            if (!r.valid()) {
                const sol::error e = r;
                return err(who + e.what());
            }
            node.step = (*node.lua)["step"];
            if (!node.step.valid())
                return err(who + "script defines no step(u, t) function");
            if ((*node.lua)["init"].is<sol::function>()) {
                sol::protected_function init = (*node.lua)["init"];
                const auto ir = init();
                if (!ir.valid()) {
                    const sol::error e = ir;
                    return err(who + "init(): " + e.what());
                }
            }
        }
    }
    return true;
}

void Flow::trigger(int node_id)
{
    for (std::size_t i = 0; i < spec_.nodes.size(); ++i)
        if (spec_.nodes[i].id == node_id && spec_.nodes[i].type == "can_out") nodes_[i]->force = true;
}

void Flow::set_control(int node_id, double value)
{
    for (std::size_t i = 0; i < spec_.nodes.size(); ++i) {
        if (spec_.nodes[i].id != node_id) continue;
        if (spec_.nodes[i].type == "slider") {
            nodes_[i]->control      = value;
            spec_.nodes[i].value    = value;   // saved with the config
        } else if (spec_.nodes[i].type == "button") {
            nodes_[i]->pulse = true;
        }
    }
}

void Flow::start()
{
    if (!loaded_ || running_) return;
    running_    = true;
    start_time_ = std::chrono::steady_clock::now();
    next_tick_  = start_time_;
    steps_      = 0;
    state_->log.set_error({});
    schedule();
}

void Flow::stop()
{
    running_ = false;
    timer_.cancel();
}

void Flow::schedule()
{
    next_tick_ += std::chrono::milliseconds(spec_.step_ms);
    const auto now = std::chrono::steady_clock::now();
    if (next_tick_ < now)   // fell behind: do not burst
        next_tick_ = now;
    timer_.expires_at(next_tick_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec || !running_) return;   // cancelled: `this` may already be gone
        tick();
        if (running_) schedule();
    });
}

void Flow::fail(const std::string& what)
{
    state_->log.set_error(what);
    stop();
}

double Flow::input(std::size_t node, std::size_t port) const
{
    const auto [src, sp] = nodes_[node]->sources[port];
    return src < 0 ? 0.0 : nodes_[static_cast<std::size_t>(src)]->out[static_cast<std::size_t>(sp)];
}

void Flow::tick()
{
    const auto   now = std::chrono::steady_clock::now();
    const double t   = std::chrono::duration<double>(now - start_time_).count();

    for (const auto i : order_) {
        Node&               node = *nodes_[i];
        const FlowNodeSpec& ns   = spec_.nodes[i];
        for (std::size_t p = 0; p < node.in.size(); ++p)
            node.in[p] = input(i, p);

        if (ns.type == "can_in") {
            for (std::size_t p = 0; p < node.out.size(); ++p)
                node.out[p] = store_ ? store_->get(ns.interface, ns.msg, ns.signals[p]).value_or(0.0) : 0.0;
        } else if (ns.type == "generator") {
            node.out[0] = generator_value(ns, t, rng_);
        } else if (ns.type == "const") {
            node.out[0] = ns.offset;
        } else if (ns.type == "slider") {
            node.out[0] = node.control;
        } else if (ns.type == "button") {
            node.out[0] = node.pulse ? 1.0 : 0.0;
            node.pulse  = false;
        } else if (ns.type == "lua") {
            const auto r = node.in.size() == 1 ? node.step(node.in[0], t) : [&] {
                std::vector<double> args = node.in;
                args.push_back(t);
                return node.step(sol::as_args(args));
            }();
            if (!r.valid()) {
                const sol::error e = r;
                return fail(std::format("block '{}': step(): {}", ns.name, e.what()));
            }
            const sol::object ret = r[0];
            if (ret.is<sol::table>()) {
                const sol::table tb = ret.as<sol::table>();
                for (std::size_t p = 0; p < node.out.size(); ++p) {
                    sol::object o = p < ns.signals.size() ? tb[ns.signals[p]] : sol::object();
                    if (!o.valid() || o == sol::lua_nil) o = tb[p + 1];
                    if (const auto v = to_double(o)) node.out[p] = *v;
                }
            } else if (const auto v = to_double(ret)) {
                node.out[0] = *v;
            }
        } else if (is_math_block(ns.type)) {
            math_block_step(ns, node.math, node.in, node.out, spec_.step_ms * 1e-3);
        } else if (ns.type == "scope") {
            if (out_fn_) out_fn_(spec_.name, ns.name, node.in[0]);
        } else if (ns.type == "can_out") {
            bool any = node.has_consts;
            for (std::size_t p = 0; p < node.in.size(); ++p) {
                if (node.sources[p].first < 0) continue;   // unconnected keeps its constant / the DBC default
                node.values[static_cast<std::size_t>(node.sig_idx[p])] = node.in[p];
                any = true;
            }
            const bool once = ns.period_ms <= 0;   // 0 = once when the flow starts
            bool       due  = any && (once ? !node.sent_once : now - node.last_sent >= std::chrono::milliseconds(ns.period_ms));
            if (node.force) due = true;
            if (due) {
                node.force     = false;
                node.sent_once = true;
                node.last_sent = now;
                const auto& msg = node.model->messages()[static_cast<std::size_t>(node.msg_idx)];
                if (send_fn_)
                    send_fn_(ns.interface, msg.id, node.model->encode_values(static_cast<std::size_t>(node.msg_idx), node.values));
            }
        }
    }

    FlowState::Values v;
    v.steps = ++steps_;
    for (const auto& n : nodes_) {
        v.in.push_back(n->in);
        v.out.push_back(n->out);
    }
    state_->set(std::move(v));
}

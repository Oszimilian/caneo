#pragma once

#include "action/Action.hpp"   // ActionLog, SendFn
#include "config/Config.hpp"
#include "send/SendModel.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <complex>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

class SignalStore;
namespace sol { class state; }

// Number of input / output ports of a block.
int flow_in_ports(const FlowNodeSpec& node);
int flow_out_ports(const FlowNodeSpec& node);

// Value of a generator block at time t (seconds).
double generator_value(const FlowNodeSpec& node, double t, std::mt19937& rng);

// State of the built-in math blocks (add, diff, compare, movavg, fft).
struct MathState {
    std::vector<double> buf;       // ring buffer (movavg, fft)
    std::size_t         pos = 0;
    std::size_t         filled = 0;
    std::size_t         since_fft = 0;
    double              sum = 0.0;
    double              prev = 0.0;
    bool                have_prev = false;
    std::vector<double> out;       // held outputs of the fft
};

// In-place radix-2 FFT, a.size() must be a power of two.
void fft_inplace(std::vector<std::complex<double>>& a);

bool is_math_block(const std::string& type);

// Evaluates one math block for one step of dt seconds; `out` has flow_out_ports() entries.
void math_block_step(const FlowNodeSpec& node, MathState& st, const std::vector<double>& in,
                     std::vector<double>& out, double dt);

// Live values of a flow; written on the asio thread, read by the GUI.
class FlowState {
public:
    struct Values {
        std::vector<std::vector<double>> in;    // per node (spec order), per input port
        std::vector<std::vector<double>> out;   // per node, per output port
        uint64_t                         steps = 0;
    };

    void   set(Values values);
    Values values() const;

    ActionLog log;   // Lua print() output + error

private:
    mutable std::mutex mutex_;
    Values             values_;
};

// Runs a FlowSpec: every step_ms all blocks are evaluated in dependency order.
//
//   can_in     latest value of each selected signal (SignalStore)
//   generator  waveform of the simulation time
//   lua        step(u, t) -> y, one Lua state per block (state lives in upvalues)
//   can_out    collects its inputs into the message and sends it every period_ms
//   scope      forwards its input to the OutputFn (graph / log)
//
// Feedback loops are allowed: a link that points "backwards" delivers the
// value of the previous step (like a unit delay in Simulink).
//
// Constructed on any thread; start/stop/step only on the asio thread.
class Flow {
public:
    using OutputFn = std::function<void(const std::string& flow, const std::string& scope, double value)>;
    using ModelFn  = std::function<std::shared_ptr<const SendModel>(const std::string& interface)>;

    Flow(boost::asio::io_context& io, FlowSpec spec, const SignalStore* store, SendFn send_fn,
         OutputFn out_fn, const ModelFn& models);
    ~Flow();

    Flow(const Flow&)            = delete;
    Flow& operator=(const Flow&) = delete;

    const FlowSpec&              spec() const   { return spec_; }
    std::shared_ptr<FlowState>   state() const  { return state_; }
    bool                         loaded() const { return loaded_; }
    bool                         running() const { return running_; }

    // can_out block: send its message on the next step (asio thread)
    void trigger(int node_id);
    // slider: new value; button: one pulse (asio thread)
    void set_control(int node_id, double value);

    void start();   // asio thread
    void stop();    // asio thread

    // Script template for new Lua blocks.
    static const char* LUA_TEMPLATE;
    static const char* LUA_POSITION_TEMPLATE;   // vx, vy, yaw rate -> X, Y

private:
    struct Node;

    bool   build(const ModelFn& models);
    void   tick();
    void   schedule();
    void   fail(const std::string& what);
    double input(std::size_t node, std::size_t port) const;

    ModelFn                        models_;
    boost::asio::steady_timer      timer_;
    FlowSpec                       spec_;
    const SignalStore*             store_;
    SendFn                         send_fn_;
    OutputFn                       out_fn_;
    std::shared_ptr<FlowState>     state_ = std::make_shared<FlowState>();
    bool                           loaded_  = false;
    bool                           running_ = false;

    std::vector<std::unique_ptr<Node>> nodes_;   // spec order
    std::vector<std::size_t>           order_;   // evaluation order (indices into nodes_)
    std::mt19937                       rng_{12345};

    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point next_tick_;
    uint64_t                              steps_ = 0;
};

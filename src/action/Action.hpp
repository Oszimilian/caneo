#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using SendFn = std::function<void(const std::string& iface, uint64_t id, const std::vector<uint8_t>& data)>;

class Action {
public:
    Action(boost::asio::io_context& io,
           std::string              interface,
           uint64_t                 msg_id,
           std::string              msg_name,
           std::vector<uint8_t>     payload,
           SendFn                   send_fn);

    virtual ~Action() = default;

    virtual bool                      is_periodic() const = 0;
    virtual std::string               type_name()   const = 0;
    virtual std::chrono::milliseconds period()      const = 0;

    const std::string&                    interface() const { return interface_; }
    uint64_t                              msg_id()    const { return msg_id_; }
    const std::string&                    msg_name()  const { return msg_name_; }
    std::chrono::steady_clock::time_point last_sent() const { return last_sent_; }
    bool                                  ever_sent() const { return ever_sent_; }

    void start(std::function<void()> on_fired, std::function<void()> on_done);
    void cancel();

protected:
    void fire();
    virtual void schedule() = 0;

    boost::asio::steady_timer             timer_;
    std::string                           interface_;
    uint64_t                              msg_id_;
    std::string                           msg_name_;
    std::vector<uint8_t>                  payload_;
    SendFn                                send_fn_;
    std::function<void()>                 on_fired_;
    std::function<void()>                 on_done_;
    std::chrono::steady_clock::time_point last_sent_{};
    bool                                  ever_sent_ = false;
};

class SingleAction : public Action {
public:
    using Action::Action;
    bool                      is_periodic() const override { return false; }
    std::string               type_name()   const override { return "Single"; }
    std::chrono::milliseconds period()      const override { return {}; }
protected:
    void schedule() override;
};

class PeriodicAction : public Action {
public:
    PeriodicAction(boost::asio::io_context&  io,
                   std::string               interface,
                   uint64_t                  msg_id,
                   std::string               msg_name,
                   std::vector<uint8_t>      payload,
                   SendFn                    send_fn,
                   std::chrono::milliseconds period);

    bool                      is_periodic() const override { return true; }
    std::string               type_name()   const override { return "Periodic"; }
    std::chrono::milliseconds period()      const override { return period_; }
protected:
    void schedule() override;
private:
    std::chrono::milliseconds period_;
};

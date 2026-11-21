#include "CanStreamService.hpp"
#include "logger/LogController.hpp"
#include "setup/InterfaceSetup.hpp"
#include "compat/print.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <unistd.h>

void CanStreamService::broadcast(const CanFrame& frame) {
    caneo::RawCanFrame proto;
    proto.set_interface(frame.header().interface);
    proto.set_can_id(frame.header().id);
    proto.set_dlc(frame.header().dlc);
    proto.set_fd(frame.header().fd);
    proto.set_brs(frame.header().brs);
    const auto& payload = frame.payload();
    proto.set_data(std::string(payload.begin(), payload.end()));
    proto.set_timestamp_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
        .count());

    std::lock_guard lock(subs_mutex_);
    for (auto& sub : subscribers_) {
        if (!sub->interface_filter.empty() &&
            sub->interface_filter != frame.header().interface)
            continue;
        std::lock_guard slock(sub->mutex);
        sub->queue.push(proto);
        sub->cv.notify_one();
    }
}

void CanStreamService::shutdown() {
    std::lock_guard lock(subs_mutex_);
    for (auto& sub : subscribers_) {
        sub->done = true;
        sub->cv.notify_one();
    }
}

grpc::Status CanStreamService::Subscribe(grpc::ServerContext*,
                                         const caneo::SubscribeRequest* req,
                                         grpc::ServerWriter<caneo::RawCanFrame>* writer) {
    auto sub = std::make_shared<Subscriber>();
    sub->interface_filter = req->interface();

    {
        std::lock_guard lock(subs_mutex_);
        subscribers_.push_back(sub);
    }

    while (!sub->done) {
        std::unique_lock lock(sub->mutex);
        sub->cv.wait(lock, [&] { return !sub->queue.empty() || sub->done.load(); });

        while (!sub->queue.empty() && !sub->done) {
            caneo::RawCanFrame frame = std::move(sub->queue.front());
            sub->queue.pop();
            lock.unlock();
            if (!writer->Write(frame))
                sub->done = true;
            lock.lock();
        }
    }

    {
        std::lock_guard lock(subs_mutex_);
        std::erase(subscribers_, sub);
    }

    return grpc::Status::OK;
}

grpc::Status CanStreamService::Send(grpc::ServerContext*,
                                    const caneo::RawCanFrame* req,
                                    caneo::SendResult* result) {
    if (send_fn_) {
        const auto& raw = req->data();
        std::vector<uint8_t> data(raw.begin(), raw.end());
        send_fn_(req->interface(), req->can_id(), data);
        result->set_ok(true);
    } else {
        result->set_ok(false);
    }
    return grpc::Status::OK;
}

static void fill_log_status(caneo::LogStatus* status, LogController* lc) {
    if (!lc) return;
    status->set_active(lc->is_logging());
    status->set_path(lc->current_path());
}

grpc::Status CanStreamService::StartLog(grpc::ServerContext*,
                                        const caneo::LogControlRequest*,
                                        caneo::LogStatus* status) {
    if (log_controller_) log_controller_->start();
    fill_log_status(status, log_controller_);
    return grpc::Status::OK;
}

grpc::Status CanStreamService::StopLog(grpc::ServerContext*,
                                       const caneo::LogControlRequest*,
                                       caneo::LogStatus* status) {
    if (log_controller_) log_controller_->stop();
    fill_log_status(status, log_controller_);
    return grpc::Status::OK;
}

grpc::Status CanStreamService::GetLogStatus(grpc::ServerContext*,
                                            const caneo::LogControlRequest*,
                                            caneo::LogStatus* status) {
    fill_log_status(status, log_controller_);
    return grpc::Status::OK;
}

void CanStreamService::set_interfaces(const std::vector<InterfaceConfig>& interfaces) {
    std::lock_guard lock(interfaces_mutex_);
    interfaces_.clear();
    for (const auto& i : interfaces)
        if (!i.is_remote()) interfaces_.push_back(i);   // only channels this caneo owns
}

grpc::Status CanStreamService::ListInterfaces(grpc::ServerContext*,
                                              const caneo::ListInterfacesRequest*,
                                              caneo::InterfaceList* list) {
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0) list->set_host(host);
    std::lock_guard lock(interfaces_mutex_);
    for (const auto& i : interfaces_) {
        auto* info = list->add_interfaces();
        info->set_name(i.name);
        info->set_bitrate(i.virtual_can ? 0 : i.baudrate);
        info->set_data_bitrate(i.fd ? i.data_baudrate : 0);
        info->set_fd(i.fd);
        std::ifstream f("/sys/class/net/" + i.name + "/operstate");
        std::string state;
        info->set_state(f >> state ? state : "missing");
    }
    return grpc::Status::OK;
}

grpc::Status CanStreamService::ResetInterfaces(grpc::ServerContext*,
                                               const caneo::ResetRequest* req,
                                               caneo::ResetResult* result) {
    std::vector<std::string> names, errors;
    {
        std::lock_guard lock(interfaces_mutex_);
        for (const auto& i : interfaces_)
            if (req->interfaces().empty() || std::ranges::contains(req->interfaces(), i.name))
                names.push_back(i.name);
        for (const auto& want : req->interfaces())
            if (std::ranges::none_of(interfaces_, [&](const auto& i) { return i.name == want; }))
                errors.push_back(want + ": not a channel of this server");
    }
    std::string err = reset_interfaces(names, /*allow_privileged=*/false);
    for (const auto& e : errors) err += (err.empty() ? "" : "; ") + e;
    std::println("gRPC: reset {} channel(s){}", names.size(), err.empty() ? "" : " - " + err);
    result->set_ok(err.empty());
    result->set_error(err);
    return grpc::Status::OK;
}

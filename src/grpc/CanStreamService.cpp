#include "CanStreamService.hpp"

#include <chrono>

void CanStreamService::broadcast(const CanFrame& frame) {
    caneo::RawCanFrame proto;
    proto.set_interface(frame.header().interface);
    proto.set_can_id(frame.header().id);
    proto.set_dlc(frame.header().dlc);
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

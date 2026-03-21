#include "SocketGrpc.hpp"

#include "frame/CanFrame.hpp"

#include <chrono>
#include <grpcpp/grpcpp.h>

SocketGrpc::SocketGrpc(boost::asio::io_context& io,
                       std::string interface,
                       std::string server_address)
    : io_(io)
    , interface_(std::move(interface))
    , server_address_(std::move(server_address)) {}

SocketGrpc::~SocketGrpc() {
    stop();
}

void SocketGrpc::start() {
    running_ = true;

    reader_thread_ = std::thread([this]() {
        while (running_) {
            auto channel = grpc::CreateChannel(server_address_,
                                               grpc::InsecureChannelCredentials());
            auto stub = caneo::CanStream::NewStub(channel);
            auto ctx  = std::make_unique<grpc::ClientContext>();

            {
                std::lock_guard lock(ctx_mutex_);
                current_ctx_ = ctx.get();
            }

            caneo::SubscribeRequest req;
            req.set_interface(interface_);
            auto reader = stub->Subscribe(ctx.get(), req);

            connected_ = true;
            caneo::RawCanFrame proto_frame;
            while (running_ && reader->Read(&proto_frame)) {
                CanHeader hdr{
                    .id        = proto_frame.can_id(),
                    .interface = interface_,
                    .dlc       = static_cast<uint8_t>(proto_frame.dlc()),
                };
                const auto& data = proto_frame.data();
                std::vector<uint8_t> payload(data.begin(), data.end());
                auto frame = std::make_unique<CanFrame>(std::move(hdr), std::move(payload));
                boost::asio::post(io_, [this, f = std::move(frame)]() mutable {
                    if (callback_) callback_(std::move(f));
                });
            }

            connected_ = false;

            {
                std::lock_guard lock(ctx_mutex_);
                current_ctx_ = nullptr;
            }

            if (running_)
                std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });
}

void SocketGrpc::stop() {
    running_ = false;
    {
        std::lock_guard lock(ctx_mutex_);
        if (current_ctx_) current_ctx_->TryCancel();
    }
    if (reader_thread_.joinable()) reader_thread_.join();
}

std::ostream& operator<<(std::ostream& os, const SocketGrpc& s) {
    return os << "SocketGrpc [" << s.interface_ << "] -> " << s.server_address_;
}

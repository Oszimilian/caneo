#include "RemoteInterfaces.hpp"

#include "can_stream.grpc.pb.h"

#include <chrono>
#include <grpcpp/grpcpp.h>

namespace {

std::string status_text(const grpc::Status& status)
{
    switch (status.error_code()) {
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::UNAVAILABLE:
        return "not reachable (is caneo --grpc_server running there?)";
    case grpc::StatusCode::UNIMPLEMENTED:
        return "server is too old - update caneo there";
    default:
        return status.error_message();
    }
}

}   // namespace

std::expected<void, std::string> reset_remote_interfaces(const std::string& address, const std::vector<std::string>& names,
                                                         int timeout_ms)
{
    if (address.empty()) return std::unexpected("no address");
    auto stub = caneo::CanStream::NewStub(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms));
    caneo::ResetRequest req;
    for (const auto& n : names) req.add_interfaces(n);
    caneo::ResetResult resp;
    if (const grpc::Status status = stub->ResetInterfaces(&ctx, req, &resp); !status.ok())
        return std::unexpected(status_text(status));
    if (!resp.ok()) return std::unexpected(resp.error());
    return {};
}

std::expected<RemoteInterfaceList, std::string> list_remote_interfaces(const std::string& address, int timeout_ms)
{
    if (address.empty()) return std::unexpected("no address");
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub    = caneo::CanStream::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms));
    caneo::ListInterfacesRequest req;
    caneo::InterfaceList         resp;
    const grpc::Status status = stub->ListInterfaces(&ctx, req, &resp);
    if (!status.ok()) return std::unexpected(status_text(status));
    RemoteInterfaceList out;
    out.host = resp.host();
    for (const auto& i : resp.interfaces())
        out.interfaces.push_back({i.name(), i.bitrate(), i.data_bitrate(), i.fd(), i.state()});
    return out;
}

std::ostream& operator<<(std::ostream& os, const RemoteInterface& i)
{
    os << i.name << " (" << i.state;
    if (i.bitrate) os << ", " << i.bitrate / 1000 << " kbit/s";
    if (i.fd) os << ", FD" << (i.data_bitrate ? " " + std::to_string(i.data_bitrate / 1000) + " kbit/s" : "");
    return os << ")";
}

std::ostream& operator<<(std::ostream& os, const RemoteInterfaceList& l)
{
    os << "RemoteInterfaceList(" << l.host << ": ";
    for (std::size_t i = 0; i < l.interfaces.size(); ++i) os << (i ? ", " : "") << l.interfaces[i];
    return os << ")";
}

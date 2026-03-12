#pragma once

#include "Decoder.hpp"

#include <dbcppp/Network.h>

#include <filesystem>
#include <memory>
#include <unordered_map>

class DbcPppWrapper : public Decoder {
public:
    explicit DbcPppWrapper(const std::filesystem::path& dbc_path);
    void decode(CanFrame& frame) override;

private:
    std::unique_ptr<dbcppp::INetwork> network_;
    std::unordered_map<uint64_t, const dbcppp::IMessage*> messages_;
};

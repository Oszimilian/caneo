#pragma once

#include "Decoder.hpp"
#include "frame/CanFrame.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

class DecoderRegistry {
public:
    void add_interface(const std::string& interface, const std::filesystem::path& dbc_path);
    void decode(CanFrame& frame);

private:
    std::unordered_map<std::string, std::unique_ptr<Decoder>> decoders_;
};

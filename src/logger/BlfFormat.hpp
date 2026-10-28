#pragma once

// Constants and little-endian helpers of the Vector BLF format, shared by
// BlfWriter and BlfReader. Values as in python-can's can/io/blf.py.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace blf {

inline constexpr uint32_t FILE_HEADER_SIZE          = 144;
inline constexpr uint16_t OBJ_HEADER_BASE_SIZE      = 16;
inline constexpr uint16_t OBJ_HEADER_V1_SIZE        = 16;
inline constexpr uint16_t OBJ_HEADER_V2_SIZE        = 24;
inline constexpr uint32_t LOG_CONTAINER_HEADER_SIZE = 16;
inline constexpr std::size_t MAX_CONTAINER_SIZE     = 128 * 1024;

// object types
inline constexpr uint32_t CAN_MESSAGE       = 1;
inline constexpr uint32_t LOG_CONTAINER     = 10;
inline constexpr uint32_t APP_TEXT          = 65;
inline constexpr uint32_t CAN_ERROR_EXT     = 73;
inline constexpr uint32_t CAN_MESSAGE2      = 86;
inline constexpr uint32_t CAN_FD_MESSAGE    = 100;
inline constexpr uint32_t CAN_FD_MESSAGE_64 = 101;

inline constexpr uint16_t NO_COMPRESSION = 0;
inline constexpr uint16_t ZLIB_DEFLATE   = 2;

inline constexpr uint32_t TIME_TEN_MICS = 0x1;
inline constexpr uint32_t TIME_ONE_NANS = 0x2;

inline constexpr uint32_t CAN_MSG_EXT = 0x80000000u;
inline constexpr uint8_t  FD_EDL = 0x1, FD_BRS = 0x2, FD_ESI = 0x4;
inline constexpr uint32_t APPTEXT_MEASUREMENTCOMMENT = 0;

template <typename T>
inline void put_le(std::vector<uint8_t>& out, T v)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i)));
}

inline void put_bytes(std::vector<uint8_t>& out, const void* data, std::size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

template <typename T>
inline T get_le(const uint8_t* p)
{
    uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<T>(v);
}

inline constexpr std::array<uint8_t, 16> DLC_TO_LEN = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

inline uint8_t len_to_dlc(std::size_t len)
{
    for (uint8_t dlc = 0; dlc < DLC_TO_LEN.size(); ++dlc)
        if (DLC_TO_LEN[dlc] >= len) return dlc;
    return 15;
}

inline std::size_t dlc_to_len(uint8_t dlc) { return DLC_TO_LEN[std::min<uint8_t>(dlc, 15)]; }

} // namespace blf

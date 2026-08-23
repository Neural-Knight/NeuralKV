#pragma once

#include <cstddef>
#include <cstdint>

namespace neuralkv::protocol {

constexpr uint16_t kMagic = 0x4E4B;  // "NK"
constexpr uint8_t kProtocolVersion = 0x01;
constexpr std::size_t kFrameHeaderSize = 8;
constexpr uint32_t kMaxPayloadSize = 16 * 1024 * 1024;  // 16 MiB

}  // namespace neuralkv::protocol

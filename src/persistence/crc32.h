#pragma once

#include <cstddef>
#include <cstdint>

namespace neuralkv::persistence {

// Standard IEEE 802.3 CRC32 (the polynomial used by zlib/gzip), computed
// bit-by-bit rather than via a lookup table: WAL records are small and
// fsync dominates append cost, so a table isn't worth the complexity.
uint32_t Crc32(const uint8_t* data, std::size_t len);

}  // namespace neuralkv::persistence

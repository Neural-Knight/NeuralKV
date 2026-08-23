#include "persistence/crc32.h"

namespace neuralkv::persistence {

uint32_t Crc32(const uint8_t* data, std::size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return ~crc;
}

}  // namespace neuralkv::persistence

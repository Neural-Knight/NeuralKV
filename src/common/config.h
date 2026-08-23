#pragma once

#include <cstdint>
#include <string>

namespace neuralkv {

struct NodeConfig {
  uint32_t node_id = 0;
  std::string host = "127.0.0.1";
  uint16_t port = 7400;
  std::string data_dir = "./data";
};

}  // namespace neuralkv

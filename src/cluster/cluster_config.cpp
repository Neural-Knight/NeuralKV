#include "cluster/cluster_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace neuralkv::cluster {

namespace {

std::string Trim(const std::string& s) {
  std::size_t begin = 0;
  while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) ++begin;
  std::size_t end = s.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
  return s.substr(begin, end - begin);
}

Status ParseLine(const std::string& raw_line, ClusterConfig& config) {
  const std::string line = Trim(raw_line);
  if (line.empty() || line.front() == '#') return Status::Ok();

  if (line.rfind("peer", 0) == 0 && (line.size() == 4 || std::isspace(static_cast<unsigned char>(line[4])) != 0)) {
    std::istringstream tokens(line);
    std::string directive, host;
    long id = 0;
    int port = 0;
    tokens >> directive >> id >> host >> port;
    if (!tokens || !tokens.eof() || id <= 0 || id > 0xFFFFFFFFL || port <= 0 || port > 0xFFFF) {
      return Status::Error(ErrorCode::kInvalidArgument, "malformed peer line: " + raw_line);
    }
    config.peers.push_back(
        PeerInfo{.node_id = static_cast<uint32_t>(id), .host = host, .port = static_cast<uint16_t>(port)});
    return Status::Ok();
  }

  const std::size_t eq = line.find('=');
  if (eq == std::string::npos) {
    return Status::Error(ErrorCode::kInvalidArgument, "unrecognized config line: " + raw_line);
  }
  const std::string key = Trim(line.substr(0, eq));
  const std::string value = Trim(line.substr(eq + 1));
  if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return Status::Error(ErrorCode::kInvalidArgument, "malformed value in line: " + raw_line);
  }
  const uint32_t parsed = static_cast<uint32_t>(std::stoul(value));

  if (key == "node_id") {
    config.local_node_id = parsed;
  } else if (key == "leader_id") {
    config.leader_node_id = parsed;
  } else {
    return Status::Error(ErrorCode::kInvalidArgument, "unknown config key: " + key);
  }
  return Status::Ok();
}

Status Validate(const ClusterConfig& config) {
  if (config.local_node_id == 0) {
    return Status::Error(ErrorCode::kInvalidArgument, "cluster config missing node_id");
  }
  if (config.peers.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "cluster config has no peers");
  }

  bool seen_local = false;
  bool seen_leader = config.leader_node_id == 0;
  for (std::size_t i = 0; i < config.peers.size(); ++i) {
    const PeerInfo& peer = config.peers[i];
    for (std::size_t j = i + 1; j < config.peers.size(); ++j) {
      if (config.peers[j].node_id == peer.node_id) {
        return Status::Error(ErrorCode::kInvalidArgument,
                              "duplicate peer id " + std::to_string(peer.node_id));
      }
    }
    if (peer.node_id == config.local_node_id) seen_local = true;
    if (peer.node_id == config.leader_node_id) seen_leader = true;
  }

  if (!seen_local) {
    return Status::Error(ErrorCode::kInvalidArgument, "local node_id is not in the peer list");
  }
  if (!seen_leader) {
    return Status::Error(ErrorCode::kInvalidArgument, "leader_id is not in the peer list");
  }
  return Status::Ok();
}

}  // namespace

const PeerInfo* ClusterConfig::FindPeer(uint32_t node_id) const {
  for (const PeerInfo& peer : peers) {
    if (peer.node_id == node_id) return &peer;
  }
  return nullptr;
}

Status LoadClusterConfig(const std::string& path, ClusterConfig& out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return Status::Error(ErrorCode::kIOError, "open cluster config: " + path);
  }

  ClusterConfig config;
  std::string line;
  while (std::getline(file, line)) {
    const Status status = ParseLine(line, config);
    if (!status.ok()) return status;
  }

  const Status status = Validate(config);
  if (!status.ok()) return status;

  out = std::move(config);
  return Status::Ok();
}

}  // namespace neuralkv::cluster

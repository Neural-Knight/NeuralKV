#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "cluster/cluster_config.h"
#include "common/result.h"
#include "common/status.h"
#include "net/socket_utils.h"
#include "protocol/codec.h"
#include "protocol/types.h"

namespace {

using neuralkv::protocol::ClientRequest;
using neuralkv::protocol::ClientResponse;
using neuralkv::protocol::Opcode;
using neuralkv::protocol::ResponseStatus;

void PrintUsage() {
  std::cout << "nkv-client - NeuralKV command-line client\n\n"
               "Usage:\n"
               "  nkv-client [--host <addr>] [--port <n>] [--cluster-config <path>] set <key> <value>\n"
               "  nkv-client [--host <addr>] [--port <n>] [--cluster-config <path>] get <key>\n"
               "  nkv-client [--host <addr>] [--port <n>] [--cluster-config <path>] delete <key>\n"
               "  nkv-client --help\n\n"
               "With --cluster-config set, a WRONG_LEADER response is followed once by a\n"
               "retry against the leader node it names.\n";
}

std::string_view NextArg(int argc, char** argv, int& i, std::string_view flag_name) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << flag_name << "\n";
    std::exit(EXIT_FAILURE);
  }
  return argv[++i];
}

std::string_view StatusName(ResponseStatus status) {
  switch (status) {
    case ResponseStatus::kOk:
      return "OK";
    case ResponseStatus::kNotFound:
      return "NOT_FOUND";
    case ResponseStatus::kBadRequest:
      return "BAD_REQUEST";
    case ResponseStatus::kInternalError:
      return "INTERNAL_ERROR";
    case ResponseStatus::kWrongLeader:
      return "WRONG_LEADER";
  }
  return "UNKNOWN";
}

// Sends req over fd and blocks for exactly one response frame.
neuralkv::Result<ClientResponse> SendRequest(int fd, const ClientRequest& req) {
  std::vector<uint8_t> encoded;
  neuralkv::Status encode_status = neuralkv::protocol::EncodeClientRequest(req, encoded);
  if (!encode_status.ok()) return encode_status;

  neuralkv::Status write_status =
      neuralkv::net::WriteFull(fd, encoded.data(), encoded.size());
  if (!write_status.ok()) return write_status;

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  ClientResponse resp;
  while (true) {
    const neuralkv::protocol::ParseResult parse_result =
        neuralkv::protocol::TryParseFrame(buffer, nullptr, &resp);
    if (parse_result == neuralkv::protocol::ParseResult::kComplete) {
      return resp;
    }
    if (parse_result == neuralkv::protocol::ParseResult::kError) {
      return neuralkv::Status::Error(neuralkv::ErrorCode::kIOError,
                                      "malformed response from server");
    }

    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      return neuralkv::Status::Error(neuralkv::ErrorCode::kIOError,
                                      std::string("read: ") + std::strerror(errno));
    }
    if (n == 0) {
      return neuralkv::Status::Error(neuralkv::ErrorCode::kIOError,
                                      "connection closed before response received");
    }
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

// Connects to host:port and performs one request/response round trip.
neuralkv::Result<ClientResponse> ConnectAndSend(const std::string& host, uint16_t port,
                                                 const ClientRequest& req) {
  neuralkv::Result<int> conn_result = neuralkv::net::TcpConnect(host, port);
  if (!conn_result.ok()) return conn_result.status();
  neuralkv::net::Fd fd(conn_result.value());
  return SendRequest(fd.get(), req);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 7400;
  std::string cluster_config_path;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--host") {
      host = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--port") {
      port = static_cast<uint16_t>(std::stoi(std::string(NextArg(argc, argv, i, arg))));
    } else if (arg == "--cluster-config") {
      cluster_config_path = std::string(NextArg(argc, argv, i, arg));
    } else {
      positional.emplace_back(arg);
    }
  }

  if (positional.empty()) {
    std::cerr << "missing command\n";
    PrintUsage();
    return EXIT_FAILURE;
  }

  const std::string& command = positional[0];
  ClientRequest req;
  req.request_id = 1;

  if (command == "set") {
    if (positional.size() != 3) {
      std::cerr << "usage: nkv-client set <key> <value>\n";
      return EXIT_FAILURE;
    }
    req.opcode = Opcode::kSet;
    req.key = positional[1];
    req.value = positional[2];
  } else if (command == "get") {
    if (positional.size() != 2) {
      std::cerr << "usage: nkv-client get <key>\n";
      return EXIT_FAILURE;
    }
    req.opcode = Opcode::kGet;
    req.key = positional[1];
  } else if (command == "delete") {
    if (positional.size() != 2) {
      std::cerr << "usage: nkv-client delete <key>\n";
      return EXIT_FAILURE;
    }
    req.opcode = Opcode::kDelete;
    req.key = positional[1];
  } else {
    std::cerr << "unknown command: " << command << "\n";
    PrintUsage();
    return EXIT_FAILURE;
  }

  neuralkv::Result<ClientResponse> result = ConnectAndSend(host, port, req);
  if (!result.ok()) {
    std::cerr << result.status().message() << "\n";
    return EXIT_FAILURE;
  }

  ClientResponse resp = result.value();
  if (resp.status == ResponseStatus::kWrongLeader && resp.leader_hint != 0) {
    if (cluster_config_path.empty()) {
      std::cerr << "server reports leader is node " << resp.leader_hint
                 << "; pass --cluster-config to redirect automatically\n";
    } else {
      neuralkv::cluster::ClusterConfig cluster_config;
      const neuralkv::Status load_status =
          neuralkv::cluster::LoadClusterConfig(cluster_config_path, cluster_config);
      if (!load_status.ok()) {
        std::cerr << "failed to load cluster config: " << load_status.message() << "\n";
        return EXIT_FAILURE;
      }
      const neuralkv::cluster::PeerInfo* leader = cluster_config.FindPeer(resp.leader_hint);
      if (leader == nullptr) {
        std::cerr << "leader node " << resp.leader_hint << " is not in " << cluster_config_path
                   << "\n";
        return EXIT_FAILURE;
      }

      std::cerr << "redirected to leader node " << leader->node_id << " at " << leader->host
                 << ":" << leader->port << "\n";
      neuralkv::Result<ClientResponse> retry_result = ConnectAndSend(leader->host, leader->port, req);
      if (!retry_result.ok()) {
        std::cerr << retry_result.status().message() << "\n";
        return EXIT_FAILURE;
      }
      resp = retry_result.value();
    }
  }

  if (resp.status == ResponseStatus::kOk) {
    if (command == "get") {
      std::cout << resp.value << "\n";
    } else {
      std::cout << "OK\n";
    }
    return EXIT_SUCCESS;
  }
  if (resp.status == ResponseStatus::kNotFound) {
    std::cerr << "NOT_FOUND\n";
    return EXIT_FAILURE;
  }
  std::cerr << StatusName(resp.status) << "\n";
  return EXIT_FAILURE;
}

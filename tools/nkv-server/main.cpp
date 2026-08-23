#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <signal.h>

#include "common/config.h"
#include "common/status.h"
#include "server/blocking_server.h"
#include "storage/sharded_kv.h"

namespace {

neuralkv::BlockingServer* g_server = nullptr;

void HandleShutdownSignal(int) {
  if (g_server != nullptr) {
    g_server->Stop();
  }
}

void PrintUsage() {
  std::cout << "nkv-server - NeuralKV single-node server\n\n"
               "Usage: nkv-server [options]\n\n"
               "Options:\n"
               "  --host <addr>   Listen address (default: 127.0.0.1)\n"
               "  --port <n>      Listen port (default: 7400; 0 for ephemeral)\n"
               "  --help          Show this message\n";
}

std::string_view NextArg(int argc, char** argv, int& i, std::string_view flag_name) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << flag_name << "\n";
    std::exit(EXIT_FAILURE);
  }
  return argv[++i];
}

}  // namespace

int main(int argc, char** argv) {
  neuralkv::NodeConfig config;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--host") {
      config.host = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--port") {
      config.port = static_cast<uint16_t>(std::stoi(std::string(NextArg(argc, argv, i, arg))));
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  neuralkv::ShardedKV kv;
  neuralkv::BlockingServer server(config.host, config.port, kv);
  g_server = &server;

  struct sigaction action {};
  action.sa_handler = HandleShutdownSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // deliberately no SA_RESTART: interrupts the blocking accept()
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;

  const neuralkv::Status status = server.Run();
  if (!status.ok()) {
    std::cerr << "server error: " << status.message() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

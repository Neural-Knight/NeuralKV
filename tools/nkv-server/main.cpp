#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include <signal.h>

#include "common/config.h"
#include "common/status.h"
#include "server/blocking_server.h"
#include "server/thread_pool_server.h"
#include "storage/sharded_kv.h"

namespace {

std::function<void()> g_stop_callback;

void HandleShutdownSignal(int) {
  if (g_stop_callback) {
    g_stop_callback();
  }
}

void PrintUsage() {
  std::cout << "nkv-server - NeuralKV single-node server\n\n"
               "Usage: nkv-server [options]\n\n"
               "Options:\n"
               "  --host <addr>    Listen address (default: 127.0.0.1)\n"
               "  --port <n>       Listen port (default: 7400; 0 for ephemeral)\n"
               "  --workers <n>    Worker threads (default: 1 = single-threaded\n"
               "                   blocking server; >1 uses a thread-pool server)\n"
               "  --help           Show this message\n";
}

std::string_view NextArg(int argc, char** argv, int& i, std::string_view flag_name) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << flag_name << "\n";
    std::exit(EXIT_FAILURE);
  }
  return argv[++i];
}

void InstallShutdownHandlers() {
  struct sigaction action {};
  action.sa_handler = HandleShutdownSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // deliberately no SA_RESTART: interrupts the blocking accept()
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  neuralkv::NodeConfig config;
  int workers = 1;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--host") {
      config.host = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--port") {
      config.port = static_cast<uint16_t>(std::stoi(std::string(NextArg(argc, argv, i, arg))));
    } else if (arg == "--workers") {
      workers = std::stoi(std::string(NextArg(argc, argv, i, arg)));
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  if (workers < 1) {
    std::cerr << "--workers must be >= 1\n";
    return EXIT_FAILURE;
  }

  neuralkv::ShardedKV kv;
  InstallShutdownHandlers();

  neuralkv::Status status = neuralkv::Status::Ok();
  if (workers == 1) {
    neuralkv::BlockingServer server(config.host, config.port, kv);
    g_stop_callback = [&server] { server.Stop(); };
    std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;
    status = server.Run();
  } else {
    neuralkv::ThreadPoolServer server(config.host, config.port, kv,
                                       static_cast<std::size_t>(workers));
    g_stop_callback = [&server] { server.Stop(); };
    std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;
    status = server.Run();
  }

  if (!status.ok()) {
    std::cerr << "server error: " << status.message() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

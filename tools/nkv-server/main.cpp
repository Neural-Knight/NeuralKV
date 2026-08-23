#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include <signal.h>

#include "cluster/cluster_config.h"
#include "common/config.h"
#include "common/result.h"
#include "common/status.h"
#include "net/epoll_server.h"
#include "persistence/durable_storage.h"
#include "server/blocking_server.h"
#include "server/thread_pool_server.h"

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
               "  --host <addr>          Listen address (default: 127.0.0.1)\n"
               "  --port <n>             Listen port (default: 7400; 0 for ephemeral)\n"
               "  --data-dir <path>      WAL and data directory (default: ./data)\n"
               "  --workers <n>          Worker threads (default: 1)\n"
               "  --io <mode>            blocking | threadpool | epoll (Linux only)\n"
               "                         default: blocking if --workers 1, else threadpool\n"
               "  --node-id <n>          This node's cluster id (required with --cluster-config)\n"
               "  --cluster-config <path>  Static cluster membership + leader file\n"
               "  --help                 Show this message\n";
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
  [[maybe_unused]] bool workers_explicit = false;
  std::string io_mode;
  std::string cluster_config_path;
  uint32_t node_id_flag = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--host") {
      config.host = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--port") {
      config.port = static_cast<uint16_t>(std::stoi(std::string(NextArg(argc, argv, i, arg))));
    } else if (arg == "--data-dir") {
      config.data_dir = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--workers") {
      workers = std::stoi(std::string(NextArg(argc, argv, i, arg)));
      workers_explicit = true;
    } else if (arg == "--io") {
      io_mode = std::string(NextArg(argc, argv, i, arg));
    } else if (arg == "--node-id") {
      node_id_flag = static_cast<uint32_t>(std::stoul(std::string(NextArg(argc, argv, i, arg))));
    } else if (arg == "--cluster-config") {
      cluster_config_path = std::string(NextArg(argc, argv, i, arg));
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  if (io_mode.empty()) {
    io_mode = (workers > 1) ? "threadpool" : "blocking";
  }
  if (io_mode != "blocking" && io_mode != "threadpool" && io_mode != "epoll") {
    std::cerr << "unknown --io mode: " << io_mode << "\n";
    return EXIT_FAILURE;
  }

  if (io_mode == "epoll") {
#ifndef NEURALKV_LINUX
    std::cerr << "--io epoll is only supported on Linux\n";
    return EXIT_FAILURE;
#else
    if (workers_explicit) {
      std::cerr << "warning: --workers is ignored with --io epoll (single-threaded event loop)\n";
    }
#endif
  } else if (workers < 1) {
    std::cerr << "--workers must be >= 1\n";
    return EXIT_FAILURE;
  }

  neuralkv::cluster::ClusterConfig cluster_config;
  const neuralkv::cluster::ClusterConfig* cluster_config_ptr = nullptr;
  if (!cluster_config_path.empty()) {
    if (node_id_flag == 0) {
      std::cerr << "--node-id is required when --cluster-config is set\n";
      return EXIT_FAILURE;
    }
    const neuralkv::Status status =
        neuralkv::cluster::LoadClusterConfig(cluster_config_path, cluster_config);
    if (!status.ok()) {
      std::cerr << "failed to load cluster config: " << status.message() << "\n";
      return EXIT_FAILURE;
    }
    if (cluster_config.local_node_id != node_id_flag) {
      std::cerr << "--node-id " << node_id_flag << " does not match node_id "
                << cluster_config.local_node_id << " in " << cluster_config_path << "\n";
      return EXIT_FAILURE;
    }
    cluster_config_ptr = &cluster_config;
  }

  neuralkv::Result<neuralkv::persistence::DurableStorage> storage_result =
      neuralkv::persistence::DurableStorage::Open(config.data_dir);
  if (!storage_result.ok()) {
    std::cerr << "failed to recover WAL in " << config.data_dir << ": "
               << storage_result.status().message() << "\n";
    return EXIT_FAILURE;
  }
  neuralkv::persistence::DurableStorage& storage = storage_result.value();

  InstallShutdownHandlers();

  neuralkv::Status status = neuralkv::Status::Ok();
  if (io_mode == "blocking") {
    neuralkv::BlockingServer server(config.host, config.port, storage, cluster_config_ptr);
    g_stop_callback = [&server] { server.Stop(); };
    std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;
    status = server.Run();
  } else if (io_mode == "threadpool") {
    neuralkv::ThreadPoolServer server(config.host, config.port, storage,
                                       static_cast<std::size_t>(workers), cluster_config_ptr);
    g_stop_callback = [&server] { server.Stop(); };
    std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;
    status = server.Run();
  } else {
#ifdef NEURALKV_LINUX
    neuralkv::net::EpollServer server(config.host, config.port, storage, cluster_config_ptr);
    g_stop_callback = [&server] { server.Stop(); };
    std::cout << "listening " << config.host << ":" << server.port() << "\n" << std::flush;
    status = server.Run();
#endif
  }

  if (!status.ok()) {
    std::cerr << "server error: " << status.message() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include "common/result.h"
#include "common/status.h"
#include "net/socket_utils.h"
#include "protocol/codec.h"
#include "protocol/types.h"

namespace {

struct BenchOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 7400;
  int duration_sec = 60;
  int clients = 1;
  int ratio = 80;  // percent of ops that are GET
  bool bench_mode = false;
};

struct ClientResult {
  std::vector<uint64_t> latencies_us;
};

void PrintUsage() {
  std::cout <<
      "nkv-bench - NeuralKV load generator\n\n"
      "Usage: nkv-bench [options]\n\n"
      "Options:\n"
      "  --host <addr>       Target host (default: 127.0.0.1)\n"
      "  --port <n>          Target port (default: 7400)\n"
      "  --duration <sec>    Run duration in seconds (default: 60)\n"
      "  --clients <n>       Concurrent client connections (default: 1)\n"
      "  --ratio <pct>       Percent of ops that are GET (default: 80)\n"
      "  --bench             Run the load benchmark against a live server\n"
      "  --help              Show this message\n";
}

// Returns the value following flag_name in argv, or exits with an error
// if the flag is missing its argument.
std::string_view NextArg(int argc, char** argv, int& i, std::string_view flag_name) {
  if (i + 1 >= argc) {
    std::cerr << "missing value for " << flag_name << "\n";
    std::exit(EXIT_FAILURE);
  }
  return argv[++i];
}

// Sends req over fd and blocks for exactly one response frame. Returns
// false (rather than propagating an error) on any I/O failure, since a
// benchmark worker should just stop rather than crash the whole run.
bool SendRequestSync(int fd, const neuralkv::protocol::ClientRequest& req,
                      neuralkv::protocol::ClientResponse& out_resp) {
  std::vector<uint8_t> encoded;
  if (!neuralkv::protocol::EncodeClientRequest(req, encoded).ok()) return false;
  if (!neuralkv::net::WriteFull(fd, encoded.data(), encoded.size()).ok()) return false;

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  while (true) {
    const neuralkv::protocol::ParseResult result =
        neuralkv::protocol::TryParseFrame(buffer, nullptr, &out_resp);
    if (result == neuralkv::protocol::ParseResult::kComplete) return true;
    if (result == neuralkv::protocol::ParseResult::kError) return false;

    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) return false;
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

// Runs one client connection for the benchmark's duration, recording the
// latency of every completed op into result.
void RunClientWorker(const BenchOptions& options, int thread_id,
                      std::chrono::steady_clock::time_point deadline, ClientResult& result) {
  neuralkv::Result<int> conn = neuralkv::net::TcpConnect(options.host, options.port);
  if (!conn.ok()) {
    std::cerr << "client " << thread_id << ": connect failed: " << conn.status().message()
              << "\n";
    return;
  }
  neuralkv::net::Fd fd(conn.value());

  constexpr int kKeySpace = 1000;
  const std::string value(256, 'v');
  const std::string key_prefix = "bench:" + std::to_string(thread_id) + ":";

  // Pre-populate this thread's keyspace outside the timed loop so GETs
  // mostly hit real data instead of measuring cold NOT_FOUND lookups.
  uint64_t request_id = 1;
  for (int i = 0; i < kKeySpace; ++i) {
    neuralkv::protocol::ClientRequest req{.request_id = request_id++,
                                           .opcode = neuralkv::protocol::Opcode::kSet,
                                           .key = key_prefix + std::to_string(i),
                                           .value = value};
    neuralkv::protocol::ClientResponse resp;
    if (!SendRequestSync(fd.get(), req, resp)) return;
  }

  uint64_t seq = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const int idx = static_cast<int>(seq % kKeySpace);
    const bool do_get = static_cast<int>(seq % 100) < options.ratio;

    neuralkv::protocol::ClientRequest req;
    req.request_id = request_id++;
    req.key = key_prefix + std::to_string(idx);
    if (do_get) {
      req.opcode = neuralkv::protocol::Opcode::kGet;
    } else {
      req.opcode = neuralkv::protocol::Opcode::kSet;
      req.value = value;
    }

    const auto op_start = std::chrono::steady_clock::now();
    neuralkv::protocol::ClientResponse resp;
    const bool ok = SendRequestSync(fd.get(), req, resp);
    const auto op_end = std::chrono::steady_clock::now();
    if (!ok) break;

    const auto latency_us =
        std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
    result.latencies_us.push_back(static_cast<uint64_t>(latency_us));
    ++seq;
  }
}

uint64_t Percentile(const std::vector<uint64_t>& sorted_latencies, double p) {
  if (sorted_latencies.empty()) return 0;
  const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted_latencies.size() - 1));
  return sorted_latencies[idx];
}

void RunBenchmark(const BenchOptions& options) {
  std::vector<ClientResult> results(static_cast<size_t>(options.clients));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(options.clients));

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(options.duration_sec);

  for (int t = 0; t < options.clients; ++t) {
    workers.emplace_back(
        [&options, &results, t, deadline]() { RunClientWorker(options, t, deadline, results[static_cast<size_t>(t)]); });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  const auto end = std::chrono::steady_clock::now();

  std::vector<uint64_t> all_latencies;
  for (const ClientResult& result : results) {
    all_latencies.insert(all_latencies.end(), result.latencies_us.begin(),
                          result.latencies_us.end());
  }
  std::sort(all_latencies.begin(), all_latencies.end());

  const double elapsed_sec = std::chrono::duration<double>(end - start).count();
  const double ops_per_sec =
      elapsed_sec > 0 ? static_cast<double>(all_latencies.size()) / elapsed_sec : 0.0;

  std::cout << "total ops: " << all_latencies.size() << "\n"
            << "ops/sec:   " << ops_per_sec << "\n"
            << "p50 (us):  " << Percentile(all_latencies, 0.50) << "\n"
            << "p95 (us):  " << Percentile(all_latencies, 0.95) << "\n"
            << "p99 (us):  " << Percentile(all_latencies, 0.99) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  BenchOptions options;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--host") {
      options.host = NextArg(argc, argv, i, arg);
    } else if (arg == "--port") {
      options.port = static_cast<uint16_t>(std::stoi(std::string(NextArg(argc, argv, i, arg))));
    } else if (arg == "--duration") {
      options.duration_sec = std::stoi(std::string(NextArg(argc, argv, i, arg)));
    } else if (arg == "--clients") {
      options.clients = std::stoi(std::string(NextArg(argc, argv, i, arg)));
    } else if (arg == "--ratio") {
      options.ratio = std::stoi(std::string(NextArg(argc, argv, i, arg)));
    } else if (arg == "--bench") {
      options.bench_mode = true;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  if (options.bench_mode) {
    RunBenchmark(options);
    return EXIT_SUCCESS;
  }

  std::cout << "nkv-bench configuration:\n"
            << "  host:     " << options.host << "\n"
            << "  port:     " << options.port << "\n"
            << "  duration: " << options.duration_sec << "s\n"
            << "  clients:  " << options.clients << "\n"
            << "pass --bench to run the load benchmark against a live server\n";

  return EXIT_SUCCESS;
}

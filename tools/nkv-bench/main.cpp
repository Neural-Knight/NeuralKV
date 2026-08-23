#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct BenchOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 7400;
  int duration_sec = 60;
  int clients = 1;
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
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  std::cout << "nkv-bench configuration:\n"
            << "  host:     " << options.host << "\n"
            << "  port:     " << options.port << "\n"
            << "  duration: " << options.duration_sec << "s\n"
            << "  clients:  " << options.clients << "\n"
            << "load generation not implemented yet\n";

  return EXIT_SUCCESS;
}

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

namespace neuralkv::testutil {

// Fresh temp directory for one test's --data-dir; removes the files
// nkv-server creates in it (wal/wal.log, wal/) on destruction.
class TempDataDir {
 public:
  TempDataDir() {
    char pattern[] = "/tmp/nkv_test_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    if (dir == nullptr) {
      std::perror("mkdtemp");
      std::abort();
    }
    path_ = dir;
  }

  ~TempDataDir() {
    ::unlink((path_ + "/wal/wal.log").c_str());
    ::rmdir((path_ + "/wal").c_str());
    ::rmdir(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Reads the "host:port" line nkv-server prints on startup from fd and
// returns the port, or 0 if the line never arrives (fd closed early).
inline uint16_t ParsePortFromChildOutput(int fd) {
  std::string line;
  char c = 0;
  while (::read(fd, &c, 1) == 1 && c != '\n') {
    line.push_back(c);
  }
  const std::size_t colon = line.rfind(':');
  if (colon == std::string::npos) return 0;
  return static_cast<uint16_t>(std::stoi(line.substr(colon + 1)));
}

}  // namespace neuralkv::testutil

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace neuralkv {

// Fixed-size pool of worker threads draining a shared task queue. No
// work-stealing or per-worker queues — plain enough for a server that just
// needs to hand off accepted connections without spawning a thread each.
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t num_workers);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Queues task for a worker to run. Silently dropped if called after
  // shutdown has begun.
  void Submit(std::function<void()> task);

 private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

}  // namespace neuralkv

// psdfx 内部のスレッドプール。最初に並列処理を頼まれたときに作り、
// psdfx_set_threads / psdfx_shutdown_threads で数を変えたり止めたりする。
#include "psdfx.h"
#include "psdfx_parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace psdfx_internal {
namespace {

thread_local bool tInParallel = false;   // 並列処理の中 (入れ子は分けない)

class Pool {
public:
  // 使うスレッド数 (呼び出し元を含む)。0 なら自動
  void setThreads(int n) {
    std::lock_guard<std::mutex> callLock(callMutex_);
    stopWorkers();
    requested_ = n;
  }
  void shutdown() {
    std::lock_guard<std::mutex> callLock(callMutex_);
    stopWorkers();
  }
  int threads() {
    int n = requested_;
    if (n <= 0) {
      if (const char *e = std::getenv("PSDFX_THREADS")) n = std::atoi(e);
    }
    if (n <= 0) n = (int)std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
    return std::max(1, std::min(64, n));
  }

  void run(int begin, int end, const std::function<void(int, int)> &fn) {
    // 同時に複数の呼び出し元から来たら 1 つずつ (並列の中身は 1 組ずつ回す)
    std::unique_lock<std::mutex> callLock(callMutex_, std::try_to_lock);
    const int n = threads();
    if (!callLock.owns_lock() || n <= 1) { fn(begin, end); return; }
    ensureWorkers(n - 1);
    // 1 スレッドあたり数個のかたまりに分けて取り合う (偏りをならす)
    const int total = end - begin;
    const int chunks = std::min(total, n * 4);
    const int step = (total + chunks - 1) / chunks;
    {
      std::lock_guard<std::mutex> lk(m_);
      job_ = &fn; jobBegin_ = begin; jobEnd_ = end; jobStep_ = step;
      next_.store(begin); pending_ = (int)workers_.size(); ++generation_;
    }
    cv_.notify_all();
    work();   // 呼び出し元も手伝う
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return pending_ == 0; });
    job_ = nullptr;
  }

private:
  void work() {
    tInParallel = true;
    for (;;) {
      const int lo = next_.fetch_add(jobStep_);
      if (lo >= jobEnd_) break;
      (*job_)(lo, std::min(jobEnd_, lo + jobStep_));
    }
    tInParallel = false;
  }
  void ensureWorkers(int count) {
    if ((int)workers_.size() == count) return;
    stopWorkers();
    stop_ = false;
    for (int i = 0; i < count; i++)
      workers_.emplace_back([this] {
        unsigned seen = 0;
        for (;;) {
          {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
          }
          work();
          std::lock_guard<std::mutex> lk(m_);
          if (--pending_ == 0) done_.notify_one();
        }
      });
  }
  void stopWorkers() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) t.join();
    workers_.clear();
  }

  std::mutex callMutex_;
  std::mutex m_;
  std::condition_variable cv_, done_;
  std::vector<std::thread> workers_;
  bool stop_ = false;
  unsigned generation_ = 0;
  int pending_ = 0;
  int requested_ = 0;
  const std::function<void(int, int)> *job_ = nullptr;
  int jobBegin_ = 0, jobEnd_ = 0, jobStep_ = 1;
  std::atomic<int> next_{ 0 };
};

// プロセスの終わりまで残す (DLL の後始末の中でスレッドを待つと止まることがあるので、
// 止めるときは psdfx_shutdown_threads を呼んでもらう)
Pool &pool() {
  static Pool *p = new Pool();
  return *p;
}

}  // namespace

void parallelFor(int begin, int end, long long work, const std::function<void(int, int)> &fn) {
  if (end <= begin) return;
  if (tInParallel || work < kParallelMinWork || end - begin < 2) { fn(begin, end); return; }
  pool().run(begin, end, fn);
}

}  // namespace psdfx_internal

extern "C" void psdfx_set_threads(int count) { psdfx_internal::pool().setThreads(count); }

extern "C" int psdfx_get_threads(void) { return psdfx_internal::pool().threads(); }

extern "C" void psdfx_shutdown_threads(void) { psdfx_internal::pool().shutdown(); }

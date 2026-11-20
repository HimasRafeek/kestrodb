#pragma once
// Process-wide worker threads for replay (opening a database). Created on first use
// and intentionally never destroyed: idle workers only wait on a condition variable
// and process exit ends them, whereas joining them from a static destructor can
// deadlock while a DLL (the Node addon) unloads.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace kestro {
class Pool {
public:
  struct Job {
    std::function<void(size_t)> fn;
    size_t n = 0;
    std::atomic<size_t> next{0};
    size_t busy = 0;            // workers inside run(), guarded by Pool::m_
    std::exception_ptr error;   // first failure, guarded by Pool::m_
  };
  static Pool& get() { static Pool* pool = new Pool(); return *pool; }
  size_t workers() const { return threads_; }
  // Runs fn(0..n-1) on the workers while the caller does other work. Indices are
  // claimed in increasing order. finish() (or the destructor) waits for all of them.
  class Batch {
  public:
    Batch(Pool& p, size_t n, std::function<void(size_t)> fn) : pool_(p), serial_(p.serial_) {
      job_ = std::make_shared<Job>();
      job_->fn = std::move(fn); job_->n = n;
      { std::lock_guard<std::mutex> g(pool_.m_); pool_.current_ = job_; pool_.generation_++; }
      pool_.wake_.notify_all();
    }
    // Claims and runs one more index on the calling thread; false when none are left.
    bool help_one() {
      size_t i = job_->next.fetch_add(1);
      if (i >= job_->n) return false;
      try { job_->fn(i); } catch (...) { pool_.fail(*job_); }
      return true;
    }
    void finish() {
      if (done_) return;
      done_ = true;
      while (help_one()) {}
      std::unique_lock<std::mutex> l(pool_.m_);
      pool_.idle_.wait(l, [&] { return job_->busy == 0; });
      if (pool_.current_ == job_) pool_.current_.reset();
      auto error = job_->error;
      l.unlock();
      serial_.unlock();  // the next batch may start now
      if (error) std::rethrow_exception(error);
    }
    ~Batch() { try { finish(); } catch (...) {} }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
  private:
    Pool& pool_;
    std::unique_lock<std::mutex> serial_;  // one batch at a time per process
    std::shared_ptr<Job> job_;
    bool done_ = false;
  };
  // fn(0..n-1), on the workers plus the caller when worthwhile.
  template <class F> void parallel_for(size_t n, bool worthwhile, F&& f) {
    if (!worthwhile || n < 2 || !threads_) { for (size_t i = 0; i < n; i++) f(i); return; }
    Batch b(*this, n, std::forward<F>(f));
    b.finish();
  }
private:
  Pool() {
    size_t want = std::min<size_t>(7, std::max(1u, std::thread::hardware_concurrency()) - 1);
    for (size_t i = 0; i < want; i++) {
      try { std::thread([this] { loop(); }).detach(); threads_++; } catch (...) { break; }
    }
  }
  void fail(Job& j) {
    std::lock_guard<std::mutex> g(m_);
    if (!j.error) j.error = std::current_exception();
    j.next = j.n;
  }
  void run(Job& j) {
    for (size_t i; (i = j.next.fetch_add(1)) < j.n;) {
      try { j.fn(i); } catch (...) { fail(j); return; }
    }
  }
  void loop() {
    uint64_t seen = 0;
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<std::mutex> l(m_);
        wake_.wait(l, [&] { return generation_ != seen; });
        seen = generation_;
        j = current_;
        if (!j) continue;
        j->busy++;
      }
      run(*j);
      { std::lock_guard<std::mutex> g(m_); if (--j->busy == 0) idle_.notify_all(); }
    }
  }
  std::mutex m_, serial_;
  std::condition_variable wake_, idle_;
  std::shared_ptr<Job> current_;
  uint64_t generation_ = 0;
  size_t threads_ = 0;
};
}

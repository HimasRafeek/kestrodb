#pragma once
// One process-wide thread for delayed background work (verification after a snapshot
// open). Like Pool, it is created on first use and never destroyed: it only waits on a
// condition variable, and process exit ends it.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace kestro {
class Background {
public:
  using Clock = std::chrono::steady_clock;
  struct Task {
    std::function<void()> fn;
    Clock::time_point when;
    std::mutex m;
    std::condition_variable cv;
    int state = 0;  // 0 pending, 1 running, 2 done, 3 cancelled
  };
  static Background& get() { static Background* b = new Background(); return *b; }
  // Runs fn on the background thread after delay. If that thread could not be started,
  // the task stays pending: cancel_or_wait drops it and run_or_wait runs it inline.
  std::shared_ptr<Task> schedule(std::chrono::milliseconds delay, std::function<void()> fn) {
    auto t = std::make_shared<Task>();
    t->fn = std::move(fn); t->when = Clock::now() + delay;
    { std::lock_guard<std::mutex> g(m_); queue_.push_back(t); }
    cv_.notify_one();
    return t;
  }
  // Cancels a task that has not started, or waits for a running one to finish.
  static void cancel_or_wait(const std::shared_ptr<Task>& t) {
    if (!t) return;
    std::unique_lock<std::mutex> l(t->m);
    if (t->state == 0) { t->state = 3; return; }
    t->cv.wait(l, [&] { return t->state >= 2; });
  }
  // Starts a pending task now (on the calling thread) or waits for it to finish.
  static void run_or_wait(const std::shared_ptr<Task>& t) {
    if (!t) return;
    std::unique_lock<std::mutex> l(t->m);
    if (t->state == 0) { t->state = 1; l.unlock(); finish(*t); return; }
    t->cv.wait(l, [&] { return t->state >= 2; });
  }
private:
  static void finish(Task& t) {
    try { t.fn(); } catch (...) {}
    std::lock_guard<std::mutex> g(t.m);
    t.state = 2; t.fn = nullptr;
    t.cv.notify_all();
  }
  Background() { try { std::thread([this] { loop(); }).detach(); } catch (...) {} }
  void loop() {
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
      // Drop cancelled tasks, then wait for the earliest pending one to come due.
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const std::shared_ptr<Task>& t) { std::lock_guard<std::mutex> g(t->m); return t->state != 0; }), queue_.end());
      if (queue_.empty()) { cv_.wait(l); continue; }
      auto next = std::min_element(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) { return a->when < b->when; });
      if (Clock::now() < (*next)->when) { cv_.wait_until(l, (*next)->when); continue; }
      auto t = *next; queue_.erase(next);
      { std::lock_guard<std::mutex> g(t->m); if (t->state != 0) continue; t->state = 1; }
      l.unlock(); finish(*t); l.lock();
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<std::shared_ptr<Task>> queue_;
};
}

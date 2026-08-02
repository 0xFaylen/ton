// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "td/utils/Status.h"

namespace ton::validator::parallel_inbound {

// Fixed local worker set for ordered, caller-owned batches. A batch is a
// barrier: run_batch() returns only after every task has completed, and a task
// exception fails the whole batch without terminating a worker. The pool does
// not publish blockchain state and is deliberately independent of scheduling
// or receipt validation.
class ReusableWorkerPool {
 public:
  using Task = std::function<void()>;

  static td::Result<std::unique_ptr<ReusableWorkerPool>> create(std::size_t worker_count);

  ReusableWorkerPool(const ReusableWorkerPool&) = delete;
  ReusableWorkerPool& operator=(const ReusableWorkerPool&) = delete;
  ~ReusableWorkerPool();

  td::Status run_batch(std::vector<Task> tasks);

  std::size_t worker_count() const {
    return worker_count_;
  }

 private:
  explicit ReusableWorkerPool(std::size_t worker_count) : worker_count_(worker_count) {
  }

  td::Status start();
  void shutdown();
  void worker_loop(std::size_t worker_index);

  const std::size_t worker_count_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable batch_done_;
  std::condition_variable workers_ready_;
  std::vector<Task> tasks_;
  std::size_t generation_{0};
  std::size_t tasks_remaining_{0};
  std::size_t ready_workers_{0};
  bool batch_active_{false};
  bool stopping_{false};
  std::string first_task_error_;
};

}  // namespace ton::validator::parallel_inbound

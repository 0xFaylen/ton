// SPDX-License-Identifier: LGPL-2.0-or-later

#include "parallel-worker-pool.h"

#include <exception>

#include "td/utils/StringBuilder.h"

namespace ton::validator::parallel_inbound {

td::Result<std::unique_ptr<ReusableWorkerPool>> ReusableWorkerPool::create(std::size_t worker_count) {
  if (worker_count == 0) {
    return td::Status::Error("reusable worker pool requires at least one worker");
  }
  auto pool = std::unique_ptr<ReusableWorkerPool>(new ReusableWorkerPool(worker_count));
  TRY_STATUS(pool->start());
  return pool;
}

ReusableWorkerPool::~ReusableWorkerPool() {
  shutdown();
}

td::Status ReusableWorkerPool::start() {
  try {
    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
      workers_.emplace_back([this, i] { worker_loop(i); });
    }
  } catch (const std::exception& error) {
    shutdown();
    return td::Status::Error(PSTRING() << "cannot start reusable worker pool: " << error.what());
  } catch (...) {
    shutdown();
    return td::Status::Error("cannot start reusable worker pool");
  }

  std::unique_lock lock(mutex_);
  workers_ready_.wait(lock, [&] { return ready_workers_ == worker_count_; });
  return td::Status::OK();
}

void ReusableWorkerPool::shutdown() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    work_ready_.notify_all();
  }
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

td::Status ReusableWorkerPool::run_batch(std::vector<Task> tasks) {
  if (tasks.empty()) {
    return td::Status::OK();
  }
  if (tasks.size() > worker_count_) {
    return td::Status::Error("reusable worker batch has more tasks than workers");
  }
  for (const auto& task : tasks) {
    if (!task) {
      return td::Status::Error("reusable worker batch contains an empty task");
    }
  }

  std::unique_lock lock(mutex_);
  if (stopping_) {
    return td::Status::Error("reusable worker pool is stopping");
  }
  if (batch_active_) {
    return td::Status::Error("reusable worker pool already has an active batch");
  }
  tasks_ = std::move(tasks);
  tasks_remaining_ = tasks_.size();
  first_task_error_.clear();
  batch_active_ = true;
  ++generation_;
  work_ready_.notify_all();
  batch_done_.wait(lock, [&] { return tasks_remaining_ == 0; });
  batch_active_ = false;
  tasks_.clear();
  if (!first_task_error_.empty()) {
    return td::Status::Error(PSTRING() << "reusable worker task failed: " << first_task_error_);
  }
  return td::Status::OK();
}

void ReusableWorkerPool::worker_loop(std::size_t worker_index) {
  std::unique_lock lock(mutex_);
  ++ready_workers_;
  workers_ready_.notify_one();
  std::size_t observed_generation = generation_;
  while (true) {
    work_ready_.wait(lock, [&] { return stopping_ || generation_ != observed_generation; });
    if (stopping_) {
      return;
    }
    observed_generation = generation_;
    if (worker_index >= tasks_.size()) {
      continue;
    }
    const auto* task = &tasks_[worker_index];
    lock.unlock();
    std::string error_message;
    try {
      (*task)();
    } catch (const std::exception& error) {
      error_message = error.what();
      if (error_message.empty()) {
        error_message = "standard exception without a message";
      }
    } catch (...) {
      error_message = "unknown exception";
    }
    lock.lock();
    if (!error_message.empty() && first_task_error_.empty()) {
      first_task_error_ = std::move(error_message);
    }
    if (--tasks_remaining_ == 0) {
      batch_done_.notify_one();
    }
  }
}

}  // namespace ton::validator::parallel_inbound

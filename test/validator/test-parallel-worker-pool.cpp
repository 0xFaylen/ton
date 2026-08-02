// SPDX-License-Identifier: LGPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "impl/parallel-worker-pool.h"
#include "td/utils/tests.h"

namespace ton::validator::parallel_inbound::test {
namespace {

TEST(ParallelWorkerPool, RejectsInvalidBatches) {
  ASSERT_TRUE(ReusableWorkerPool::create(0).is_error());
  auto pool = ReusableWorkerPool::create(2).move_as_ok();
  ASSERT_TRUE(pool->run_batch({}).is_ok());
  ASSERT_TRUE(pool->run_batch({[] {}, [] {}, [] {}}).is_error());
  ASSERT_TRUE(pool->run_batch({ReusableWorkerPool::Task{}}).is_error());
}

TEST(ParallelWorkerPool, ReusesReadyWorkersAcrossBatches) {
  auto pool = ReusableWorkerPool::create(4).move_as_ok();
  std::set<std::thread::id> first_threads;
  std::set<std::thread::id> second_threads;
  std::mutex thread_mutex;
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};

  auto run = [&](std::set<std::thread::id>& threads) {
    std::vector<ReusableWorkerPool::Task> tasks;
    for (std::size_t i = 0; i < pool->worker_count(); ++i) {
      tasks.push_back([&] {
        {
          std::lock_guard lock(thread_mutex);
          threads.insert(std::this_thread::get_id());
        }
        const auto current = ++active;
        auto observed = max_active.load();
        while (observed < current && !max_active.compare_exchange_weak(observed, current)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --active;
      });
    }
    return pool->run_batch(std::move(tasks));
  };

  ASSERT_TRUE(run(first_threads).is_ok());
  ASSERT_TRUE(run(second_threads).is_ok());
  ASSERT_EQ(first_threads.size(), 4u);
  ASSERT_EQ(second_threads, first_threads);
  ASSERT_EQ(max_active.load(), 4);
}

TEST(ParallelWorkerPool, ContainsTaskFailureAndRemainsReusable) {
  auto pool = ReusableWorkerPool::create(2).move_as_ok();
  std::atomic<int> completed{0};
  auto failed = pool->run_batch({[] { throw std::runtime_error("expected failure"); }, [&] { ++completed; }});
  ASSERT_TRUE(failed.is_error());
  ASSERT_EQ(completed.load(), 1);

  ASSERT_TRUE(pool->run_batch({[&] { ++completed; }, [&] { ++completed; }}).is_ok());
  ASSERT_EQ(completed.load(), 3);
}

TEST(ParallelWorkerPool, KeepsTaskIndexOnTheSameWorker) {
  auto pool = ReusableWorkerPool::create(4).move_as_ok();
  std::vector<std::thread::id> first(pool->worker_count());
  std::vector<std::thread::id> second(pool->worker_count());
  auto capture = [&](std::vector<std::thread::id>& ids) {
    std::vector<ReusableWorkerPool::Task> tasks;
    for (std::size_t index = 0; index < pool->worker_count(); ++index) {
      tasks.push_back([&, index] { ids[index] = std::this_thread::get_id(); });
    }
    return pool->run_batch(std::move(tasks));
  };
  ASSERT_TRUE(capture(first).is_ok());
  ASSERT_TRUE(capture(second).is_ok());
  ASSERT_EQ(second, first);
}

TEST(ParallelWorkerPool, CompletesManyGenerationsWithoutLostWakeups) {
  auto pool = ReusableWorkerPool::create(4).move_as_ok();
  std::atomic<std::size_t> completed{0};
  for (std::size_t generation = 0; generation < 200; ++generation) {
    std::vector<ReusableWorkerPool::Task> tasks;
    for (std::size_t worker = 0; worker < pool->worker_count(); ++worker) {
      tasks.push_back([&] { ++completed; });
    }
    ASSERT_TRUE(pool->run_batch(std::move(tasks)).is_ok());
    ASSERT_EQ(completed.load(), (generation + 1) * pool->worker_count());
  }
}

}  // namespace
}  // namespace ton::validator::parallel_inbound::test

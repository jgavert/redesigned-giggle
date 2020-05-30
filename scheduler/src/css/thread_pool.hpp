#pragma once
#include "css/utils/cpu_info.hpp"
#include "css/utils/lockfree_queue.hpp"
#include "css/utils/dynamic_allocator.hpp"
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
#include <thread>
#include <cassert>
#include <algorithm>
#include <experimental/coroutine>
#include <windows.h>

// define this to enable atomic stat collection
#define CSS_STEALER_COLLECT_STATS
// define to use lockfree version of the queue
#define CSS_STEALER_USE_LOCKFREE_QUEUE

#ifdef CSS_STEALER_COLLECT_STATS
#define CSS_STEALER_STATS_IF if (1)
#else
#define CSS_STEALER_STATS_IF if (0)
#endif

namespace css 
{
namespace internal_locals
{
  extern thread_local bool thread_from_pool;
  extern thread_local int thread_id;
}

class ThreadPool 
{
  struct StackTask
  {
    std::atomic_int* reportCompletion = nullptr;
    std::experimental::coroutine_handle<> handle = {};
    std::vector<std::atomic_int*> childs;
    std::vector<std::atomic_int*> waitQueue;
    size_t atomics_seen = 0;

    // returns the amount of times "resume" can be called
    [[nodiscard]] size_t canExecute() noexcept {
      if (handle.done())
        return 0;
      size_t count = 0;
      for (size_t i = atomics_seen; i < waitQueue.size(); ++i) {
        if (waitQueue[i]->load() > 0)
          return count;
        atomics_seen++;
        count++;
      }
      return count;
    }

    bool done() const noexcept {
      return handle.done();
    }
  };

  // spawned when a coroutine is created
  struct FreeLoot
  {
    std::experimental::coroutine_handle<> handle; // this might spawn childs, becomes host that way.
    std::atomic_int* reportCompletion = nullptr; // when task is done, inform here
  };

  struct StealableQueue
  {
    size_t m_group_id = 0;
  #if defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
    css::parallel::per_thread_queue<FreeLoot, 262144> loot; // get it if you can :smirk:
  #else
    std::deque<FreeLoot> loot;
  #endif
    std::mutex lock; // version 1 stealable queue works with mutex.
    StealableQueue(size_t group_id): m_group_id(group_id){}
    StealableQueue(StealableQueue& other): m_group_id(other.m_group_id){}
    StealableQueue(StealableQueue&& other) noexcept : m_group_id(other.m_group_id) {}
  };

  struct ThreadCoroStack
  {
    std::deque<StackTask> m_coroStack;
    size_t m_stackPointer = 0;

    ThreadCoroStack() {}

    StackTask& current_stack() {
      assert(m_stackPointer > 0);
      return m_coroStack[m_stackPointer-1];
    }
    void push_stack(std::atomic_int* reportCompletion, std::experimental::coroutine_handle<> handle) {
      assert(reportCompletion != nullptr);
      m_stackPointer++;
      if (m_coroStack.size() <= m_stackPointer) {
        m_coroStack.push_back(StackTask());
      }
      auto& task = current_stack();
      task.handle = handle;
      task.reportCompletion = reportCompletion;
      task.childs.clear();
      task.waitQueue.clear();
      task.atomics_seen = 0;
    }
    void pop_stack() {
      assert(m_stackPointer > 0);
      m_stackPointer--;
    }
    bool empty() {
      return m_stackPointer == 0;
    }
  };

  struct ThreadData
  {
    ThreadCoroStack m_coroStack;
    size_t m_id = 0;
    size_t m_wakeThread = 0;
    size_t m_group_id = 0;
    uint64_t m_group_mask = 0;
    DynamicHeapAllocator m_localAllocator;
    ThreadData(){}
    ThreadData(size_t id, size_t group_id, uint64_t group_mask):m_id(id), m_group_id(group_id), m_group_mask(group_mask){}
    ThreadData(ThreadData& other) : m_coroStack(other.m_coroStack), m_id(other.m_id), m_group_id(other.m_group_id), m_group_mask(other.m_group_mask) { assert(false); }
    ThreadData(ThreadData&& other) noexcept
      : m_coroStack(std::move(other.m_coroStack))
      , m_id(other.m_id)
      , m_group_id(other.m_group_id)
      , m_group_mask(other.m_group_mask)
      , m_localAllocator(std::move(other.m_localAllocator)) {
    }

    void initializeAllocator(size_t size) {
      m_localAllocator = DynamicHeapAllocator(static_cast<uint16_t>(m_id), size);
    }
  };

  struct StealStats
  {
    size_t tasks_done = 0;
    size_t tasks_stolen = 0;
    size_t steal_tries = 0;
    size_t tasks_unforked = 0;
    size_t tasks_stolen_within_l3 = 0;
    size_t tasks_stolen_outside_l3 = 0;
  };
  std::vector<ThreadData> m_data;
  std::vector<StealableQueue> m_stealQueues;
  size_t m_threads = 0;
  std::atomic_size_t m_globalTasksLeft = 0;
  std::atomic_int m_doable_tasks = 0;
  std::atomic_int m_thread_sleeping = 0;

  // hmm, to make threads sleep in groups...? No sense to wake threads from outside L3 if L3 isn't awake, perf--
  std::mutex sleepLock;
  std::condition_variable cv;

  std::mutex m_global;
  std::vector<FreeLoot> m_nobodyOwnsTasks; // "global tasks", tasks that "main thread" owns.

  std::atomic_bool m_poolAlive;
  std::vector<std::thread> m_threadHandles;
  // statistics
  std::atomic_size_t m_tasks_done = 0;
  std::atomic_size_t m_tasks_stolen_within_l3 = 0;
  std::atomic_size_t m_tasks_stolen_outside_l3 = 0;
  std::atomic_size_t m_steal_fails = 0;
  std::atomic_size_t m_tasks_unforked = 0;
public:
  StealStats stats() {
    auto stolen = m_tasks_stolen_within_l3 + m_tasks_stolen_outside_l3;
    return {m_tasks_done, stolen, m_steal_fails, m_tasks_unforked, m_tasks_stolen_within_l3, m_tasks_stolen_outside_l3};
  }

  ThreadPool() noexcept {
    m_threads = std::thread::hardware_concurrency();
    SystemCpuInfo info;
    size_t l3threads = info.numas.front().threads / info.numas.front().coreGroups.size();

    m_poolAlive = true;
    auto threadStacksLeft = m_threads;
    // configure thread datas and steal queues.
    for (size_t group = 0; group < info.numas.front().coreGroups.size(); ++group){
      for (size_t t = 0; t < l3threads; t++) {
        if (threadStacksLeft == 0)
          break;
        auto index = group*l3threads + t;
        m_data.emplace_back(index, group, info.numas.front().coreGroups[group].mask);
        m_stealQueues.emplace_back(group);
        threadStacksLeft--;
      }
    }
    // init allocators for each thread.
    for (auto&& it : m_data) {
      it.initializeAllocator(8ull * 1024ull * 1024ull); // per thread allocation granularity.
    }
    m_thread_sleeping = static_cast<int>(m_threads)-1;

    // threads started here
    SetThreadAffinityMask(GetCurrentThread(), m_data[0].m_group_mask);
    for (size_t t = 1; t < m_threads; t++) {
      m_threadHandles.push_back(std::thread(&ThreadPool::thread_loop, this, std::ref(m_data[t])));
      SetThreadAffinityMask(m_threadHandles.back().native_handle(), m_data[t].m_group_mask);
    }
  }

  ~ThreadPool() noexcept {
    m_poolAlive = false;
    m_doable_tasks = static_cast<int>(m_threads)+1;
    cv.notify_all();
    for (auto& it : m_threadHandles)
      it.join();
  }

  inline void wakeThread(ThreadData& thread) noexcept {
    int countToWake = std::min(static_cast<int>(m_doable_tasks.load()), m_thread_sleeping.load());
    if (countToWake > 2)
      cv.notify_all();
    else if (countToWake > 0)
      cv.notify_one();
  }

  std::optional<FreeLoot> stealTask(const ThreadData& thread) noexcept {
    FreeLoot stealed = {};
    for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
    {
      auto& ownQueue = m_stealQueues[index % m_threads];
      if (ownQueue.m_group_id != thread.m_group_id)
        continue;
#if !defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
      std::unique_lock lock(ownQueue.lock);
      if (!ownQueue.loot.empty()) {
        auto freetask = ownQueue.loot.front();
        ownQueue.loot.pop_front();
        CSS_STEALER_STATS_IF m_tasks_stolen_within_l3++;
        return std::optional<FreeLoot>(freetask);
      }
#else
      if (!ownQueue.loot.empty() && ownQueue.loot.pop_front(stealed)) {
        CSS_STEALER_STATS_IF m_tasks_stolen_within_l3++;
        return std::optional<FreeLoot>(stealed);
      }
#endif
    }
    for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
    {
      auto& ownQueue = m_stealQueues[index % m_threads];
      if (ownQueue.m_group_id == thread.m_group_id)
        continue;
#if !defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
      std::unique_lock lock(ownQueue.lock);
      if (!ownQueue.loot.empty()) {
        auto freetask = ownQueue.loot.front();
        ownQueue.loot.pop_front();
        CSS_STEALER_STATS_IF m_tasks_stolen_outside_l3++;
        return std::optional<FreeLoot>(freetask);
      }
#else
      if (!ownQueue.loot.empty() && ownQueue.loot.pop_front(stealed)) {
        CSS_STEALER_STATS_IF m_tasks_stolen_outside_l3++;
        return std::optional<FreeLoot>(stealed);
      }
#endif
    }
    CSS_STEALER_STATS_IF m_steal_fails++;
    return std::optional<FreeLoot>();
  }

  bool unfork(ThreadData& thread, FreeLoot& loot) noexcept {
    auto& myStealQueue = m_stealQueues[thread.m_id];
#if !defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
    std::unique_lock lock(myStealQueue.lock);
    if (myStealQueue.loot.empty())
      return false;
    auto freetask = myStealQueue.loot.back();
    myStealQueue.loot.pop_back();
    CSS_STEALER_STATS_IF m_tasks_unforked++;
    loot = freetask;
    return true;
#else
    if (myStealQueue.loot.pop_back(loot)) {
      CSS_STEALER_STATS_IF m_tasks_unforked++;
      return true;
    }
    return false;
#endif
  }

  void* localAllocate(size_t sz) {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    return m_data[threadID].m_localAllocator.allocate(sz);
  }

  void localFree(void* ptr, size_t sz) {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];
    data.m_localAllocator.deallocate(ptr);
  }

  size_t localQueueSize() {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_stealQueues[threadID];
    return data.loot.size();
  }

  // called by coroutine - from constructor 
  void spawnTask(std::experimental::coroutine_handle<> handle, std::atomic_int* counter) noexcept {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];
    FreeLoot loot{};
    loot.handle = handle;
    assert(counter->load() == 1);
    loot.reportCompletion = counter;
    if (!data.m_coroStack.empty())
    {
      data.m_coroStack.current_stack().childs.push_back(counter);
    }
    else
    {
      // add to global pool for being able to track the source coroutine completion.
      std::lock_guard<std::mutex> guard(m_global);
      m_nobodyOwnsTasks.push_back(loot);
      m_globalTasksLeft++;
    }
    // add task to own queue
    {
#if !defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
      auto& stealQueue = m_stealQueues[threadID];
      std::unique_lock lock(stealQueue.lock);
      stealQueue.loot.push_back(std::move(loot));
#else
      auto& stealQueue = m_stealQueues[threadID];
      stealQueue.loot.push_back(std::move(loot));
#endif
    }
    m_doable_tasks++;
    wakeThread(data);
  }

  // called by coroutine - when entering co_await, handle is what current coroutine is depending from.
  void addDependencyToCurrentTask(std::atomic_int* trackerPtr) noexcept {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];

    assert(trackerPtr != nullptr); // "tracker should be always valid");
    data.m_coroStack.current_stack().waitQueue.push_back(trackerPtr);
  }

  void workOnTasks(ThreadData& myData, StealableQueue& myQueue) noexcept {
    if (!myData.m_coroStack.empty()) {
      auto& task = myData.m_coroStack.current_stack();
      auto executeCount = task.canExecute();
      if (executeCount > 0) {
        for (size_t run = 0; run < executeCount; ++run)
          task.handle.resume();
      }
      if (task.done()) {
        auto* ptr = task.reportCompletion;
        myData.m_coroStack.pop_stack();
        ptr->store(0);
        CSS_STEALER_STATS_IF m_tasks_done++;
      }
      else {
        FreeLoot task = {};
        if (unfork(myData, task)) [[likely]] {
          myData.m_coroStack.push_stack(task.reportCompletion, task.handle);
          m_doable_tasks--;
          myData.m_coroStack.current_stack().handle.resume();
        }
      }
    }
    else if (myData.m_coroStack.empty()) {
      if (auto task = stealTask(myData)) {
        myData.m_coroStack.push_stack(task.value().reportCompletion, task.value().handle);
        m_doable_tasks--;
        myData.m_coroStack.current_stack().handle.resume();
      }
    }
  }

  void thread_loop(ThreadData& myData) noexcept {
    internal_locals::thread_id = static_cast<int>(myData.m_id);
    internal_locals::thread_from_pool = true;
    m_thread_sleeping--;
    auto& myQueue = m_stealQueues[myData.m_id];
    while(m_poolAlive){
      if (auto task = stealTask(myData)) {
        myData.m_coroStack.push_stack(task.value().reportCompletion, task.value().handle);
        m_doable_tasks--;
        myData.m_coroStack.current_stack().handle.resume();
      } else if (myData.m_coroStack.empty() && m_doable_tasks.load() == 0){
        std::unique_lock<std::mutex> lk(sleepLock);
        m_thread_sleeping++;
        cv.wait(lk, [&](){
          return m_doable_tasks.load() > 0;
        });
        m_thread_sleeping--;
      }
      while(!myData.m_coroStack.empty() || !myQueue.loot.empty())
        workOnTasks(myData, myQueue);
    }

    internal_locals::thread_id = -1;
    internal_locals::thread_from_pool = false;
    m_thread_sleeping++;
  }

  std::atomic_int* findWorkToWaitFor() noexcept {
    std::atomic_int* ptr = nullptr;
    {
      std::lock_guard<std::mutex> guard(m_global);
      if (!m_nobodyOwnsTasks.empty())
        ptr = m_nobodyOwnsTasks.back().reportCompletion;
    }
    return ptr;
  }

  void freeCompletedWork(ThreadData& data) noexcept {
    std::lock_guard<std::mutex> guard(m_global);
    while(!m_nobodyOwnsTasks.empty() && m_nobodyOwnsTasks.back().reportCompletion->load() == 0) {
      m_nobodyOwnsTasks.pop_back();
      m_globalTasksLeft--;
    }
  }

  void execute() noexcept {
    auto& myData = m_data[0];
    auto& myQueue = m_stealQueues[0];
    while(m_globalTasksLeft > 0) {
      std::atomic_int* wait = findWorkToWaitFor();
      while (wait && wait->load() > 0) {
        workOnTasks(myData, myQueue);
      }
      freeCompletedWork(myData);
    }
  }
};
  void createThreadPool();
  extern std::unique_ptr<ThreadPool> s_stealPool;
}
#pragma once
#include "scheduler/coroutine/cpu_info.hpp"
#include "scheduler/helpers/freelist.hpp"
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
#include <thread>
#include <cassert>
#include <algorithm>
#include <coroutine>
#include <windows.h>

// stealer always keeps handles alive until it can prove that all dependencies are done...

// define this to enable atomic stat collection
#define STEALER_COLLECT_STATS

#ifdef STEALER_COLLECT_STATS
#define STEALER_STATS_IF if (1)
#else
#define STEALER_STATS_IF if (0)
#endif


namespace taskstealer_v1
{
struct StackTask
{
  std::atomic_int* reportCompletion = nullptr;
  std::coroutine_handle<> handle;
  std::vector<std::atomic_int*> childs;
  std::deque<std::atomic_int*> waitQueue; // handle address that is waited to be complete, so that handle can continue.

  // returns the amount of times "resume" can be called
  // and how many elements can be removed from waitQueue
  [[nodiscard]] size_t canExecute() const noexcept {
    if (handle.done())
      return 0;
    size_t count = 0;
    for (auto&& it : waitQueue) {
      if (it->load() > 0)
        return count;
      count++;
    }
    return count;
  }

  bool done() const noexcept {
    if (!waitQueue.empty())
      return false;
    for (auto&& child : childs) 
      if (child != nullptr && child->load() > 0)
        return false;
    return handle.done();
  }
};

// spawned when a coroutine is created
struct FreeLoot
{
  std::coroutine_handle<> handle; // this might spawn childs, becomes host that way.
  std::atomic_int* reportCompletion; // when task is done, inform here
  // I thought of separate queue where to add "completed tasks", but using atomics for icity.
};

struct StealableQueue
{
  size_t m_group_id = 0;
  std::deque<FreeLoot> loot; // get it if you can :smirk:
  std::mutex lock; // version 1 stealable queue works with mutex.
  StealableQueue(size_t group_id): m_group_id(group_id){}
  StealableQueue(StealableQueue& other): m_group_id(other.m_group_id){}
  StealableQueue(StealableQueue&& other) noexcept : m_group_id(other.m_group_id) {}
};

struct ThreadCoroStack
{
  std::deque<StackTask> m_coroStack;
  size_t m_stackPointer = 0;

  StackTask& current_stack() {
    assert(m_stackPointer > 0);
    return m_coroStack[m_stackPointer-1];
  }
  void push_stack(std::atomic_int* reportCompletion, std::coroutine_handle<> handle) {
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
  AllocatingFreelist<std::atomic_int> m_atomics;
  size_t m_id = 0;
  size_t m_wakeThread = 0;
  size_t m_group_id = 0;
  uint64_t m_group_mask = 0;
  ThreadData(){}
  ThreadData(size_t id, size_t group_id, uint64_t group_mask):m_id(id), m_group_id(group_id), m_group_mask(group_mask){}
  ThreadData(ThreadData& other) : m_coroStack(other.m_coroStack), m_id(other.m_id), m_group_id(other.m_group_id), m_group_mask(other.m_group_mask) { assert(false); }
  ThreadData(ThreadData&& other) noexcept
    : m_coroStack(std::move(other.m_coroStack))
    , m_atomics(std::move(other.m_atomics))
    , m_id(other.m_id)
    , m_group_id(other.m_group_id)
    , m_group_mask(other.m_group_mask) {}
};
namespace locals
{
  extern thread_local bool thread_from_pool;
  extern thread_local int thread_id;
}

struct StealStats
{
  size_t tasks_done = 0;
  size_t tasks_stolen = 0;
  size_t steal_tries = 0;
  size_t tasks_unforked = 0;
  size_t tasks_stolen_within_l3 = 0;
  size_t tasks_stolen_outside_l3 = 0;
};

class ThreadPool 
{
  // there is only single thread so this is simple
  std::vector<ThreadData> m_data;
  std::vector<StealableQueue> m_stealQueues; // separate to avoid false sharing
  size_t m_threads = 0;
  std::atomic_size_t m_globalTasksLeft = 0;
  std::atomic_size_t m_doable_tasks = 0;
  std::atomic_size_t m_thread_sleeping = 0;

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
    m_thread_sleeping = m_threads-1;
    for (size_t t = 1; t < m_threads; t++) {
      m_threadHandles.push_back(std::thread(&ThreadPool::thread_loop, this, std::ref(m_data[t])));
      SetThreadAffinityMask(m_threadHandles.back().native_handle(), m_data[t].m_group_mask);
    }
  }
  ~ThreadPool() noexcept {
    m_poolAlive = false;
    m_doable_tasks = m_threads+1;
    cv.notify_all();
    for (auto& it : m_threadHandles)
      it.join();
  }

  void wakeThread(ThreadData& thread) noexcept {
    size_t countToWake = std::max(std::min(0ull, m_doable_tasks.load()), m_thread_sleeping.load());
    if (countToWake < 2)
      cv.notify_one();
    else
      cv.notify_all();
  }

  std::optional<FreeLoot> stealTask(const ThreadData& thread) noexcept {
    if (m_doable_tasks > 0)
    {
      for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
      {
        auto& ownQueue = m_stealQueues[index % m_threads];
        if (ownQueue.m_group_id != thread.m_group_id)
          continue;
        std::unique_lock lock(ownQueue.lock);
        if (!ownQueue.loot.empty()) {
          auto freetask = ownQueue.loot.front();
          ownQueue.loot.pop_front();
          STEALER_STATS_IF m_tasks_stolen_within_l3++;
          return std::optional<FreeLoot>(freetask);
        }
      }
      for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
      {
        auto& ownQueue = m_stealQueues[index % m_threads];
        if (ownQueue.m_group_id == thread.m_group_id)
          continue;
        std::unique_lock lock(ownQueue.lock);
        if (!ownQueue.loot.empty()) {
          auto freetask = ownQueue.loot.front();
          ownQueue.loot.pop_front();
          STEALER_STATS_IF m_tasks_stolen_outside_l3++;
          return std::optional<FreeLoot>(freetask);
        }
      }
    }
    STEALER_STATS_IF m_steal_fails++;
    return std::optional<FreeLoot>();
  }

  std::optional<FreeLoot> unfork(ThreadData& thread) noexcept {
    auto& myStealQueue = m_stealQueues[thread.m_id];
    std::unique_lock lock(myStealQueue.lock);
    if (myStealQueue.loot.empty())
      return std::optional<FreeLoot>();
    auto freetask = myStealQueue.loot.back();
    myStealQueue.loot.pop_back();
    STEALER_STATS_IF m_tasks_unforked++;
    return std::optional<FreeLoot>(freetask);
  }

  // called by coroutine - from constructor 
  [[nodiscard]] uintptr_t spawnTask(std::coroutine_handle<> handle) noexcept {
    size_t threadID = static_cast<size_t>(locals::thread_id);
    if (!locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];
    FreeLoot loot{};
    loot.handle = handle;
    std::atomic_int* counter = data.m_atomics.allocate();
    counter->store(1);
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
      auto& stealQueue = m_stealQueues[threadID];
      std::unique_lock lock(stealQueue.lock);
      stealQueue.loot.push_back(std::move(loot));
    }
    m_doable_tasks++;
    wakeThread(data);
    return reinterpret_cast<uintptr_t>(counter);
  }

  // called by coroutine - when entering co_await, handle is what current coroutine is depending from.
  void addDependencyToCurrentTask(uintptr_t trackerPtr) noexcept {
    size_t threadID = static_cast<size_t>(locals::thread_id);
    if (!locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];

    assert(trackerPtr != 0); // "tracker should be always valid");
    std::atomic_int* tracker = reinterpret_cast<std::atomic_int*>(trackerPtr);
    data.m_coroStack.current_stack().waitQueue.push_back(tracker);
  }

  // call after all handles are allowed to be killed.
  void updateStack(ThreadData& data, size_t allowedQueueToClear) {
    auto& task = data.m_coroStack.current_stack();
    while (allowedQueueToClear > 0) {
      auto atomPtr = task.waitQueue.front();
      task.waitQueue.pop_front();
      for (auto&& it : task.childs)
        if (it == atomPtr)
        {
          data.m_atomics.release(it);
          it = nullptr;
          break;
        }
      allowedQueueToClear--;
    }
  }

  void workOnTasks(ThreadData& myData, StealableQueue& myQueue) noexcept {
    if (!myData.m_coroStack.empty()) {
      auto& task = myData.m_coroStack.current_stack();
      auto executeCount = task.canExecute();
      if (executeCount > 0) {
        for (size_t run = 0; run < executeCount; ++run)
          task.handle.resume();
        updateStack(myData, executeCount);
      }
      if (task.done()) {
          auto* ptr = task.reportCompletion;
          assert(task.waitQueue.empty());
          for (auto&& it : myData.m_coroStack.current_stack().childs) {
            if (it != nullptr)
              myData.m_atomics.release(it);
          }
          myData.m_coroStack.pop_stack();
          ptr->store(0);
          STEALER_STATS_IF m_tasks_done++;
      }
      else {
        if (auto task = unfork(myData)) {
          myData.m_coroStack.push_stack(task.value().reportCompletion, task.value().handle);
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
    locals::thread_id = static_cast<int>(myData.m_id);
    locals::thread_from_pool = true;
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

    locals::thread_id = -1;
    locals::thread_from_pool = false;
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
      // illegal to destroy handles here, otherwise we cannot get the value from within the handle to the caller.
      data.m_atomics.release(m_nobodyOwnsTasks.back().reportCompletion);
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
namespace globals
{
  void createThreadPool();
  extern std::unique_ptr<ThreadPool> s_stealPool;
}
}
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
#include <chrono>
#include <coroutine>
#include <windows.h>

// define this to enable atomic stat collection
#define CSS_STEALER_COLLECT_STATS
// define to use lockfree version of the queue
#define CSS_STEALER_USE_LOCKFREE_QUEUE

#define CSS_COLLECT_THREAD_AWAKE_INFORMATION

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
  extern thread_local int thread_parallelStackID;
}
struct TimeStats {
  std::vector<size_t> threads;
  size_t max_time_active = 1;
  size_t total_time_active = 0;

  inline double totalCpuPercentage() {
    const auto sz = size();
    return double(total_time_active) / double(max_time_active * (sz > 0? sz : 1));
  }

  inline double thread(size_t i) {
    return double(threads[i]) / double(max_time_active);
  }

  inline size_t size() const {
    return threads.size();
  }
};
enum class Priority
{
  Default,
  LowPriority,
  All,
  ReverseAll
};

class ThreadPool
{
  struct ThreadTiming {
    std::unique_ptr<std::atomic_uint64_t> time_before;
    std::unique_ptr<std::atomic_uint64_t> time_active;
    std::unique_ptr<std::atomic_bool> active;
  };
  struct Statistics {
    std::vector<ThreadTiming> m_threads;
    std::unique_ptr<std::atomic_uint64_t> time_before;
  };
  struct StackTask
  {
    std::atomic_int* reportCompletion = nullptr;
    std::coroutine_handle<> handle = {};
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
    std::coroutine_handle<> handle; // this might spawn childs, becomes host that way.
    std::atomic_int* reportCompletion = nullptr; // when task is done, inform here
    int source_stack_id = 0;
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
    StealableQueue(size_t group_id) : m_group_id(group_id) {}
    StealableQueue(StealableQueue& other) : m_group_id(other.m_group_id) {}
    StealableQueue(StealableQueue&& other) noexcept : m_group_id(other.m_group_id) {}
  };

  struct ThreadCoroStack
  {
    std::deque<StackTask> m_coroStack;
    size_t m_stackPointer = 0;
    Priority m_priority = Priority::Default;
    Priority m_forcedPriority = Priority::All;

    int64_t tasksDone = 0; // below zero for lowprio tasks and + for prio tasks

    ThreadCoroStack() {}

    StackTask& current_stack() {
      assert(m_stackPointer > 0);
      return m_coroStack[m_stackPointer - 1];
    }
    void push_stack(std::atomic_int* reportCompletion, std::coroutine_handle<> handle, Priority taskPrio) {
      assert(reportCompletion != nullptr);
      if (empty()) m_priority = taskPrio;
      //assert(m_priority == taskPrio);
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
      tasksDone += (m_priority == Priority::Default) ? 1 : -10;
    }
    bool empty() const {
      return m_stackPointer == 0;
    }
    // basically tasksDone balances that normal tasks are done more often than low priority tasks
    // this allows low priority tasks to be spammed so that they never end, but normal tasks that get added later still get prioritised over them.
    // TODO: If there is only one kind of work, this will cause the other queues to be checked always first which might be empty... therefore unnecessary
    Priority allowedPriority() const {
      if (m_forcedPriority != Priority::All) return m_forcedPriority;
      if (empty()){
        return (tasksDone < 0) ? Priority::All : Priority::ReverseAll;
      }
      return m_priority;
    }
    Priority preferUnforkPriority() const {
      return (tasksDone < 0) ? Priority::Default : Priority::LowPriority;
    }
  };

  // idea: have more than 1 ThreadCoroStack per thread
  struct ParallelStacks
  {
      std::deque<ThreadCoroStack> m_stacks;
      Priority m_priority = Priority::Default;
      Priority m_forcedPriority = Priority::All;
        
      ParallelStacks() {
          m_stacks.push_back(ThreadCoroStack());
      }

      int searchEmptyStack() const {
          for (size_t id = 0; id < m_stacks.size(); id++) {
              if (m_stacks[id].empty())
                  return static_cast<int>(id);
          }
          return -1;
      }

      bool allEmpty() const {
          for (auto& it : m_stacks) {
              if (!it.empty())
                  return false;
          }
          return true;
      }

      int64_t tasksDone() const {
          int64_t val = 0;
          for (auto& it : m_stacks) {
              val += it.tasksDone;
          }
          return val;
      }

      Priority allowedPriority() const {
          if (m_forcedPriority != Priority::All) return m_forcedPriority;
          if (allEmpty()) {
              return (tasksDone() < 0) ? Priority::All : Priority::ReverseAll;
          }
          return m_priority;
      }
      Priority preferUnforkPriority() const {
          return (tasksDone() < 0) ? Priority::Default : Priority::LowPriority;
      }
  };

  struct ThreadData
  {
    ParallelStacks m_coroStack;
    std::vector<size_t> m_l3friends;
    std::vector<size_t> m_outsiders;
    size_t m_id = 0;
    size_t m_wakeThread = 0;
    size_t m_group_id = 0;
    uint64_t m_group_mask = 0;
    DynamicHeapAllocator m_localAllocator;
    ThreadData() {}
    ThreadData(size_t id, size_t group_id, uint64_t group_mask, std::vector<size_t> l3friends, std::vector<size_t> outsiders) : m_l3friends(l3friends), m_outsiders(outsiders), m_id(id), m_group_id(group_id), m_group_mask(group_mask) {}
    ThreadData(ThreadData& other) : m_coroStack(other.m_coroStack), m_l3friends(other.m_l3friends), m_outsiders(other.m_outsiders), m_id(other.m_id), m_group_id(other.m_group_id), m_group_mask(other.m_group_mask) { assert(false); }
    ThreadData(ThreadData&& other) noexcept
      : m_coroStack(std::move(other.m_coroStack))
      , m_l3friends(std::move(other.m_l3friends))
      , m_outsiders(std::move(other.m_outsiders))
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
  std::vector<StealableQueue> m_stealQueuesLowPriority;
  size_t m_threads = 0;
  std::atomic_size_t m_globalTasksLeft = 0;
  std::atomic_int m_doable_tasks = 0;
  std::atomic_int m_thread_sleeping = 0;

  // hmm, to make threads sleep in groups...? No sense to wake threads from outside L3 if L3 isn't awake, perf--
  std::mutex sleepLock;
  std::condition_variable cv;

  std::atomic_bool m_poolAlive;
  std::vector<std::thread> m_threadHandles;
  // statistics
  std::atomic_size_t m_tasks_done = 0;
  std::atomic_size_t m_tasks_stolen_within_l3 = 0;
  std::atomic_size_t m_tasks_stolen_outside_l3 = 0;
  std::atomic_size_t m_steal_fails = 0;
  std::atomic_size_t m_tasks_unforked = 0;
  Statistics m_time_active;
public:
  // not thread safe, call only from one thread at a time.
  TimeStats threadUsage() {
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    uint64_t currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto maxValue = currentTime - m_time_active.time_before->load();
    m_time_active.time_before->store(currentTime);

    TimeStats stats{};
    auto count = m_time_active.m_threads.size();
    stats.threads.resize(count);
    stats.max_time_active = maxValue;

    for (size_t i = 0; i < count; ++i) {
      auto& thrd = m_time_active.m_threads[i];
      auto val = thrd.time_active->load();
      thrd.time_active->fetch_sub(val);

      if (thrd.active->load()) {
        auto bef = thrd.time_before->load();
        val += currentTime - bef;
        thrd.time_before->store(currentTime);
      }
        
      stats.threads[i] = val;
      stats.total_time_active += stats.threads[i];
    }
    return stats;
#else
    return TimeStats{};
#endif
  }

  StealStats stats() {
    auto stolen = m_tasks_stolen_within_l3 + m_tasks_stolen_outside_l3;
    return { m_tasks_done, stolen, m_steal_fails, m_tasks_unforked, m_tasks_stolen_within_l3, m_tasks_stolen_outside_l3 };
  }

  ThreadPool() noexcept {
      m_threads = 1;// std::thread::hardware_concurrency() / 2;
    SystemCpuInfo info;
    size_t l3threads = info.numas.front().threads / info.numas.front().coreGroups.size();

    m_poolAlive = true;
    auto threadStacksLeft = m_threads;
    // configure thread datas and steal queues.
    size_t maxThreadCount = std::max(m_threads, (info.numas.front().coreGroups.size() - 1) * l3threads);
    for (size_t group = 0; group < info.numas.front().coreGroups.size(); ++group) {
      for (size_t t = 0; t < l3threads; t++) {
        if (threadStacksLeft == 0)
          break;
        auto index = group * l3threads + t;
        std::vector<size_t> l3Friends;
        for (size_t f = t; f < l3threads + t; f++) {
            auto calcIndex = group * l3threads + f % l3threads;
            if (calcIndex != index)
                l3Friends.push_back(calcIndex);
        }
        std::vector<size_t> outsiders;
        for (size_t f = t; f < maxThreadCount + t; f++)
        {
            auto calcIndex = f % maxThreadCount;
            if (calcIndex == index)
                continue;
            for (auto l3friend : l3Friends) {
                if (calcIndex == l3friend)
                    continue;
            }
            outsiders.push_back(calcIndex);
        }

        m_data.emplace_back(index, group, info.numas.front().coreGroups[group].mask, l3Friends, outsiders);
        m_stealQueues.emplace_back(group);
        m_stealQueuesLowPriority.emplace_back(group);
        threadStacksLeft--;
      }
    }
    // init allocators for each thread.
    for (auto&& it : m_data) {
      it.initializeAllocator(8ull * 1024ull * 1024ull); // per thread allocation granularity.
    }
    m_thread_sleeping = static_cast<int>(m_threads) - 1;

#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    m_time_active.time_before = std::make_unique<std::atomic_uint64_t>(0);
    for (size_t t = 0; t < m_threads; t++) {
      m_time_active.m_threads.push_back(ThreadTiming{ std::make_unique<std::atomic_uint64_t>(0), std::make_unique<std::atomic_uint64_t>(0), std::make_unique<std::atomic_bool>(false) });
    }
    m_time_active.time_before->store(std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif

    // threads started here
    SetThreadAffinityMask(GetCurrentThread(), m_data[0].m_group_mask);
    for (size_t t = 1; t < m_threads; t++) {
      m_threadHandles.push_back(std::thread(&ThreadPool::thread_loop, this, std::ref(m_data[t])));
      SetThreadAffinityMask(m_threadHandles.back().native_handle(), m_data[t].m_group_mask);
    }
  }

  ~ThreadPool() noexcept {
    m_poolAlive = false;
    m_doable_tasks = static_cast<int>(m_threads) + 1;
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

  // actual task stealing implemented here
  std::optional<FreeLoot> stealTaskInternal(std::vector<css::ThreadPool::StealableQueue>& stealQueues, const ThreadData& thread) noexcept {
    FreeLoot stealed = {};
    //for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
    for (size_t index : thread.m_l3friends)
    {
      auto& ownQueue = stealQueues[index % m_threads];
      //if (ownQueue.m_group_id != thread.m_group_id)
      //  continue;
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
    //for (size_t index = thread.m_id; index < thread.m_id + m_threads; index++)
    for (size_t index : thread.m_outsiders)
    {
      auto& ownQueue = stealQueues[index % m_threads];
      //if (ownQueue.m_group_id == thread.m_group_id)
      //  continue;
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

  // handles the priority and calls internalStealTask
  std::optional<FreeLoot> stealTask(const ThreadData& thread, Priority& priority) noexcept {
    std::optional<FreeLoot> loot;
    Priority allowedPriority = thread.m_coroStack.allowedPriority();
    if (allowedPriority == Priority::ReverseAll) {
      loot = stealTaskInternal(m_stealQueuesLowPriority, thread);
      if (loot) {
        priority = Priority::LowPriority;
        return loot;
      }
    }
    if (allowedPriority == Priority::All
      || allowedPriority == Priority::ReverseAll
      || allowedPriority == Priority::Default) {
      loot = stealTaskInternal(m_stealQueues, thread);
      if (loot) {
        priority = Priority::Default;
        return loot;
      }
    }
    if (allowedPriority == Priority::All
      || allowedPriority == Priority::LowPriority) {
      loot = stealTaskInternal(m_stealQueuesLowPriority, thread);
      priority = Priority::LowPriority;
    }
    return loot;
  }

  bool unforkInternal(css::ThreadPool::StealableQueue& myStealQueue, FreeLoot& loot) noexcept {
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

  // Handles mostly priority, calls unforkInternal
  bool unfork(int threadID, ParallelStacks& paraStack, FreeLoot& loot, Priority& currentPriority) noexcept {
    bool found = false;
    Priority allowedPriority = paraStack.allowedPriority();
    if (m_threads == 1 && (allowedPriority == Priority::Default
      || allowedPriority == Priority::LowPriority)) {
      allowedPriority = paraStack.preferUnforkPriority();
    }
    // if priority is reversed, check lowPrio first??
    if (paraStack.m_forcedPriority == Priority::All &&
    (allowedPriority == Priority::ReverseAll
     || allowedPriority == Priority::LowPriority)) {
      found = unforkInternal(m_stealQueuesLowPriority[threadID], loot);
      if (found) {
        currentPriority = Priority::LowPriority;
        return found;
      }
    }
    // normal
    if (paraStack.m_forcedPriority == Priority::All
    || paraStack.m_forcedPriority == Priority::Default
    || (m_threads == 1 && allowedPriority == Priority::LowPriority)) {
      found = unforkInternal(m_stealQueues[threadID], loot);
      if (found){
        currentPriority = Priority::Default;
        return found;
      }
    }
    // low priority last
    if ((paraStack.m_forcedPriority == Priority::All
           && (allowedPriority == Priority::All
               || (m_threads == 1 && allowedPriority == Priority::Default)))
       || paraStack.m_forcedPriority == Priority::LowPriority) {
      found = unforkInternal(m_stealQueuesLowPriority[threadID], loot);
      currentPriority = Priority::LowPriority;
    }
    return found;
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
  void spawnTask(std::coroutine_handle<> handle, std::atomic_int* counter, Priority priority) noexcept {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    int activeStackID = internal_locals::thread_parallelStackID;
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];
    FreeLoot loot{};
    loot.handle = handle;
    assert(counter->load() == 1);
    loot.reportCompletion = counter;
    loot.source_stack_id = activeStackID;
    // This check basically checks, do we add the spawned task as dependency to task it was spawned from
    // basically normal task spawns a lowPrio task which is long lived and lives outside the normal task scope
    // technically it shouldn't be handled through priority, but rather is there execution dependency or not
    auto& stack = data.m_coroStack.m_stacks[activeStackID];
    if (!stack.empty() && Priority::Default == priority)
    {
        stack.current_stack().childs.push_back(counter);
    }
    else {
        //loot.source_stack_id = -1; // not part of stack above? Avoid associating with any existing stacks.
    }
    // add task to own queue
    {
      css::ThreadPool::StealableQueue* stealQueue = nullptr;
      if (priority == Priority::Default)
        stealQueue = &m_stealQueues[threadID];
      else 
        stealQueue = &m_stealQueuesLowPriority[threadID];

#if !defined(CSS_STEALER_USE_LOCKFREE_QUEUE)
      std::unique_lock lock(stealQueue->lock);
      stealQueue->loot.push_back(std::move(loot));
#else
      stealQueue->loot.push_back(std::move(loot));
#endif
    }
    m_doable_tasks++;
    wakeThread(data);
  }

  // called by coroutine - when entering co_await, handle is what current coroutine is depending from.
  void addDependencyToCurrentTask(std::atomic_int* trackerPtr) noexcept {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    int activeStackID = internal_locals::thread_parallelStackID;
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    auto& data = m_data[threadID];

    assert(trackerPtr != nullptr); // "tracker should be always valid");
    data.m_coroStack.m_stacks[activeStackID].current_stack().waitQueue.push_back(trackerPtr);
  }

  void workOnTasks(ThreadData& myData, StealableQueue& myQueue) noexcept {
      // goals, loop through all tasks and try to do a little bit of all of them
      // if succeed, nice
      // if no task, create another stack and steal a task!

#if 1 // new
      // try to do a task
      bool idle = true;
      for (size_t stackID = 0; stackID < myData.m_coroStack.m_stacks.size(); stackID++) {
          auto& currentStack = myData.m_coroStack.m_stacks[stackID];
          if (!currentStack.empty()) { // has a stack that hasn't finished
              internal_locals::thread_parallelStackID = static_cast<int>(stackID); // set stack id as active for work
              auto& task = currentStack.current_stack();
              auto executeCount = task.canExecute();
              if (executeCount > 0) {
                  idle = false;
                  for (size_t run = 0; run < executeCount; ++run)
                      task.handle.resume();
              }
              if (task.done()) { // !! a coroutine was finished, can pop it from stack
                  auto* ptr = task.reportCompletion;
                  currentStack.pop_stack();
                  ptr->store(0);
                  CSS_STEALER_STATS_IF m_tasks_done++;
              }
          }
      }
#define ALWAYS_LOOK_INSIDE_OWN_QUEUE
#if defined(ALWAYS_LOOK_INSIDE_OWN_QUEUE)
      if (idle) {
          FreeLoot task = {};
          Priority priority;
          if (unfork(static_cast<int>(myData.m_id), myData.m_coroStack, task, priority)) [[likely]] {
              // it's guaranteed that stealing from own queue is part this threads stacks
              int stackID = task.source_stack_id;
              auto& parallelStack = myData.m_coroStack.m_stacks[stackID];
              parallelStack.push_stack(task.reportCompletion, task.handle, priority);
              m_doable_tasks--;
              // need to always work once for a new task
              internal_locals::thread_parallelStackID = static_cast<int>(stackID);
              parallelStack.current_stack().handle.resume();
              idle = false;
          }
      }
#endif
      int freeStackID = myData.m_coroStack.searchEmptyStack();
      if (idle && (myData.m_coroStack.m_stacks.size() <= 4 || freeStackID != -1)) { // managed to do nothing useful above
          // check own queue for a task
          FreeLoot task = {};
          Priority priority;
#if !defined(ALWAYS_LOOK_INSIDE_OWN_QUEUE)
          if (unfork(static_cast<int>(myData.m_id), myData.m_coroStack, task, priority)) [[likely]] {
              // it's guaranteed that stealing from own queue is part this threads stacks
              int stackID = -1;
              if (task.source_stack_id >= 0) { // it's a normal task
                  stackID = task.source_stack_id;
              }
              else { // needs a new stack
                  stackID = myData.m_coroStack.searchEmptyStack();
                  if (stackID == -1) { // cannot reuse, needs a new stack
                      stackID = static_cast<int>(myData.m_coroStack.m_stacks.size());
                      myData.m_coroStack.m_stacks.push_back(ThreadCoroStack());
                  }
              }
              auto& parallelStack = myData.m_coroStack.m_stacks[stackID];
              parallelStack.push_stack(task.reportCompletion, task.handle, priority);
              m_doable_tasks--;
              // need to always work once for a new task
              internal_locals::thread_parallelStackID = static_cast<int>(stackID);
              parallelStack.current_stack().handle.resume();
          }
          else
#endif
          if (auto task = stealTask(myData, priority)) { // didn't have task in own queue, steal! 
              auto stackID = myData.m_coroStack.searchEmptyStack();
              if (stackID == -1) { // cannot reuse, needs a new stack
                  stackID = static_cast<int>(myData.m_coroStack.m_stacks.size());
                  myData.m_coroStack.m_stacks.push_back(ThreadCoroStack());
              }
              auto& parallelStack = myData.m_coroStack.m_stacks[stackID];
              parallelStack.push_stack(task.value().reportCompletion, task.value().handle, priority);
              // need to always work once for a new task
              m_doable_tasks--;
              internal_locals::thread_parallelStackID = static_cast<int>(stackID);
              parallelStack.current_stack().handle.resume();
          }
      }
      // try to prune! risky!
      //int stackID = static_cast<int>(myData.m_coroStack.m_stacks.size()) - 1;
      //while (stackID > 0 && myData.m_coroStack.m_stacks[stackID].empty()) {
      //    myData.m_coroStack.m_stacks.pop_back();
      //}
#else // old
      bool hadWork = false;
      if (!myData.m_coroStack.allEmpty()) {
          for (size_t stackID = 0; stackID < myData.m_coroStack.m_stacks.size(); stackID++) {
              if (!myData.m_coroStack.m_stacks[stackID].empty()) {
                  internal_locals::thread_parallelStackID = static_cast<int>(stackID);
                  auto& currentStack = myData.m_coroStack.m_stacks[stackID];
                  auto& task = currentStack.current_stack();
                  auto executeCount = task.canExecute();
                  if (executeCount > 0) {
                      for (size_t run = 0; run < executeCount; ++run)
                          task.handle.resume();
                  }
                  if (task.done()) {
                      auto* ptr = task.reportCompletion;
                      currentStack.pop_stack();
                      ptr->store(0);
                      CSS_STEALER_STATS_IF m_tasks_done++;
                  }
                  else {
                      FreeLoot task = {};
                      Priority priority;
                      if (unfork(myData.m_id, myData.m_coroStack, task, priority)) [[likely]] {
                          currentStack.push_stack(task.reportCompletion, task.handle, priority);
                          m_doable_tasks--;
                          currentStack.current_stack().handle.resume();
                          }
                  }
              }
          }
      }
      else if (myData.m_coroStack.allEmpty()) {
          Priority priority;
          if (auto task = stealTask(myData, priority)) {
              myData.m_coroStack.m_stacks[task.value().stack_id].push_stack(task.value().reportCompletion, task.value().handle, priority);
              m_doable_tasks--;
              myData.m_coroStack.m_stacks[task.value().stack_id].current_stack().handle.resume();
          }
      }
#endif
  }

  void thread_loop(ThreadData& myData) noexcept {
    internal_locals::thread_id = static_cast<int>(myData.m_id);
    internal_locals::thread_from_pool = true;
    m_thread_sleeping--;
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    {
      auto currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
      m_time_active.m_threads[myData.m_id].time_before->store(currentTime);
      m_time_active.m_threads[myData.m_id].active->store(true);
    }
#endif
    auto& myQueue = m_stealQueues[myData.m_id];
    while (m_poolAlive) {
      Priority priority;
      if (auto task = stealTask(myData, priority)) {
        auto& stack = myData.m_coroStack.m_stacks[0];
        stack.push_stack(task.value().reportCompletion, task.value().handle, priority);
        m_doable_tasks--;
        internal_locals::thread_parallelStackID = static_cast<int>(0);
        stack.current_stack().handle.resume();
      }
      else if (myData.m_coroStack.allEmpty() && m_doable_tasks.load() == 0) {
        std::unique_lock<std::mutex> lk(sleepLock);
        m_thread_sleeping++;
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
        {
          m_time_active.m_threads[myData.m_id].active->store(false);
          auto currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
          auto before = m_time_active.m_threads[myData.m_id].time_before->load();
          m_time_active.m_threads[myData.m_id].time_active->fetch_add(currentTime - before);
        }
#endif
        cv.wait(lk, [&]() {
          return m_doable_tasks.load() > 0;
          });
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
        {
          auto currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
          m_time_active.m_threads[myData.m_id].time_before->store(currentTime);
          m_time_active.m_threads[myData.m_id].active->store(true);
        }
#endif
        m_thread_sleeping--;
      }
      while (!myData.m_coroStack.allEmpty() || !myQueue.loot.empty())
        workOnTasks(myData, myQueue);
    }

    internal_locals::thread_id = -1;
    internal_locals::thread_from_pool = false;
    m_thread_sleeping++;
  }

  void execute(std::atomic_int* wait) noexcept {
    auto& myData = m_data[0];
    auto& myQueue = m_stealQueues[0];
    const bool alone = m_threads == 1;
    myData.m_coroStack.m_forcedPriority = alone ? Priority::All : Priority::Default;
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    auto currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    m_time_active.m_threads[myData.m_id].time_before->store(currentTime);
    m_time_active.m_threads[myData.m_id].active->store(true);
#endif
    while (wait && wait->load() > 0) {
      workOnTasks(myData, myQueue);
    }
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    {
        m_time_active.m_threads[myData.m_id].active->store(false);
        auto currentTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        auto before = m_time_active.m_threads[myData.m_id].time_before->load();
        m_time_active.m_threads[myData.m_id].time_active->fetch_add(currentTime - before);
    }
#endif
  }

  void executeFor(size_t microSeconds) noexcept {
    auto& myData = m_data[0];
    auto& myQueue = m_stealQueues[0];
    const bool alone = m_threads == 1;
    myData.m_coroStack.m_forcedPriority = alone ? Priority::All : Priority::Default;
    auto currentTime = std::chrono::high_resolution_clock::now();
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    m_time_active.m_threads[myData.m_id].time_before->store(currentTime.time_since_epoch().count());
    m_time_active.m_threads[myData.m_id].active->store(true);
#endif
    auto endTime = currentTime + std::chrono::microseconds(microSeconds);
    auto safeSleepUntil = endTime - std::chrono::microseconds(1000);
    while (endTime > currentTime) {
      currentTime = std::chrono::high_resolution_clock::now();
      if (endTime < currentTime && (alone || myData.m_coroStack.allEmpty()))
        break;
      workOnTasks(myData, myQueue);
      if (myData.m_coroStack.allEmpty() && !alone && safeSleepUntil > currentTime) {
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
        {
          auto before = m_time_active.m_threads[myData.m_id].time_before->load();
          m_time_active.m_threads[myData.m_id].time_active->fetch_add(currentTime.time_since_epoch().count() - before);
          m_time_active.m_threads[myData.m_id].active->store(false);
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
        {
          currentTime = std::chrono::high_resolution_clock::now();
          m_time_active.m_threads[myData.m_id].time_before->store(currentTime.time_since_epoch().count());
          m_time_active.m_threads[myData.m_id].active->store(true);
        }
#endif
      }
    }
#ifdef CSS_COLLECT_THREAD_AWAKE_INFORMATION
    {
      auto currentTime2 = std::chrono::high_resolution_clock::now();
      auto before = m_time_active.m_threads[myData.m_id].time_before->load();
      m_time_active.m_threads[myData.m_id].time_active->fetch_add(currentTime2.time_since_epoch().count() - before);
      m_time_active.m_threads[myData.m_id].active->store(false);
    }
#endif
  }
  void waitOnQueueEmpty() noexcept {
    size_t threadID = static_cast<size_t>(internal_locals::thread_id);
    if (!internal_locals::thread_from_pool)
      threadID = 0;
    const bool alone = m_threads == 1;
    if (alone)
      return;
    auto& myData = m_data[threadID];
    auto& myQueue = m_stealQueues[threadID];
    while (!myQueue.loot.empty()) {}
  }
};
void createThreadPool();
void executeFor(size_t microseconds);
void waitOwnQueueStolen();
extern std::unique_ptr<ThreadPool> s_stealPool;
}
#pragma once
#include "scheduler/version2/task_stealing_pool_v2.hpp"
#include "scheduler/coroutine/coroutine_helpers.hpp"

namespace coro_v2
{
template<typename T>
class StolenTask {
public:
  struct promise_type {
    using coro_handle = std::experimental::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      std::atomic_int* counter = taskstealer_v2::globals::s_stealPool->localAllocator().allocObj<std::atomic_int>();
      return StolenTask(coro_handle::from_promise(*this), counter);
    }
    void* operator new(size_t sz) {
      return taskstealer_v2::globals::s_stealPool->localAllocator().allocate(sz);
    }
    void operator delete(void* p, size_t) {
      taskstealer_v2::globals::s_stealPool->localAllocator().free(p);
    }
    constexpr coro::suspend_always initial_suspend() noexcept {
      return {};
    }

    constexpr coro::suspend_always final_suspend() noexcept {
      return {};
    }
    void return_value(T value) noexcept {m_value = value;}
    void unhandled_exception() noexcept {
      std::terminate();
    }
    T m_value;
  };
  using coro_handle = std::experimental::coroutine_handle<promise_type>;
  StolenTask(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle), tracker_(counter)
  {
    assert(handle_);
    counter->store(1);
    taskstealer_v2::globals::s_stealPool->spawnTask(handle_, counter);
  }
  StolenTask(StolenTask& other) noexcept {
    handle_ = other.handle_;
    tracker_ = other.tracker_;
  };
  StolenTask(StolenTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    tracker_ = other.tracker_;
    assert(handle_);
    other.handle_ = nullptr;
  }
  StolenTask& operator=(StolenTask& other) noexcept {
    handle_ = other.handle_;
    tracker_ = other.tracker_;
    return *this;
  };
  StolenTask& operator=(StolenTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    tracker_ = other.tracker_;
    assert(handle_);
    other.handle_ = nullptr;
    return *this;
  }
  T await_resume() noexcept {
    return handle_.promise().m_value;
  }
  constexpr bool await_ready() noexcept {
    return false;
  }

  // enemy coroutine needs this coroutines result, therefore we compute it.
  template <typename Type>
  void await_suspend(Type handle) noexcept {
    taskstealer_v2::globals::s_stealPool->addDependencyToCurrentTask(tracker_);
  }
  ~StolenTask() noexcept {
    if (handle_)
      handle_.destroy();
    taskstealer_v2::globals::s_stealPool->localAllocator().freeObj(tracker_);
  }

  T get() noexcept
  {
    if (!handle_.done())
      taskstealer_v2::globals::s_stealPool->execute();
    auto val = handle_.promise().m_value;
    return val; 
  }
  bool is_ready() const {
    return handle_.done();
  }
  /*
  bool is_ready() const {
    return handle_ && handle_.done();
  }
  explicit operator bool() const {
    return handle_.address() != nullptr;
  }*/
  // unwrap() future<future<int>> -> future<int>
  // future then(lambda) -> attach function to be done after current Task.
  // is_ready() are you ready?
private:
  std::experimental::coroutine_handle<promise_type> handle_;
  std::atomic_int* tracker_;
};

// void version
template <>
class StolenTask<void> {
public:
  struct promise_type {
    using coro_handle = std::experimental::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      std::atomic_int* counter = taskstealer_v2::globals::s_stealPool->localAllocator().allocObj<std::atomic_int>();
      return StolenTask(coro_handle::from_promise(*this), counter);
    }
    void* operator new(size_t sz) {
      return taskstealer_v2::globals::s_stealPool->localAllocator().allocate(sz);
    }
    void operator delete(void* p, size_t) {
      taskstealer_v2::globals::s_stealPool->localAllocator().free(p);
    }
    constexpr coro::suspend_always initial_suspend() noexcept {
      return {};
    }

    constexpr coro::suspend_always final_suspend() noexcept {
      return {};
    }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      std::terminate();
    }
  };
  using coro_handle = std::experimental::coroutine_handle<promise_type>;
  StolenTask(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle), tracker_(counter)
  {
    assert(handle_);
    counter->store(1);
    taskstealer_v2::globals::s_stealPool->spawnTask(handle_, counter);
  }
  StolenTask(StolenTask& other) noexcept {
    handle_ = other.handle_;
    tracker_ = other.tracker_;
  };
  StolenTask(StolenTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    tracker_ = other.tracker_;
    assert(handle_);
    other.handle_ = nullptr;
  }
  StolenTask& operator=(StolenTask& other) noexcept {
    handle_ = other.handle_;
    tracker_ = other.tracker_;
    return *this;
  };
  StolenTask& operator=(StolenTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    tracker_ = other.tracker_;
    assert(handle_);
    other.handle_ = nullptr;
    return *this;
  }
  void await_resume() noexcept {
  }
  constexpr bool await_ready() noexcept {
    return false;
  }

  // enemy coroutine needs this coroutines result, therefore we compute it.
  template <typename Type>
  void await_suspend(Type handle) noexcept {
    taskstealer_v2::globals::s_stealPool->addDependencyToCurrentTask(tracker_);
  }
  ~StolenTask() noexcept {
    if (handle_)
      handle_.destroy();
    taskstealer_v2::globals::s_stealPool->localAllocator().freeObj(tracker_);
  }

  void wait() noexcept
  {
    if (!handle_.done())
      taskstealer_v2::globals::s_stealPool->execute();
  }

  bool is_ready() const {
    return handle_.done();
  }

/*
  explicit operator bool() const {
    return !handle_.done();
  }*/
  // unwrap() future<future<int>> -> future<int>
  // future then(lambda) -> attach function to be done after current Task.
  // is_ready() are you ready?
private:
  std::experimental::coroutine_handle<promise_type> handle_;
  std::atomic_int* tracker_;
};

template<typename Func>
StolenTask<void> run(Func&& f)
{
  f();
  co_return;
}

}
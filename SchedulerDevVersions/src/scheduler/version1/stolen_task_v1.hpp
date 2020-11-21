#pragma once
#include "scheduler/version1/task_stealing_pool_v1.hpp"
#include "scheduler/coroutine/coroutine_helpers.hpp"

namespace coro_v1
{
template<typename T>
class StolenTask {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return StolenTask(coro_handle::from_promise(*this));
    }
    constexpr std::suspend_always initial_suspend() noexcept {
      return {};
    }

    constexpr std::suspend_always final_suspend() noexcept {
      return {};
    }
    void return_value(T value) noexcept {m_value = value;}
    void unhandled_exception() noexcept {
      std::terminate();
    }
    T m_value;
  };
  using coro_handle = std::coroutine_handle<promise_type>;
  StolenTask(coro_handle handle) noexcept : handle_(handle)
  {
    assert(handle_);
    tracker_ = taskstealer_v1::globals::s_stealPool->spawnTask(handle_);
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
    taskstealer_v1::globals::s_stealPool->addDependencyToCurrentTask(tracker_);
  }
  ~StolenTask() noexcept {
    if (handle_)
      handle_.destroy();
  }

  T get() noexcept
  {
    if (!handle_.done())
      taskstealer_v1::globals::s_stealPool->execute();
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
  std::coroutine_handle<promise_type> handle_;
  uintptr_t tracker_ = 0;
};

// void version
template <>
class StolenTask<void> {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return StolenTask(coro_handle::from_promise(*this));
    }
    constexpr std::suspend_always initial_suspend() noexcept {
      return {};
    }

    constexpr std::suspend_always final_suspend() noexcept {
      return {};
    }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      std::terminate();
    }
  };
  using coro_handle = std::coroutine_handle<promise_type>;
  StolenTask(coro_handle handle) noexcept : handle_(handle)
  {
    assert(handle_);
    tracker_ = taskstealer_v1::globals::s_stealPool->spawnTask(handle_);
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
    taskstealer_v1::globals::s_stealPool->addDependencyToCurrentTask(tracker_);
  }
  ~StolenTask() noexcept {
    if (handle_)
      handle_.destroy();
  }

  void wait() noexcept
  {
    if (!handle_.done())
      taskstealer_v1::globals::s_stealPool->execute();
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
  std::coroutine_handle<> handle_;
  uintptr_t tracker_ = 0;
};

template<typename Func>
StolenTask<void> run(Func&& f)
{
  f();
  co_return;
}

}
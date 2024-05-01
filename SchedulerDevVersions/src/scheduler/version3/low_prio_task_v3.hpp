#pragma once
#include "thread_pool_v3.hpp"

namespace coro_v3
{
template<typename T>
class LowPrioTask {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return LowPrioTask(coro_handle::from_promise(*this), &this->counter);
    }
    /*
    void* operator new(size_t sz) {
      return css::s_stealPool->localAllocate(sz);
    }
    void operator delete(void* p, size_t sz) {
      css::s_stealPool->localFree(p, sz);
    }*/
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
    std::atomic_int counter = 1;
  };
  using coro_handle = std::coroutine_handle<promise_type>;
  LowPrioTask(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle)
  {
    assert(handle_);
    taskstealer_v3::s_stealPool->spawnTask(handle_, counter, taskstealer_v3::Priority::LowPriority);
  }
  LowPrioTask(LowPrioTask& other) noexcept {
    handle_ = other.handle_;
  };
  LowPrioTask(LowPrioTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    assert(handle_);
    other.handle_ = nullptr;
  }
  LowPrioTask& operator=(LowPrioTask& other) noexcept {
    handle_ = other.handle_;
    return *this;
  };
  LowPrioTask& operator=(LowPrioTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
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
      taskstealer_v3::s_stealPool->addDependencyToCurrentTask(&handle_.promise().counter);
  }
  ~LowPrioTask() noexcept {
    if (handle_)
      handle_.destroy();
  }

  T get() noexcept
  {
    if (!handle_.done())
        taskstealer_v3::s_stealPool->execute(&handle_.promise().counter);
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
};

// void version
template <>
class LowPrioTask<void> {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return LowPrioTask(coro_handle::from_promise(*this), &this->counter);
    }
    /*
    void* operator new(size_t sz) {
      return css::s_stealPool->localAllocate(sz);
    }
    void operator delete(void* p, size_t sz) {
      css::s_stealPool->localFree(p, sz);
    }*/
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
    std::atomic_int counter = 1;
  };
  using coro_handle = std::coroutine_handle<promise_type>;
  LowPrioTask(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle)
  {
    assert(handle_);
    taskstealer_v3::s_stealPool->spawnTask(handle_, counter, taskstealer_v3::Priority::LowPriority);
  }
  LowPrioTask(LowPrioTask& other) noexcept {
    handle_ = other.handle_;
  };
  LowPrioTask(LowPrioTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    assert(handle_);
    other.handle_ = nullptr;
  }
  LowPrioTask& operator=(LowPrioTask& other) noexcept {
    handle_ = other.handle_;
    return *this;
  };
  LowPrioTask& operator=(LowPrioTask&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
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
      taskstealer_v3::s_stealPool->addDependencyToCurrentTask(&handle_.promise().counter);
  }
  ~LowPrioTask() noexcept {
    if (handle_)
      handle_.destroy();
  }

  void wait() noexcept
  {
    if (!handle_.done())
        taskstealer_v3::s_stealPool->execute(&handle_.promise().counter);
  }

  bool is_ready() const {
    return handle_.done();
  }

private:
  std::coroutine_handle<promise_type> handle_;
};

template<typename Func>
LowPrioTask<void> lowPrioAsync(Func&& f)
{
  f();
  co_return;
}
}
#pragma once
#include "thread_pool.hpp"
#include "css/coroutine/coroutine_helpers.hpp"

namespace css
{
template<typename T>
class Task {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return Task(coro_handle::from_promise(*this), &this->counter);
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
  Task(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle)
  {
    assert(handle_);
    css::s_stealPool->spawnTask(handle_, counter);
  }
  Task(Task& other) noexcept {
    handle_ = other.handle_;
  };
  Task(Task&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    assert(handle_);
    other.handle_ = nullptr;
  }
  Task& operator=(Task& other) noexcept {
    handle_ = other.handle_;
    return *this;
  };
  Task& operator=(Task&& other) noexcept {
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
    css::s_stealPool->addDependencyToCurrentTask(&handle_.promise().counter);
  }
  ~Task() noexcept {
    if (handle_)
      handle_.destroy();
  }

  T get() noexcept
  {
    if (!handle_.done())
      css::s_stealPool->execute(&handle_.promise().counter);
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
class Task<void> {
public:
  struct promise_type {
    using coro_handle = std::coroutine_handle<promise_type>;
    __declspec(noinline) auto get_return_object() noexcept {
      return Task(coro_handle::from_promise(*this), &this->counter);
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
  Task(coro_handle handle, std::atomic_int* counter) noexcept : handle_(handle)
  {
    assert(handle_);
    css::s_stealPool->spawnTask(handle_, counter);
  }
  Task(Task& other) noexcept {
    handle_ = other.handle_;
  };
  Task(Task&& other) noexcept {
    if (other.handle_)
      handle_ = std::move(other.handle_);
    assert(handle_);
    other.handle_ = nullptr;
  }
  Task& operator=(Task& other) noexcept {
    handle_ = other.handle_;
    return *this;
  };
  Task& operator=(Task&& other) noexcept {
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
    css::s_stealPool->addDependencyToCurrentTask(&handle_.promise().counter);
  }
  ~Task() noexcept {
    if (handle_)
      handle_.destroy();
  }

  void wait() noexcept
  {
    if (!handle_.done())
      css::s_stealPool->execute(&handle_.promise().counter);
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
  std::coroutine_handle<promise_type> handle_;
};

template<typename Func>
Task<void> async(Func&& f)
{
  f();
  co_return;
}

/*
template<size_t ppt, typename T, typename Func>
css::Task<void> parallel_forCSS(T start, T end, Func&& f) {
  size_t size = end - start;
  while (size > 0) {
    if (size > ppt) {
      if (css::s_stealPool->localQueueSize() == 0) {
        size_t splittedSize = size / 2;
        auto a = parallel_forCSS<ppt>(start, end - splittedSize, std::forward<decltype(f)>(f));
        auto b = parallel_forCSS<ppt>(end - splittedSize, end, std::forward<decltype(f)>(f));
        co_await a;
        co_await b;
        co_return;
      }
    }
    size_t doPPTWork = std::min(ppt, size);

    for (T i = start; i != start+doPPTWork; ++i)
      co_await f(*i);
    start += doPPTWork;
    size = end - start;
  }
  co_return;
}

template<size_t ppt, typename T, typename Func>
css::Task<void> parallel_forCSS2(T start, T end, Func&& f) {
  size_t size = end - start;
  while (size > 0) {
    if (size > ppt) {
      if (css::s_stealPool->localQueueSize() == 0) {
        size_t splittedSize = size / 2;
        auto a = parallel_forCSS2<ppt>(start, end - splittedSize, std::forward<decltype(f)>(f));
        auto b = parallel_forCSS2<ppt>(end - splittedSize, end, std::forward<decltype(f)>(f));
        co_await a;
        co_await b;
        co_return;
      }
    }
    size_t doPPTWork = std::min(ppt, size);

    for (T i = start; i != start+doPPTWork; ++i)
      f(*i);
    start += doPPTWork;
    size = end - start;
  }
  co_return;
}*/
}
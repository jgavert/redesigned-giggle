#pragma once
#include <experimental/coroutine>

namespace css
{
struct noop_task {
  struct promise_type {
    noop_task get_return_object() noexcept {
      return { std::experimental::coroutine_handle<promise_type>::from_promise(*this) };
    }
    void unhandled_exception() noexcept {}
    void return_void() noexcept {}
    std::experimental::suspend_always initial_suspend() noexcept { return {}; }
    std::experimental::suspend_never final_suspend() noexcept { return {}; }
  };
  std::experimental::coroutine_handle<> coro;
};
std::experimental::coroutine_handle<> noop_coroutine() noexcept;
}
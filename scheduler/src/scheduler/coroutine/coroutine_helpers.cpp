#include "scheduler/coroutine/coroutine_helpers.hpp"

namespace coro
{
std::experimental::coroutine_handle<> noop_coroutine() noexcept {
  return []() -> noop_task {
    co_return;
  }().coro;
}
}
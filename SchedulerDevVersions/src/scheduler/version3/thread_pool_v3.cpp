#include "thread_pool_v3.hpp"

namespace taskstealer_v3
{
namespace internal_locals
{
thread_local bool thread_from_pool = false;
thread_local int thread_id = -1;
}

std::unique_ptr<ThreadPool> s_stealPool;

void createThreadPool() {
  if (!s_stealPool) s_stealPool = std::make_unique<ThreadPool>();
}
void executeFor(size_t microseconds) {
  s_stealPool->executeFor(microseconds);
}
void waitOwnQueueStolen() {
  s_stealPool->waitOnQueueEmpty();
}
}
#include "scheduler/version0/task_stealing_pool.hpp"

namespace taskstealer 
{
namespace locals 
{
thread_local bool thread_from_pool = false;
thread_local int thread_id = -1;
}
namespace globals 
{
  std::unique_ptr<ThreadPool> s_stealPool;
  void createThreadPool() {
    if (!s_stealPool) s_stealPool = std::make_unique<ThreadPool>();
  }
}
}
#include "scheduler/version2/task_stealing_pool_v2.hpp"

namespace taskstealer_v2 
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
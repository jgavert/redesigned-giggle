#include <cstdio>
#include <scheduler/version2/stolen_task_v2.hpp>
#include <scheduler/version1/stolen_task_v1.hpp>
#include <scheduler/version0/stolen_task.hpp>
#include <css/task.hpp>
#include <css/low_prio_task.hpp>
#include <chrono>
#include <cassert>

//namespace taskstealer_c = taskstealer_v2::globals;
namespace taskstealer_c = css;
namespace coro_c = css;
//namespace coro_c = coro_vV2;
//namespace taskstealer_c = taskstealer;
//namespace coro_c = coro;

class Timer
{
public:
  Timer() : start(std::chrono::high_resolution_clock::now())
  {

  }
  int64_t reset()
  {
    auto current = std::chrono::high_resolution_clock::now();
    auto val = std::chrono::duration_cast<std::chrono::nanoseconds>(current - start).count();
    start = current;
    return val;
  }
  int64_t timeFromLastReset()
  {
    auto current = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(current - start).count();
  }
  int64_t timeMicro()
  {
    auto current = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(current - start).count();
  }
private:
  std::chrono::high_resolution_clock::time_point start;
};

int addInTreeNormal(int treeDepth) {
  if (treeDepth <= 0)
    return 1;
  int sum = 0;
  sum += addInTreeNormal(treeDepth - 1);
  sum += addInTreeNormal(treeDepth - 1);
  return sum;
}

coro_c::Task<int> addInTreeTS(int treeDepth, int parallelDepth) noexcept {
  if (treeDepth <= 0)
    co_return 1;
  if (treeDepth > parallelDepth) {
    int result = 0;
    auto res0 = addInTreeTS(treeDepth - 1, parallelDepth);
    auto res1 = addInTreeTS(treeDepth - 1, parallelDepth);
    result += co_await res0;
    result += co_await res1;
    co_return result;
  }
  else {
    auto res0 = addInTreeNormal(treeDepth - 1);
    auto res1 = addInTreeNormal(treeDepth - 1);
    co_return res0 + res1;
  }
}

coro_c::LowPrioTask<int> asyncLoopTest(int treeSize, int computeTree) noexcept {
  Timer time2;
  size_t mint = 0, maxt = 0;
  size_t avegMin = 0, avegMax = 0;
  size_t aveCount = 0;
  int a = addInTreeNormal(treeSize);
  auto refTime = time2.timeMicro();
  mint = std::numeric_limits<size_t>::max();
  maxt = 0;
  auto omi = mint;
  auto oma = 0;
  auto stats = taskstealer_c::s_stealPool->stats();
  size_t aveg = 0;
  for (int i = 0; i < 3000; i++) {
    auto another = addInTreeTS(treeSize, treeSize - computeTree);
    int lbs = co_await another;
    assert(a == lbs);
    auto t = static_cast<size_t>(time2.timeMicro());
    aveg += t;
    mint = (mint > t) ? t : mint;
    maxt = (maxt < t) ? t : maxt;
    if (i % 100 == 0) {
      avegMin = avegMin + mint;
      avegMax = avegMax + maxt;
      aveCount++;
      auto newTasksDone = taskstealer_c::s_stealPool->stats();
      auto diffDone = (newTasksDone.tasks_done - stats.tasks_done) / 100;
      auto diffStolen = newTasksDone.tasks_stolen - stats.tasks_stolen;
      auto stealsWithinL3 = (newTasksDone.tasks_stolen_within_l3 - stats.tasks_stolen_within_l3) / float(diffStolen) * 100;
      diffStolen = diffStolen / 100;
      auto diffStealTries = (newTasksDone.steal_tries - stats.steal_tries) / 100;
      auto diffUnforked = (newTasksDone.tasks_unforked - stats.tasks_unforked) / 100;
      stats = newTasksDone;
      auto times = taskstealer_c::s_stealPool->threadUsage();
      printf("%d. ref: %.3fms ratio %.2f aveg: %.3fms min: %.3fms max: %.3fms tasks done: %zu tasks stolen: %zu(from within L3 cache: %.1f%%) failed steals: %zu didn't steal: %zu cpuUse:%f\n",i, refTime/1000.f, refTime / (aveg / 100.f), aveg / 100 / 1000.f, mint / 1000.f, maxt / 1000.f, diffDone, diffStolen, stealsWithinL3, diffStealTries, diffUnforked, times.totalCpuPercentage());
      for (size_t thread = 0; thread < times.size(); ++thread) {
        printf("%f ", times.thread(thread));
      }
      printf("\n");
      aveg = 0;
      mint = omi;
      maxt = oma;
      fflush(stdout);
    }
    time2.reset();
  }
  co_return a; //co_await overlap;
}

int main(int argc, char** argv) {
  taskstealer_c::createThreadPool();
  asyncLoopTest(24, 6).get();
  auto times = taskstealer_c::s_stealPool->threadUsage();
  printf("cpu percentage %f\n", times.totalCpuPercentage());
  for (size_t thread = 0; thread < times.size(); ++thread) {
    printf("%f ", times.thread(thread));
  }
  printf("\n");
}
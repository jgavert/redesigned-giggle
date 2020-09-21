#include <catch2/catch.hpp>

#include <scheduler/version0/stolen_task.hpp>
#include <scheduler/version1/stolen_task_v1.hpp>
#include <scheduler/version2/stolen_task_v2.hpp>
#include <css/task.hpp>
#include <scheduler/coroutine/reference_coroutine_task.hpp>

#include <vector>
#include <thread>
#include <future>
#include <optional>
#include <cstdio>
#include <iostream>
#include <deque>
#include <experimental/coroutine>
#include <execution>
#include <algorithm>

namespace {

    template<typename T>
    T empty_function() {
      co_return;
    }

    template<typename T>
    T child() {
      co_return 1;
    }

    

    uint64_t Fibonacci(uint64_t number) noexcept {
      return number < 2 ? 1 : Fibonacci(number - 1) + Fibonacci(number - 2);
    }
    
    uint64_t fibonacciLoop(uint64_t nthNumber) noexcept {
      uint64_t previouspreviousNumber, previousNumber = 0, currentNumber = 1;
      for (uint64_t i = 1; i < nthNumber+1 ; i++) {
        previouspreviousNumber = previousNumber;
        previousNumber = currentNumber;
        currentNumber = previouspreviousNumber + previousNumber;
      }
      return currentNumber;
    }
    reference::Task<uint64_t> FibonacciReferenceIterative(uint64_t number) noexcept {
      co_return fibonacciLoop(number);
    }

    reference::Task<uint64_t> FibonacciReferenceRecursive(uint64_t number) noexcept {
      co_return Fibonacci(number);
    }
    template<typename T>
    T FibonacciCoro(uint64_t number) noexcept {
      if (number < 2)
        co_return 1;
        
      auto v0 = FibonacciCoro<T>(number-1);
      auto v1 = FibonacciCoro<T>(number-2);
      co_return co_await v0 + co_await v1;
    }
    template<typename T>
    T SpawnEmptyTasksInTree(uint64_t tasks) noexcept {
      if (tasks <= 1)
        co_return;
      tasks = tasks - 1;
      if (tasks == 1){
        co_await empty_function<T>();
        co_return;
      }
      
      uint64_t split = tasks / 2;
      uint64_t splitOff = tasks % 2;
      auto v0 = SpawnEmptyTasksInTree<T>(split);
      auto v1 = SpawnEmptyTasksInTree<T>(split + splitOff);
      co_await v0;
      co_await v1;
      co_return;
    }

    template<typename T>
    T TonsOfEmptyTasks(uint64_t taskCount) noexcept {
      int sum = 0;
      std::vector<T> tasks;

      for (uint64_t i = 0; i < taskCount; ++i) {
        tasks.emplace_back(child<T>());
      }

      for (auto&& it : tasks) {
        if (!it.is_ready())
          sum += co_await it;
        else
          sum += it.get();
      }

      co_return sum;
    }

    coro_v2::StolenTask<int> returnOne() {
      co_return 1;
    }

    template<size_t ppt, typename T, typename Func>
    coro_v2::StolenTask<void> parallel_for(T start, T end, Func&& f) {
      size_t size = end - start;
      while (size > 0) {
        if (size > ppt) {
          if (taskstealer_v2::globals::s_stealPool->localQueueSize() == 0) {
            size_t splittedSize = size / 2;
            auto a = parallel_for<ppt>(start, end - splittedSize, std::forward<decltype(f)>(f));
            auto b = parallel_for<ppt>(end - splittedSize, end, std::forward<decltype(f)>(f));
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
    }
}

#define BenchFunction(calledFunction, argument) \
    BENCHMARK(#calledFunction ## "(" ## #argument ## ")") { \
        return calledFunction(argument).get(); \
    }
#define BenchFunctionWait(calledFunction, argument) \
    BENCHMARK(#calledFunction ## "(" ## #argument ## ")") { \
        return calledFunction(argument).wait(); \
    }


#define checkAllEmptyTasksSpawning(argument) \
    BenchFunctionWait(SpawnEmptyTasksInTree<reference::Task<void>>, argument); \
    BenchFunctionWait(SpawnEmptyTasksInTree<coro::StolenTask<void>>, argument); \
    BenchFunctionWait(SpawnEmptyTasksInTree<coro_v1::StolenTask<void>>, argument); \
    BenchFunctionWait(SpawnEmptyTasksInTree<coro_v2::StolenTask<void>>, argument); \
    BenchFunctionWait(SpawnEmptyTasksInTree<css::Task<void>>, argument)

#define checkAllTonsOfEmptyTasks(argument) \
    BenchFunction(TonsOfEmptyTasks<coro_v2::StolenTask<int>>, argument); \
    BenchFunction(TonsOfEmptyTasks<css::Task<int>>, argument)


#define checkAllFibonacci(argument) \
    BenchFunction(FibonacciReferenceIterative, argument); \
    BenchFunction(FibonacciReferenceRecursive, argument); \
    BenchFunction(FibonacciCoro<reference::Task<uint64_t>>, argument); \
    BenchFunction(FibonacciCoro<coro::StolenTask<uint64_t>>, argument); \
    BenchFunction(FibonacciCoro<coro_v1::StolenTask<uint64_t>>, argument); \
    BenchFunction(FibonacciCoro<css::Task<uint64_t>>, argument)
    //BenchFunction(FibonacciCoro<coro_v2::StolenTask<uint64_t>>, argument);
TEST_CASE("Benchmark Fibonacci", "[benchmark]") {
    taskstealer::globals::createThreadPool();
    taskstealer_v1::globals::createThreadPool();
    taskstealer_v2::globals::createThreadPool();
    css::createThreadPool();
    reference::globals::createExecutor();
    
    CHECK(FibonacciReferenceIterative(0).get() == 1);
    CHECK(FibonacciReferenceIterative(5).get() == 8);
    CHECK(FibonacciReferenceRecursive(0).get() == 1);
    CHECK(FibonacciReferenceRecursive(5).get() == 8);
    CHECK(FibonacciCoro<reference::Task<uint64_t>>(0).get() == 1);
    CHECK(FibonacciCoro<reference::Task<uint64_t>>(5).get() == 8);
    CHECK(FibonacciCoro<coro::StolenTask<uint64_t>>(0).get() == 1);
    CHECK(FibonacciCoro<coro::StolenTask<uint64_t>>(5).get() == 8);
    CHECK(FibonacciCoro<coro_v1::StolenTask<uint64_t>>(0).get() == 1);
    CHECK(FibonacciCoro<coro_v1::StolenTask<uint64_t>>(5).get() == 8);
    CHECK(FibonacciCoro<coro_v2::StolenTask<uint64_t>>(0).get() == 1);
    CHECK(FibonacciCoro<coro_v2::StolenTask<uint64_t>>(5).get() == 8);
    CHECK(TonsOfEmptyTasks<coro_v2::StolenTask<int>>(5).get() == 5);
    CHECK(FibonacciCoro<css::Task<uint64_t>>(0).get() == 1);
    CHECK(FibonacciCoro<css::Task<uint64_t>>(5).get() == 8);
    std::vector<int> lol( 1000000, 1);
    std::vector<int> ref(1000000, 1);
    parallel_for<32>(lol.begin(), lol.end(), [](int& woot) {
      woot = woot * 2;
      }).wait();
    for (int i = 0; i < ref.size(); ++i) {
      auto refV = ref[i] * 2;
      CHECK(refV == lol[i]);
    }

    BENCHMARK("for_each") {
      size_t asd = 0;
//#pragma loop(no_vector)
      for (auto&& it : lol) {
        it = it * 2;
        asd += it;
      }
      return asd;
    };
    BENCHMARK("std::for_each(std::execution::par") {
      return std::for_each(std::execution::par, lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      });
    };
    BENCHMARK("std::for_each(std::execution::par_unseq") {
      return std::for_each(std::execution::par_unseq, lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      });
    };
    BENCHMARK("parallel_for<32>") {
      return parallel_for<32>(lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      }).wait();
    };
    BENCHMARK("parallel_for<256>") {
      return parallel_for<256>(lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      }).wait();
    };
    BENCHMARK("parallel_for<32768>") {
      return parallel_for<32768>(lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      }).wait();
    };
    BENCHMARK("std::for_each(std::execution::par_unseq special") {
      std::for_each(std::execution::par_unseq, lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      });
      return std::for_each(std::execution::par_unseq, ref.begin(), ref.end(), [](int& woot) {
        woot = woot * 2;
      });
    };
    BENCHMARK("parallel_for<32768> special") {
      auto a = parallel_for<32768>(lol.begin(), lol.end(), [](int& woot) {
        woot = woot * 2;
      });
      auto b = parallel_for<32768>(ref.begin(), ref.end(), [](int& woot) {
        woot = woot * 2;
      });
      b.wait();
      return a.wait();
    };
    checkAllFibonacci(20);
    checkAllEmptyTasksSpawning(100);
    checkAllEmptyTasksSpawning(1000);
    checkAllEmptyTasksSpawning(65000);
    checkAllEmptyTasksSpawning(100000);
    checkAllEmptyTasksSpawning(200000);
    //checkAllTonsOfEmptyTasks(1000);
    //checkAllTonsOfEmptyTasks(10000);
    //checkAllTonsOfEmptyTasks(100000);
    //checkAllTonsOfEmptyTasks(200000);
    //checkAllTonsOfEmptyTasks(1000000);
    /*
    BENCHMARK("Coroutine Fibonacci 25") {
        return FibonacciCoro(25, 25-parallel+1).get();
    };
    BENCHMARK("Fibonacci 30") {
        return FibonacciOrig(30).get();
    };
    BENCHMARK("Coroutine Fibonacci 30") {
        return FibonacciCoro(30,30-parallel).get();
    };
    
    BENCHMARK("Fibonacci 32") {
        return FibonacciOrig(32).get();
    };
    BENCHMARK("Coroutine Fibonacci 32") {
        return FibonacciCoro(32,32-parallel).get();
    };
    
    BENCHMARK("Fibonacci 36") {
        return FibonacciOrig(36).get();
    };
    BENCHMARK("Coroutine Fibonacci 36") {
        return FibonacciCoro(36,36-parallel-1).get();
    };
    /*
    BENCHMARK("Fibonacci 38") {
        return FibonacciOrig(38).get();
    };
    BENCHMARK("Coroutine Fibonacci 38") {
        return FibonacciCoro(38,38-parallel-3).get();
    };*/
}
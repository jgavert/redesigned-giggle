#include <iostream>

#include <v0/hello.hpp>
#include <v0/coroutine/stolen_task.hpp>

int main()
{
  taskstealer::globals::createThreadPool();
  std::cout << "Entering main !" << std::endl;
  coro::run([]() {
    libstuff::say_hello();
  }).wait();
  return 0;
}


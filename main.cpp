#include <thread> // needed for sleep
#include <print>

#include "cosched.hpp"

using namespace cosched;

Task grandchild()
{
  std::println("grandchild started; sleep");
  co_await AwaitSleep{300};
  std::println("grandchild woke up; return");
  co_return;
}



Task child1()
{
  std::println("child1 started; sleep");
  co_await AwaitSleep{100};
  std::println("child1 woke up; return");
  co_return;
}


Task child2()
{
  std::println("child2 started; sleep");
  co_await AwaitSleep{10};
  std::println("child2 woke up; spawn grandchild");
  co_await AwaitSpawn{grandchild, NULL};
  co_return;
}


Task comain()
{
  std::println("main start");

  TaskId child1id, child2id;
  std::println("main started; spawn child1");
  co_await AwaitSpawn{child1, &child1id};
  std::println("main child1 spawned; spawn child2");
  co_await AwaitSpawn{child2, &child2id};
  std::println("main child2 spawned; sleep");
  co_await AwaitSleep{20};
  std::println("main woke up; spawn child2");
  co_await AwaitSpawn{child2, &child2id};
  std::println("main spawned child2; join child1");
  co_await AwaitJoin{child1id};
  std::println("main joined child1; join child2");
  co_await AwaitJoin{child2id};
  std::println("main joined child2; return");
}

int main()
{
  {
  Scheduler sched(comain);
  while (!sched.done()) {
    sched.Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  }

}

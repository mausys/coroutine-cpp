#include <thread> // needed for sleep

#include "cosched.hpp"



CoTask grandchild()
{
  std::println("grandchild started; sleep");
  co_await CoAwaitSleep{300};
  std::println("grandchild woke up; return");
  co_return;
}



CoTask child1()
{
  std::println("child1 started; sleep");
  co_await CoAwaitSleep{100};
  std::println("child1 woke up; return");
  co_return;
}


CoTask child2()
{
  std::println("child2 started; sleep");
  co_await CoAwaitSleep{10};
  std::println("child2 woke up; spawn grandchild");
  co_await CoAwaitSpawn{grandchild, NULL};
  co_return;
}


CoTask comain()
{
  std::println("main start");

  CoTaskId child1id, child2id;
  std::println("main started; spawn child1");
  co_await CoAwaitSpawn{child1, &child1id};
  std::println("main child1 spawned; spawn child2");
  co_await CoAwaitSpawn{child2, &child2id};
  std::println("main child2 spawned; sleep");
  co_await CoAwaitSleep{20};
  std::println("main woke up; spawn child2");
  co_await CoAwaitSpawn{child2, &child2id};
  std::println("main spawned child2; join child1");
  co_await CoAwaitJoin{child1id};
  std::println("main joined child1; join child2");
  co_await CoAwaitJoin{child2id};
  std::println("main joined child2; return");
}

int main()
{
  {
  CoScheduler sched(comain);
  while (!sched.done()) {
    sched.Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  }

}

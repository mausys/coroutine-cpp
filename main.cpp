#include <thread> // needed for sleep
#include <print>
#include <exception>

#include "cosched.hpp"

using namespace cosched;

class Context {
  public:
    int  cnt = 0;
};

Task<Context> grandchild900(Context& )
{
  std::println("grandchild900 started; sleep");
  co_await AwaitSleep{MilliSeconds(900)};
  std::println("grandchild900 woke up; return");
  co_return;
}

Task<Context> grandchild800(Context&)
{
  std::println("grandchild800 started; sleep");
  co_await AwaitSleep{MilliSeconds(800)};
  std::println("grandchild800 woke up; return");
  co_return;
}

Task<Context> grandchild700(Context&)
{
  std::println("grandchild700 started; sleep");
  co_await AwaitSleep{MilliSeconds(700)};
  std::println("grandchild700 woke up; return");
  throw std::logic_error("test");
  co_return;
}
Task<Context> grandchild600(Context&)
{
  std::println("grandchild600 started; sleep");
  co_await AwaitSleep{MilliSeconds(600)};
  std::println("grandchild600 woke up; return");
  co_return;
}
Task<Context> grandchild500(Context&)
{
  std::println("grandchild500 started; sleep");
  co_await AwaitSleep{MilliSeconds(500)};
  std::println("grandchild500 woke up; return");
  co_return;
}
Task<Context> grandchild400(Context&)
{
  std::println("grandchild400 started; sleep");
  co_await AwaitSleep{MilliSeconds(400)};
  std::println("grandchild400 woke up; return");
  co_return;
}
Task<Context> grandchild300(Context&)
{
  std::println("grandchild300 started; sleep");
  co_await AwaitSleep{MilliSeconds(300)};
  std::println("grandchild300 woke up; return");
  co_return;
}
Task<Context> grandchild200(Context&)
{
  std::println("grandchild200 started; sleep");
  co_await AwaitSleep{MilliSeconds(200)};
  std::println("grandchild200 woke up; return");
  co_return;
}

Task<Context> grandchild100(Context&)
{
  std::println("grandchild100 started; sleep");
  co_await AwaitSleep{MilliSeconds(100)};
  std::println("grandchild100 woke up; return");
  co_return;
}


Task<Context> grandchild0(Context&)
{
  std::println("grandchild0 started; sleep");
  co_await AwaitSleep{MilliSeconds(0)};
  std::println("grandchild0 woke up; return");
  co_return;
}

Task<Context> grandchild(Context&)
{
  std::println("grandchild started; sleep");
  co_await AwaitSleep{MilliSeconds(300)};
  std::println("grandchild woke up; return");
  co_await AwaitSpawn{grandchild900, NULL};
  co_await AwaitSpawn{grandchild0, NULL};
  co_await AwaitSpawn{grandchild800, NULL};
  co_await AwaitSpawn{grandchild100, NULL};
  co_await AwaitSpawn{grandchild700, NULL};
  co_await AwaitSpawn{grandchild200, NULL};
  co_await AwaitSpawn{grandchild600, NULL};
  co_await AwaitSpawn{grandchild300, NULL};
  co_await AwaitSpawn{grandchild500, NULL};

  co_return;
}



Task<Context> child1(Context&)
{
  std::println("child1 started; sleep");
  co_await AwaitSleep{MilliSeconds(100)};
  std::println("child1 woke up; return");
  co_return;
}


Task<Context> child2(Context&)
{
  std::println("child2 started; sleep");
  co_await AwaitSleep{MilliSeconds(10)};
  std::println("child2 woke up; spawn grandchild");
  co_await AwaitSpawn{grandchild, NULL};
  co_return;
}


Task<Context> comain(Context&)
{
  std::println("main start");

  TaskId child1id, child2id;
  std::println("main started; spawn child1");
  co_await AwaitSpawn{child1, &child1id};
  std::println("main child1 spawned; spawn child2");
  co_await AwaitSpawn{child2, &child2id};
  std::println("main child2 spawned; sleep");
  co_await AwaitSleep{MilliSeconds(20)};
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
    Context ctx;
  Scheduler<Context> sched(comain, ctx);
  for  (int i = 0; !sched.done(); i++) {
    //std::println("poll {}", i);
    try {
      sched.Poll();
    } catch (std::logic_error e) {
      std::println("exception cauth {}", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  }

}

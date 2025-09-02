#include <cassert>
#include <climits>
#include <queue>
#include <chrono>
#include <coroutine>


namespace cosched {


struct Task;
class Scheduler;
using Clock = std::chrono::steady_clock;
using  Timeout = std::chrono::time_point<Clock>;
using UId = unsigned;
using Index = std::vector<Task>::size_type;

const UId kUIdInvalid = UINT_MAX;
const Index kIndexInvalid = ~(Index)0;

struct TaskId {
  // index of scheduler task vector
  Index index;
  // since indices are recycled uid as additional check is needed,
  UId uid;

  bool operator==(const TaskId& rhs) {
    return (index == rhs.index) && (uid == rhs.uid);
  }
};

const TaskId TaskIdInvalid = {
    .index = kIndexInvalid,
    .uid = kUIdInvalid,
};


// for wait queue
struct TimeoutTask{
  Timeout timeout;
  TaskId tid;
};


// for wait queue
struct CompareTimeoutTask {
  bool operator()(const TimeoutTask& t1, const TimeoutTask& t2) {
    return t1.timeout > t2.timeout;
  }
};


Timeout GetTimeout(unsigned ms) {
  auto now = Clock::now();
  return now + std::chrono::milliseconds(ms);
}

// entry point of coroutine
typedef Task (*start_fn)();

enum class AwaitType {
  None,
  Sleep,
  Spawn,
  Join,
};

struct AwaitData {
  AwaitType type;
  union {
    unsigned sleep_ms;
    struct {
      start_fn start;
      TaskId *tid;
    } spawn;
    TaskId join_tid;
  } data;
};


struct AwaitBase {
  bool await_ready() noexcept { return false; }
  void await_suspend (std::coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

struct AwaitSleep : AwaitBase
{
  explicit AwaitSleep(unsigned ms) : ms {ms} {}
  unsigned ms;
};


struct AwaitSpawn : AwaitBase {
  AwaitSpawn(start_fn start, TaskId *tid) : start {start}, tid {tid}  {}
  start_fn start;
  TaskId *tid;
};


struct AwaitJoin : AwaitBase {
  explicit AwaitJoin(TaskId tid) : tid {tid} {}
  TaskId tid;
};



struct Task
{
  struct promise_type
  {
    using coro_handle = std::coroutine_handle<promise_type>;

    AwaitData data = { .type = AwaitType::None, .data = {} };
    // TODO: handle rethrow exception in scheduler poll
    std::exception_ptr exception_ = nullptr;

    auto get_return_object() { return coro_handle::from_promise(*this); }

    auto initial_suspend() noexcept {  return std::suspend_never(); }

    // suspend_always is needed so scheduler can handle it
    auto final_suspend() noexcept {
      data.type = AwaitType::None;
      return std::suspend_always();
    }

    // copy await data for scheduler
    auto await_transform(struct AwaitSleep await) noexcept {
      assert(data.type == AwaitType::None);

      data.type = AwaitType::Sleep;
      data.data.sleep_ms = await.ms;

      return await;
    };

    auto await_transform(AwaitSpawn await) noexcept {
      assert(data.type == AwaitType::None);

      data.type = AwaitType::Spawn;
      data.data.spawn.start = await.start;
      data.data.spawn.tid = await.tid;

      return await;
    };


    auto await_transform(AwaitJoin await) noexcept {
      assert(data.type == AwaitType::None);

      data.type = AwaitType::Join;
      data.data.join_tid = await.tid;

      return await;
    };

    void return_void() {}

    void unhandled_exception() {
      exception_ = std::current_exception();
    }
  };

  Task(promise_type::coro_handle handle) : handle_(handle) {}


  Task(Task const&) = delete;
  Task& operator=(Task &task) = delete;

  Task(Task&& task) {
     // new task added to task vector
     assert(handle_ == nullptr);

    task.moved_ = true;
    this->handle_ = std::move(task.handle_);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
  };

  Task& operator=(Task&& task)  {
    // recycling vector index, check old task
    assert(handle_.done());

    task.moved_ = true;
    this->handle_ = std::move(task.handle_);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
    return *this;
  };


  ~Task()
  {
    assert(moved_ || handle_.done());
  }

  bool done() {
    return handle_.done();
  }

  AwaitData take_data() {
    AwaitData data = handle_.promise().data;
    handle_.promise().data.type = AwaitType::None;
    return data;
  }

  bool resume()
  {
    ready_ = false;
    if (!handle_.done())
      handle_();
    return !handle_.done();
  }

  bool setReady() {
    bool tmp = ready_;
    ready_ = true;
    return tmp;
  }

  UId uid_ = kUIdInvalid;
  TaskId parent_ = TaskIdInvalid;


private:
  bool ready_ = {};
  bool moved_ = false;
  promise_type::coro_handle handle_ = nullptr;


};

class Scheduler {
public:
  Scheduler(start_fn start) : spawn_{start} {}

  void Poll() {
    // moves tasks from wait_ to ready_ queue, if timeout elapsed
    CheckWaitingTasks();

    if (spawn_.start) {
      start_fn start = spawn_.start;
      spawn_.start = nullptr;

      Task task = start();

      if (task.done()) {
        // task finished; no tid tell parent
        if (spawn_.tid)
          *spawn_.tid = TaskIdInvalid;
      }

      TaskId tid = AddTask(std::move(task));

      if (spawn_.tid) {
        // tell parent tid of child
        *spawn_.tid = tid;
        spawn_.tid = nullptr;
      }

      ProcessResult(tid);
    } else if (!ready_.empty()) {
      // process ready queue

      TaskId tid = ready_.front();
      ready_.pop();
      Task *task = GetTask(tid);

      assert(task && !task->done());

      task->resume();

      ProcessResult(tid);
    }
  }
  bool done() {
    return wait_.empty() && ready_.empty() && !spawn_.start;
  }

private:

  Task* GetTask(TaskId tid) {
    if (tid == TaskIdInvalid)
      return nullptr;

    Task &task = tasks_[tid.index];

    return tid.uid == task.uid_ ? &task : nullptr;
  }

  void SetReady(TaskId tid) {
    Task *task = GetTask(tid);
    if (!task)
      return;

    if (task->done())
      return;

    if (!task->setReady()) {
        // only push task to ready queue once
        ready_.push(tid);
    }
  }


  TaskId AddTask(Task &&task)
  {
    task.uid_ = next_uid_++;

    unsigned index;
    if (!free_indices_.empty()) {
      // recycle index
      index = free_indices_.back();
      free_indices_.pop();

      tasks_[index] = std::move(task);
    } else {
      tasks_.push_back(std::move(task));
      index = tasks_.size() - 1;
    }
    return TaskId{index, task.uid_};
  }


  void CheckWaitingTasks()
  {
    auto now = GetTimeout(0);

    for (;;) {
      if (wait_.empty())
        break;

      const TimeoutTask &task = wait_.top();

      if (now < task.timeout)
        break;

      SetReady(task.tid);

      wait_.pop();
    }
  }

  void ProcessResult(TaskId tid) {
    Task *task = GetTask(tid);

    assert(task);

    if (task->done()) {
      SetReady(task->parent_);
      tasks_[tid.index].uid_= kUIdInvalid;
      // recycle index
      free_indices_.push(tid.index);
      return;
    }

    AwaitData await = task->take_data();

    switch (await.type) {
      case AwaitType::None:
        SetReady(tid);
        break;
      case AwaitType::Sleep:
        wait_.push(TimeoutTask{GetTimeout(await.data.sleep_ms), tid});
        break;
      case AwaitType::Spawn:
        assert(spawn_.start == nullptr);
        spawn_.start = await.data.spawn.start;
        spawn_.tid = await.data.spawn.tid;
        SetReady(tid);
        break;
      case AwaitType::Join:
        Task *child = GetTask(await.data.join_tid);

        if (!child || child->done()) {
          // child already done
          SetReady(tid);
          break;
        }

        assert(child->parent_ == TaskIdInvalid);
        child->parent_ = tid;

        break;
    }
  }


  // next_uid incremented everytime it is used
  unsigned next_uid_ = 0;

  // task requested spawn; spawn in next poll
  struct {
    start_fn start;
    TaskId *tid = nullptr;
  } spawn_;

  // all tasks
  std::vector<Task> tasks_;

  // can be executed
  std::queue<TaskId> ready_;

  // sleeping tasks
  std::priority_queue<TimeoutTask, std::vector<TimeoutTask>, CompareTimeoutTask> wait_;

  // for recycling tasks_indices
  std::queue<unsigned> free_indices_;
};


}

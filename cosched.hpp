#include <cassert>
#include <climits>
#include <variant>
#include <vector>
#include <queue>
#include <chrono>
#include <coroutine>


namespace cosched {


struct Task;
class Scheduler;
using Clock = std::chrono::steady_clock;
using  TimePoint = std::chrono::time_point<Clock>;
using Duration = Clock::duration;
using MilliSeconds = std::chrono::milliseconds;
using UId = unsigned;
using Index = std::vector<Task>::size_type;

const UId kUIdInvalid = UINT_MAX;
const Index kIndexInvalid = ~(Index)0;

struct TaskId {
  // index of scheduler task vector
  Index index;
  // since indices are recycled uid as additional check is needed,
  UId uid;

  bool operator==(const TaskId& rhs) const = default;
};

const TaskId TaskIdInvalid = {
    .index = kIndexInvalid,
    .uid = kUIdInvalid,
};


// for wait queue
struct TimeoutTask{
  TimePoint timepoint;
  TaskId tid;
  auto operator<=>(const TimeoutTask& rhs) const {
    return timepoint <=> rhs.timepoint;
  }
};


TimePoint GetTimeout(const Duration &duration) {
  auto now = Clock::now();
  return now + duration;
}

// entry point of coroutine
typedef Task (*start_fn)();


struct AwaitDataSleep {
  Duration duration;
};

struct AwaitDataSpawn {
  start_fn start;
  TaskId *tid;
};

struct AwaitDataJoin {
  TaskId tid;
};


using AwaitData = std::variant<std::monostate, AwaitDataSleep, AwaitDataSpawn, AwaitDataJoin>;




struct AwaitBase {
  bool await_ready() noexcept { return false; }
  void await_suspend (std::coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

struct AwaitSleep : AwaitBase
{
  explicit AwaitSleep(Duration duration) : data{duration} {}
  AwaitDataSleep data;
};


struct AwaitSpawn : AwaitBase {
  AwaitSpawn(start_fn start, TaskId *tid) : data{start, tid} {}
  AwaitDataSpawn data;
};


struct AwaitJoin : AwaitBase {
  explicit AwaitJoin(TaskId tid) : data {tid} {}
  AwaitDataJoin data;
};



struct Task
{
  struct promise_type
  {
    using coro_handle = std::coroutine_handle<promise_type>;

    AwaitData data;
    // TODO: handle rethrow exception in scheduler poll
    std::exception_ptr exception_ = nullptr;

    auto get_return_object() { return coro_handle::from_promise(*this); }

    auto initial_suspend() noexcept {  return std::suspend_never(); }

    // suspend_always is needed so scheduler can handle it
    auto final_suspend() noexcept {
      data = std::monostate{};
      return std::suspend_always();
    }

    // copy await data for scheduler
    auto await_transform(struct AwaitSleep await) noexcept {
      assert(std::get_if<std::monostate>(&data));
      data = await.data;
      return await;
    };

    auto await_transform(AwaitSpawn await) noexcept {
      assert(std::get_if<std::monostate>(&data));

      data = await.data;

      return await;
    };


    auto await_transform(AwaitJoin await) noexcept {
      assert(std::get_if<std::monostate>(&data));

      data = await.data;

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
    return std::exchange(handle_.promise().data, std::monostate{});
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
      index = free_indices_.front();
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
    auto now =  Clock::now();

    for (;;) {
      if (wait_.empty())
        break;

      const TimeoutTask &task = wait_.top();

      if (now < task.timepoint)
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

    if (std::get_if<std::monostate>(&await)) {
      SetReady(tid);
    } else if (const auto* data = std::get_if<AwaitDataSleep>(&await)) {
      wait_.push(TimeoutTask{GetTimeout(data->duration), tid});
    } else if (const auto* data = std::get_if<AwaitDataSpawn>(&await)) {
      assert(spawn_.start == nullptr);
      spawn_.start = data->start;
      spawn_.tid = data->tid;
      SetReady(tid);
    } else if (const auto* data = std::get_if<AwaitDataJoin>(&await)) {
      Task *child = GetTask(data->tid);

      if (!child || child->done()) {
        // child already done
        SetReady(tid);
      } else {
        assert(child->parent_ == TaskIdInvalid);
        child->parent_ = tid;
      }
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
  std::priority_queue<TimeoutTask, std::vector<TimeoutTask>, std::greater<TimeoutTask>> wait_;

  // for recycling tasks_indices
  std::queue<unsigned> free_indices_;
};


}

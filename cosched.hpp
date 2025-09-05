#pragma once


#include <cassert>
#include <climits>
#include <variant>
#include <vector>
#include <queue>
#include <chrono>
#include <coroutine>

namespace cosched {

template<typename T>
struct Task;
using Clock = std::chrono::steady_clock;
using  TimePoint = std::chrono::time_point<Clock>;
using Duration = Clock::duration;
using MilliSeconds = std::chrono::milliseconds;
using UId = unsigned;
using Index = size_t;

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



struct AwaitDataSleep {
  Duration duration;
};


template <typename T>
struct AwaitDataSpawn {
  using start_fn = Task<T> (*)(T&);
  start_fn start;
  TaskId *tid;

};

struct AwaitDataJoin {
  TaskId tid;
};




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

template <typename T>
struct AwaitSpawn : AwaitBase {
  using start_fn = Task<T> (*)(T&);
  AwaitSpawn(start_fn start, TaskId *tid) : data{start, tid} {}
  AwaitDataSpawn<T> data;
};


struct AwaitJoin : AwaitBase {
  explicit AwaitJoin(TaskId tid) : data {tid} {}
  AwaitDataJoin data;
};

template<typename T>
using AwaitData = std::variant<std::monostate, AwaitDataSleep, AwaitDataSpawn<T>, AwaitDataJoin>;


template<typename T>
struct Task
{
  struct promise_type
  {
    using coro_handle = std::coroutine_handle<promise_type>;

    AwaitData<T> data;

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

    auto await_transform(AwaitSpawn<T> await) noexcept {
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
    assert(this->handle_ == nullptr);

    this->handle_ = std::exchange(task.handle_, nullptr);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
  };

  Task& operator=(Task&& task)  {
    // recycling vector index, check old task
    assert(this->handle_ == nullptr);

    this->handle_ = std::exchange(task.handle_, nullptr);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
    return *this;
  };


  ~Task()
  {
    if (handle_ == nullptr)
      return;

    if (handle_.done())
      handle_.destroy();
  }


  bool done() {
    if (handle_ == nullptr)
      return true;

    if (handle_.done()) {
      handle_.destroy();
      handle_ = nullptr;
      return true;
    }
    return false;
  }

  AwaitData<T> take_data() {
    return std::exchange(handle_.promise().data, std::monostate{});
  }

  bool resume()
  {
    ready_ = false;
    if (!handle_.done()) {
      handle_();
      std::exception_ptr exception = std::exchange(handle_.promise().exception_, nullptr);
      if (exception)
        std::rethrow_exception(exception);
    }

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
  promise_type::coro_handle handle_ = nullptr;


};

template <typename T>
class Scheduler {
  using start_fn = Task<T> (*)(T&);


public:
  Scheduler(start_fn start, T arg) : arg_{arg}, spawn_{start} {}

  void Poll() {
    // moves tasks from wait_ to ready_ queue, if timeout elapsed
    CheckWaitingTasks();

    if (spawn_.start) {
      start_fn start = spawn_.start;
      spawn_.start = nullptr;

      Task task = start(arg_);

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
      Task<T> *task = GetTask(tid);

      assert(task && !task->done());

      task->resume();

      ProcessResult(tid);
    }
  }
  bool done() {
    return wait_.empty() && ready_.empty() && !spawn_.start;
  }

private:

  Task<T>* GetTask(TaskId tid) {
    if (tid == TaskIdInvalid)
      return nullptr;

    Task<T> &task = tasks_[tid.index];

    return tid.uid == task.uid_ ? &task : nullptr;
  }

  void SetReady(TaskId tid) {
    Task<T> *task = GetTask(tid);
    if (!task)
      return;

    if (task->done())
      return;

    if (!task->setReady()) {
        // only push task to ready queue once
        ready_.push(tid);
    }
  }


  TaskId AddTask(Task<T> &&task)
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
    Task<T> *task = GetTask(tid);

    assert(task);

    if (task->done()) {
      SetReady(task->parent_);
      tasks_[tid.index].uid_= kUIdInvalid;
      // recycle index
      free_indices_.push(tid.index);
      return;
    }

    AwaitData<T> await = task->take_data();

    if (std::get_if<std::monostate>(&await)) {
      SetReady(tid);
    } else if (const auto* data = std::get_if<AwaitDataSleep>(&await)) {
      wait_.push(TimeoutTask{GetTimeout(data->duration), tid});
    } else if (const auto* data = std::get_if<AwaitDataSpawn<T>>(&await)) {
      assert(spawn_.start == nullptr);
      spawn_.start = data->start;
      spawn_.tid = data->tid;
      SetReady(tid);
    } else if (const auto* data = std::get_if<AwaitDataJoin>(&await)) {
      Task<T> *child = GetTask(data->tid);

      if (!child || child->done()) {
        // child already done
        SetReady(tid);
      } else {
        assert(child->parent_ == TaskIdInvalid);
        child->parent_ = tid;
      }
    }
  }
  T& arg_;

  // next_uid incremented everytime it is used
  unsigned next_uid_ = 0;

  // task requested spawn; spawn in next poll
  struct {
    start_fn start;
    TaskId *tid = nullptr;
  } spawn_;

  // all tasks
  std::vector<Task<T>> tasks_;

  // can be executed
  std::queue<TaskId> ready_;

  // sleeping tasks
  std::priority_queue<TimeoutTask, std::vector<TimeoutTask>, std::greater<TimeoutTask>> wait_;

  // for recycling tasks_indices
  std::queue<unsigned> free_indices_;
};


}

#include <cassert>
#include <climits>
#include <queue>
#include <print>
#include <chrono>
#include <coroutine>

struct CoTask;
class CoScheduler;
using CoClock = std::chrono::steady_clock;
using  CoTimeout = std::chrono::time_point<CoClock>;
using CoUId = unsigned;
using CoIndex = std::vector<CoTask>::size_type;

const CoUId kCoUIdInvalid = UINT_MAX;
const CoIndex kCoIndexInvalid = ~(CoIndex)0;

struct CoTaskId {
  // index of scheduler task vector
  CoIndex index;
  // since indices are recycled uid as additional check is needed,
  CoUId uid;

  bool operator==(const CoTaskId& rhs) {
    return (index == rhs.index) && (uid == rhs.uid);
  }
};

const CoTaskId CoTaskIdInvalid = {
    .index = kCoIndexInvalid,
    .uid = kCoUIdInvalid,
};


// for wait queue
struct CoTimeoutTask{
  CoTimeout timeout;
  CoTaskId tid;
};


// for wait queue
struct CoCompareTimeoutTask {
  bool operator()(const CoTimeoutTask& t1, const CoTimeoutTask& t2) {
    return t1.timeout > t2.timeout;
  }
};


CoTimeout CoGetTimeout(unsigned ms) {
  auto now = CoClock::now();
  return now + std::chrono::milliseconds(ms);
}

// entry point of coroutine
typedef CoTask (*co_start_fn)();

enum class CoAwaitType {
  None,
  Sleep,
  Spawn,
  Join,
};

struct CoAwaitData {
  CoAwaitType type;
  union {
    unsigned sleep_ms;
    struct {
      co_start_fn start;
      CoTaskId *tid;
    } spawn;
    CoTaskId join_tid;
  } data;
};


struct CoAwaitBase {
  bool await_ready() noexcept { return false; }
  void await_suspend (std::coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

struct CoAwaitSleep : CoAwaitBase
{
  explicit CoAwaitSleep(unsigned ms) : ms {ms} {}
  unsigned ms;
};


struct CoAwaitSpawn : CoAwaitBase {
  CoAwaitSpawn(co_start_fn start, CoTaskId *tid) : start {start}, tid {tid}  {}
  co_start_fn start;
  CoTaskId *tid;
};


struct CoAwaitJoin : CoAwaitBase {
  explicit CoAwaitJoin(CoTaskId tid) : tid {tid} {}
  CoTaskId tid;
};



struct CoTask
{
  struct promise_type
  {
    using coro_handle = std::coroutine_handle<promise_type>;

    CoAwaitData data = {.type= CoAwaitType::None, .data = {} };
    // TODO: handle rethrow exception in scheduler poll
    std::exception_ptr exception_ = nullptr;

    auto get_return_object() { return coro_handle::from_promise(*this); }

    auto initial_suspend() noexcept {  return std::suspend_never(); }

    // suspend_always is needed so scheduler can handle it
    auto final_suspend() noexcept {
      data.type = CoAwaitType::None;
      return std::suspend_always();
    }

    // copy await data for scheduler
    auto await_transform(struct CoAwaitSleep await) noexcept {
      assert(data.type == CoAwaitType::None);

      data.type = CoAwaitType::Sleep;
      data.data.sleep_ms = await.ms;

      return await;
    };

    auto await_transform(CoAwaitSpawn await) noexcept {
      assert(data.type == CoAwaitType::None);

      data.type = CoAwaitType::Spawn;
      data.data.spawn.start = await.start;
      data.data.spawn.tid = await.tid;

      return await;
    };


    auto await_transform(CoAwaitJoin await) noexcept {
      assert(data.type == CoAwaitType::None);

      data.type = CoAwaitType::Join;
      data.data.join_tid = await.tid;

      return await;
    };

    void return_void() {}

    void unhandled_exception() {
      exception_ = std::current_exception();
    }
  };

  CoTask(promise_type::coro_handle handle) : handle_(handle) {}


  CoTask(CoTask const&) = delete;
  CoTask& operator=(CoTask &task) = delete;

  CoTask(CoTask&& task) {
     // new task added to task vector
     assert(handle_ == nullptr);

    task.moved_ = true;
    this->handle_ = std::move(task.handle_);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
  };

  CoTask& operator=(CoTask&& task)  {
    // recycling vector index, check old task
    assert(handle_.done());

    task.moved_ = true;
    this->handle_ = std::move(task.handle_);
    this->uid_ = task.uid_;
    this->parent_ = task.parent_;
    return *this;
  };


  ~CoTask()
  {
    assert(moved_ || handle_.done());
  }

  bool done() {
    return handle_.done();
  }

  CoAwaitData take_data() {
    CoAwaitData data = handle_.promise().data;
    handle_.promise().data.type = CoAwaitType::None;
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

  CoUId uid_ = kCoUIdInvalid;
  CoTaskId parent_ = CoTaskIdInvalid;


private:
  bool ready_ = {};
  bool moved_ = false;
  promise_type::coro_handle handle_ = nullptr;


};

class CoScheduler {
public:
  CoScheduler(co_start_fn start) : spawn_{start} {}

  void Poll() {
    // moves tasks from wait_ to ready_ queue, if timeout elapsed
    CheckWaitingTasks();

    if (spawn_.start) {
      co_start_fn start = spawn_.start;
      spawn_.start = nullptr;

      CoTask task = start();

      if (task.done()) {
        // task finished; no tid tell parent
        if (spawn_.tid)
          *spawn_.tid = CoTaskIdInvalid;
      }

      CoTaskId tid = AddTask(std::move(task));

      if (spawn_.tid) {
        // tell parent tid of child
        *spawn_.tid = tid;
        spawn_.tid = nullptr;
      }

      ProcessResult(tid);
    } else if (!ready_.empty()) {
      // process ready queue

      CoTaskId tid = ready_.front();
      ready_.pop();
      CoTask *task = GetTask(tid);

      assert(task && !task->done());

      task->resume();

      ProcessResult(tid);
    }
  }
  bool done() {
    return wait_.empty() && ready_.empty() && !spawn_.start;
  }

private:

  CoTask* GetTask(CoTaskId tid) {
    if (tid == CoTaskIdInvalid)
      return nullptr;

    CoTask &task = tasks_[tid.index];

    return tid.uid == task.uid_ ? &task : nullptr;
  }

  void SetReady(CoTaskId tid) {
    CoTask *task = GetTask(tid);
    if (!task)
      return;

    if (task->done())
      return;

    if (!task->setReady()) {
        // only push task to ready queue once
        ready_.push(tid);
    }
  }


  CoTaskId AddTask(CoTask &&task)
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
    return CoTaskId{index, task.uid_};
  }


  void CheckWaitingTasks()
  {
    auto now = CoGetTimeout(0);

    for (;;) {
      if (wait_.empty())
        break;

      const CoTimeoutTask &task = wait_.top();

      if (now < task.timeout)
        break;

      SetReady(task.tid);

      wait_.pop();
    }
  }

  void ProcessResult(CoTaskId tid) {
    CoTask *task = GetTask(tid);

    assert(task);

    if (task->done()) {
      SetReady(task->parent_);
      tasks_[tid.index].uid_= kCoUIdInvalid;
      // recycle index
      free_indices_.push(tid.index);
      return;
    }

    CoAwaitData await = task->take_data();

    switch (await.type) {
      case CoAwaitType::None:
        SetReady(tid);
        break;
      case CoAwaitType::Sleep:
        wait_.push(CoTimeoutTask{CoGetTimeout(await.data.sleep_ms), tid});
        break;
      case CoAwaitType::Spawn:
        assert(spawn_.start == nullptr);
        spawn_.start = await.data.spawn.start;
        spawn_.tid = await.data.spawn.tid;
        SetReady(tid);
        break;
      case CoAwaitType::Join:
        CoTask *child = GetTask(await.data.join_tid);

        if (!child || child->done()) {
          // child already done
          SetReady(tid);
          break;
        }

        assert(child->parent_ == CoTaskIdInvalid);
        child->parent_ = tid;

        break;
    }
  }


  // next_uid incremented everytime it is used
  unsigned next_uid_ = 0;

  // task requested spawn; spawn in next poll
  struct {
    co_start_fn start;
    CoTaskId *tid = nullptr;
  } spawn_;

  // all tasks
  std::vector<CoTask> tasks_;

  // can be executed
  std::queue<CoTaskId> ready_;

  // sleeping tasks
  std::priority_queue<CoTimeoutTask, std::vector<CoTimeoutTask>, CoCompareTimeoutTask> wait_;

  // for recycling tasks_indices
  std::queue<unsigned> free_indices_;
};

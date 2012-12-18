#include "ten/task/runtime.hh"
#include "ten/task/deadline.hh"
#include "thread_context.hh"

namespace ten {

namespace {
static std::mutex runtime_mutex;
static std::vector<thread_context *> threads;
static std::atomic<uint64_t> thread_count{0};
static task_pimpl *waiting_task = nullptr;

struct runtime_tag {};
ten::thread_local<runtime_tag, thread_context> this_ctx;

static void add_thread(thread_context *ctx) {
    using namespace std;
    lock_guard<mutex> lock(runtime_mutex);
    threads.push_back(ctx);
    ++thread_count;
}

static void remove_thread(thread_context *ctx) {
    using namespace std;
    lock_guard<mutex> lock(runtime_mutex);
    auto i = find(begin(threads), end(threads), ctx);
    if (i != end(threads)) {
        if (thread_count == 2 && waiting_task) {
            waiting_task->ready();
        }
        threads.erase(i);
        --thread_count;
    }
}

} // anon

thread_context::thread_context() {
    add_thread(this);
}

thread_context::~thread_context() {
    remove_thread(this);
}

///////////// scheduler /////////////

scheduler::scheduler() : _canceled{false} {
    // TODO: reenable this when our libstdc++ is fixed
    //static_assert(clock::is_steady, "clock not steady");
    update_cached_time();
    _task._scheduler = this;
    _current_task = &_task;
}

void scheduler::cancel() {
    _canceled = true;
}

void scheduler::ready(task_pimpl *t) {
    if (t->_ready.exchange(true) == false) {
        if (this != &this_ctx->scheduler) {
            _dirtyq.push(t);
            // TODO: speed this up?
            std::unique_lock<std::mutex> lock{_mutex};
            _cv.notify_one();
        } else {
            DVLOG(5) << "readyq scheduler::ready: " << t;
            _readyq.push_back(t);
        }
    }
}

void scheduler::remove_task(task_pimpl *t) {
    DVLOG(5) << "remove task " << t;
    using namespace std;
    {
        // TODO: might not need this anymore
        // assert not in readyq
        auto i = find(begin(_readyq), end(_readyq), t);
        if (i != end(_readyq)) {
            _readyq.erase(i);
        }
    }

    // TODO: needed?
    //_alarms.remove(t);

    auto i = find_if(begin(_alltasks), end(_alltasks),
            [t](runtime::shared_task &other) {
            return other.get() == t;
            });
    _gctasks.push_back(*i);
    _alltasks.erase(i);
}

int scheduler::dump() {
    LOG(INFO) << &_task;
#ifdef TEN_TASK_TRACE
    LOG(INFO) << _task._trace.str();
#endif
    for (runtime::shared_task &t : _alltasks) {
        LOG(INFO) << t.get();
#ifdef TEN_TASK_TRACE
        LOG(INFO) << t->_trace.str();
#endif
    }
    return 0;
}

void scheduler::check_dirty_queue() {
    task_pimpl *t = nullptr;
    while (_dirtyq.pop(t)) {
        DVLOG(5) << "readyq adding " << t << " from dirtyq";
        _readyq.push_back(t);
    }
}

void scheduler::check_timeout_tasks() {
    _alarms.tick(_now, [this](task_pimpl *t, std::exception_ptr exception) {
        if (exception != nullptr && t->_exception == nullptr) {
            DVLOG(5) << "alarm with exception fired: " << t;
            t->_exception = exception;
        }
        ready(t);
    });
}

void scheduler::schedule() {
    //CHECK(!_alltasks.empty());
    task_pimpl *self = _current_task;

    if (_canceled && !_tasks_canceled) {
        // TODO: maybe if canceled is set we don't let any more tasks spawn?
        _tasks_canceled = true;
        for (auto t : _alltasks) {
            t->cancel();
        }
    }

    do {
        check_dirty_queue();
        update_cached_time();
        check_timeout_tasks();

        if (_readyq.empty()) {
            if (_alltasks.empty()) {
                std::lock_guard<std::mutex> lock(runtime_mutex);
                if (waiting_task && this == waiting_task->_scheduler) {
                    waiting_task->ready();
                    continue;
                }
            }
            std::unique_lock<std::mutex> lock{_mutex};
            check_dirty_queue();
            if (!_readyq.empty()) {
                continue;
            }
            auto when = _alarms.when();
            if (when) {
                _cv.wait_until(lock, *when);
            } else {
                _cv.wait(lock);
            }
        }
    } while (_readyq.empty());

    using ::operator <<;
    DVLOG(5) << "readyq: " << _readyq;

    task_pimpl *t = _readyq.front();
    _readyq.pop_front();
    t->_ready.store(false);
    _current_task = t;
    DVLOG(5) << self << " swap to " << t;
#ifdef TEN_TASK_TRACE
    self->_trace.capture();
#endif
    if (t != self) {
        self->_ctx.swap(t->_ctx, reinterpret_cast<intptr_t>(t));
    }
    _current_task = self;
    _gctasks.clear();
}

void scheduler::attach(runtime::shared_task t) {
    DCHECK(t && t->_scheduler == nullptr);
    t->_scheduler = this;
    _alltasks.push_back(t);
    DVLOG(5) << "attach readyq " << t.get();
    _readyq.push_back(t.get());
}

///////////// runtime ///////////////

task_pimpl *runtime::current_task() {
    return this_ctx->scheduler._current_task;
}

void runtime::sleep_until(const time_point &sleep_time) {
    thread_context *ctx = this_ctx.get();
    task_pimpl *t = ctx->scheduler._current_task;
    scheduler::alarm_clock::scoped_alarm sleep_alarm(ctx->scheduler._alarms, t, sleep_time);
    task_pimpl::cancellation_point cancelable;
    t->swap();
}

runtime::time_point runtime::now() {
    return this_ctx->scheduler._now;
}

runtime::shared_task runtime::task_with_id(uint64_t id) {
    return this_ctx->scheduler.task_with_id(id);
}

void runtime::shutdown() {
    using namespace std;
    lock_guard<mutex> lock(runtime_mutex);
    for (thread_context *r : threads) {
        r->scheduler.cancel();
    }
}

void runtime::wait_for_all() {
    using namespace std;
    CHECK(is_main_thread());
    thread_context *ctx = this_ctx.get();
    {
        lock_guard<mutex> lock(runtime_mutex);
        waiting_task = ctx->scheduler._current_task;
        DVLOG(5) << "waiting for all " << waiting_task;
    }

    while (!ctx->scheduler._alltasks.empty() || thread_count > 1) {
        ctx->scheduler.schedule();
    }

    {
        lock_guard<mutex> lock(runtime_mutex);
        DVLOG(5) << "done waiting for all";
        waiting_task = nullptr;
    }
}


// XXX: only safe to call from debugger
int runtime::dump_all() {
    using namespace std;
    lock_guard<mutex> lock(runtime_mutex);
    for (thread_context *r : threads) {
        LOG(INFO) << "runtime: " << r;
        r->scheduler.dump();
    }
    return 0;
}

void runtime::attach(shared_task t) {
    this_ctx->scheduler.attach(t);
}


///////////// deadline //////////////
// deadline is here because of this_ctx

struct deadline_pimpl {
    scheduler::alarm_clock::scoped_alarm alarm;
};

deadline::deadline(std::chrono::milliseconds ms)
    : _pimpl(std::make_shared<deadline_pimpl>())
{
    if (ms.count() < 0)
        throw errorx("negative deadline: %jdms", intmax_t(ms.count()));
    if (ms.count() > 0) {
        thread_context *r = this_ctx.get();
        task_pimpl *t = r->scheduler._current_task;
        _pimpl->alarm = std::move(scheduler::alarm_clock::scoped_alarm(
                    r->scheduler._alarms, t, ms+runtime::now(), deadline_reached()
                    )
                );
        DVLOG(1) << "deadline alarm armed: " << _pimpl->alarm._armed << " in " << ms.count() << "ms";
    }
}

void deadline::cancel() {
    _pimpl->alarm.cancel();
}

deadline::~deadline() {
    cancel();
}

std::chrono::milliseconds deadline::remaining() const {
    return _pimpl->alarm.remaining();
}

} // ten


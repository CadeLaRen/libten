#ifndef TASK_HH
#define TASK_HH

#include <chrono>
#include <functional>
#include <mutex>
#include <deque>
#include <poll.h>
#include "logging.hh"

//! user must define
extern const size_t default_stacksize;

namespace ten {

extern void netinit();

using namespace std::chrono;

#if (__GNUC__ >= 4 && (__GNUC_MINOR__ >= 7))
typedef steady_clock monotonic_clock;
#endif

//! exception to unwind stack on taskcancel
struct task_interrupted {};

#define SEC2MS(s) (s*1000)

struct task;
struct proc;
typedef std::deque<task *> tasklist;
typedef std::deque<proc *> proclist;

uint64_t taskspawn(const std::function<void ()> &f, size_t stacksize=default_stacksize);
uint64_t taskid();
int64_t taskyield();
bool taskcancel(uint64_t id);
void tasksystem();
const char *taskstate(const char *fmt=0, ...);
const char * taskname(const char *fmt=0, ...);
std::string taskdump();
void taskdumpf(FILE *of = stderr);

uint64_t procspawn(const std::function<void ()> &f, size_t stacksize=default_stacksize);
void procshutdown();

// returns cached time, not precise
const time_point<monotonic_clock> &procnow();

struct procmain {
    procmain();

    int main(int argc=0, char *argv[]=0);
};

void tasksleep(uint64_t ms);
int taskpoll(pollfd *fds, nfds_t nfds, uint64_t ms=0);
bool fdwait(int fd, int rw, uint64_t ms=0);

// inherit from task_interrupted so lock/rendez/poll canceling
// doesn't need to be duplicated
struct deadline_reached : task_interrupted {};
struct deadline {
    void *timeout_id;
    deadline(milliseconds ms);

    deadline(const deadline &) = delete;
    deadline &operator =(const deadline &) = delete;

    ~deadline();
};

} // end namespace ten

#endif // TASK_HH

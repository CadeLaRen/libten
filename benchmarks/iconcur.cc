#include <thread>
#include "semaphore.hh"
#include "task.hh"
#include "logging.hh"

using namespace ten;
const size_t default_stacksize=256*1024;

struct state {
    qutex q;
    rendez r;
    int x;

    state() : x(0) {}
};

void qlocker(std::shared_ptr<state> st) {
    taskname("qlocker");
    for (int i=0; i<1000; ++i) {
        std::unique_lock<qutex> lk(st->q);
        ++(st->x);
    }
    st->r.wakeup();
}

bool is_done(int &x) {
    taskstate("sleeping on rendez: %d %x", x, &x);
    return x == 20*1000;
}

void qutex_task_spawn() {
    taskname("qutex_task_spawn");
    std::shared_ptr<state> st = std::make_shared<state>();
    for (int i=0; i<20; ++i) {
        taskstate("spawning %d", i);
        procspawn(std::bind(qlocker, st));
    }
    std::unique_lock<qutex> lk(st->q);
    st->r.sleep(lk, std::bind(is_done, std::ref(st->x)));
    CHECK(st->x == 20*1000);
}

int main(int argc, char *argv[]) {
    procmain p;
    for (int i=0; i<10; ++i) {
        taskspawn(qutex_task_spawn);
    }
    p.main();
}

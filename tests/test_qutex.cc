#include "gtest/gtest.h"
#include <thread>
#include "ten/semaphore.hh"
#include "ten/synchronized.hh"
#include "ten/thread_guard.hh"
#include "ten/task/rendez.hh"

using namespace ten;

struct state {
    qutex q;
    rendez r;
    int x;

    state() : x(0) {}
};

void qlocker(std::shared_ptr<state> st) {
    for (int i=0; i<1000; ++i) {
        std::unique_lock<qutex> lk{st->q};
        ++(st->x);
    }
    st->r.wakeup();
}

bool is_done(int &x) { return x == 20*1000; }

void qutex_task_spawn() {

    std::shared_ptr<state> st = std::make_shared<state>();
    std::vector<thread_guard> threads;
    for (int i=0; i<20; ++i) {
        threads.emplace_back(task::spawn_thread([=] {
            qlocker(st);
        }));
        this_task::yield();
    }
    std::unique_lock<qutex> lk{st->q};
    st->r.sleep(lk, std::bind(is_done, std::ref(st->x)));
    EXPECT_EQ(st->x, 20*1000);
}

TEST(Qutex, Test1) {
    task::main([=] {
        task::spawn(qutex_task_spawn);
    });
}

TEST(Qutex, Synchronized) {
    synchronized<std::string> s("empty");

    s([](std::string &str) {
        EXPECT_EQ("empty", str);
        str = "test";
    });

    sync(s, [](std::string &str) {
        EXPECT_EQ("test", str);
        str = "test2";
    });

    EXPECT_EQ(*sync_view(s), "test2");
}

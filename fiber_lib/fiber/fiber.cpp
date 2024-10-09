#include "fiber.h"

namespace sylar {

static thread_local Fiber* t_fiber = nullptr;
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
static thread_local Fiber* t_scheduler_fiber = nullptr;

static std::atomic<uint64_t> s_fiber_id{0};
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f) {
    t_fiber = f;
}

std::shared_ptr<Fiber> Fiber::GetThis() {
    if (t_fiber)
        return t_fiber->shared_from_this();
    
    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;
    t_scheduler_fiber = main_fiber.get(); // 除非调用SetSchedulerFiber，否则调度协程为主协程

    assert(t_fiber == main_fiber.get());
    return main_fiber;
}

}
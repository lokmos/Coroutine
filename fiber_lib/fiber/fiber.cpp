#include "fiber.h"

static bool debug = false;

namespace sylar {

// 当前正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 线程局部变量，主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 线程局部变量，调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 下一个创建协程的id
static std::atomic<uint64_t> s_fiber_id{0};
// 协程计数器
static std::atomic<uint64_t> s_fiber_count{0};

// 设置当前协程
void Fiber::SetThis(Fiber *f) {
    t_fiber = f;
}

// 获取当前协程，如果不存在，则创建主协程
std::shared_ptr<Fiber> Fiber::GetThis() {
    if (t_fiber)
        return t_fiber->shared_from_this();
    
    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;
    t_scheduler_fiber = main_fiber.get(); // 除非调用SetSchedulerFiber，否则调度协程为主协程

    assert(t_fiber == main_fiber.get());
    return main_fiber;
}

// 设置调度协程
void Fiber::SetSchedulerFiber(Fiber *f) {
    t_scheduler_fiber = f;
}

// 获取当前协程id
uint64_t Fiber::GetFiberId() {
    if (t_fiber)
        return t_fiber->getId();
    return (uint64_t)-1;
}

/**
* @brief 构造函数
* @attention ⽆参构造函数只⽤于创建线程的第⼀个协程，也就是线程主函数对应的协程，
* 这个协程只能由GetThis()⽅法调⽤，所以定义成私有⽅法
*/
Fiber::Fiber() {
    SetThis(this);
    m_state = RUNNING;

    if (getcontext(&m_ctx)) {
        std::cerr << "Fiber() failed\n";
        pthread_exit(NULL);
    }

    m_id = s_fiber_id++;
    s_fiber_count++;

    if (debug)
        std::cout << "Fiber(): child id = " << m_id << std::endl;
}

/**
* @brief 构造函数，⽤于创建⽤户协程
* @details 本架构中，协程采用独立栈空间，所以需要指定栈空间⼤⼩；构造函数负责分配栈内空间
*/
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler): 
m_cb(cb), m_runInScheduler(run_in_scheduler) {
    m_state = READY;

    m_stacksize = stacksize ? stacksize : 128000;
    m_stack = malloc(m_stacksize);

    if (getcontext(&m_ctx)) {
        std::cerr << "Fiber() failed\n";
        pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    m_id = s_fiber_id++;
    s_fiber_count++;
    
    if (debug)
        std::cout << "Fiber(): child id = " << m_id << std::endl;
}

// 析构函数
Fiber::~Fiber() {
    s_fiber_count--;
    if (m_stack) {
        free(m_stack);
    }
    if (debug)
        std::cout << "~Fiber(): id = " << m_id << std::endl;
}

// 重置协程函数，重置协程状态和入口函数，复用栈空间
void Fiber::reset(std::function<void()> cb) {
    assert(m_stack != nullptr && m_state == TERM);

    m_state = READY;
    m_cb = cb;

    if (getcontext(&m_ctx)) {
        std::cerr << "reset() failed\n";
        pthread_exit(NULL);
    }

    // m_ctx 指向协程上下文
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}


/**
 * @brief 切换到当前协程运行
 * @details 当前协程和正在运行的协程进行交换，前者状态变为RUNNING，后者状态变为READY
 */
void Fiber::resume() {
    // 确认当前协程状态为READY，然后修改为RUNNING
    assert(m_state == READY);
    m_state = RUNNING;

    // 如果在调度器中运行，那么当前协程为调度协程
    if (m_runInScheduler) {
        SetThis(this);
        if (swapcontext(&t_scheduler_fiber->m_ctx, &m_ctx)) {
            std::cerr << "resume() to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    // 否则，当前协程为线程局部变量 t_thread_fiber
    else {
        SetThis(this);
        if (swapcontext(&t_thread_fiber->m_ctx, &m_ctx)) {
            std::cerr << "resume() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

/**
 * @brief 让出当前协程的执行权
 */
void Fiber::yield() {
    // 确认当前协程状态为RUNNING或TERM
    assert(m_state == RUNNING || m_state == TERM);

    // 如果不是TERM，修改为READY
    if (m_state != TERM)
        m_state = READY;

    // 如果在调度器中运行，那么应当让给调度协程
    if (m_runInScheduler) {
        SetThis(t_scheduler_fiber);
        if (swapcontext(&m_ctx, &t_scheduler_fiber->m_ctx)) {
            std::cerr << "yield() to t_scheduler_fiber failed\n";
            pthread_exit(NULL);
        }
    }
    // 否则，应当让给线程局部变量 t_thread_fiber
    else {
        SetThis(t_thread_fiber.get());
        if (swapcontext(&m_ctx, &t_thread_fiber->m_ctx)) {
            std::cerr << "yield() to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

/**
 * @brief 协程入口函数
 * @note 没有处理异常
 */
void Fiber::MainFunc() {
    // 获取当前协程
    std::shared_ptr<Fiber> cur = GetThis();
    assert(cur != nullptr);

    // 触发协程回调函数，使用完后，将其置空（防止误用或占用内存），并将协程状态置为TERM（表示其任务已经完成）
    cur->m_cb();
    cur->m_cb = nullptr;
    cur->m_state = TERM;

    // 释放协程对象，交出控制权
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();
}

}
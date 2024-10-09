#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>
#include <memory>
#include <atomic>
#include <functional>
#include <cassert>
#include <ucontext.h>
#include <unistd.h>
#include <mutex>

namespace sylar {

/*
std::enable_shared_from_this 是 C++ 标准库中的一个模板类，
它位于 <memory> 头文件中，用于帮助管理类对象的共享指针（std::shared_ptr）。
当一个类继承了 std::enable_shared_from_this，
这个类的对象可以安全地从自身创建 std::shared_ptr 实例。
*/
class Fiber : public std::enable_shared_from_this<Fiber> {
public:

    /**
     * @brief 协程状态
     * @details 定义三态转换关系，初始状态为READY，不区分协程是正常结束还是异常结束
     */
    enum State {
        READY,
        RUNNING,
        TERM
    };

    // 锁
    std::mutex m_mutex;

private:
    /**
     * @brief 构造函数
     * @attention 无参构造函数只用于创建第一个协程，即主协程
     * 仅由GetThis()调用，因此为private
     */
    Fiber();

public:
    /**
     * @brief 构造函数，用于创建用户协程
     * @param[in] cb 协程执行函数
     * @param[in] stacksize 协程栈大小
     * @param[in] run_in_scheduler 是否在调度器中运行
     */
    Fiber(std::function<void()> cb, size_t stacksize=0, bool run_in_scheduler=true);
    /**
     * @brief 析构函数
     */
    ~Fiber();

    /**
     * @brief 重置协程函数，重置协程状态和入口函数，复用栈空间
     * @param[in] cb 协程执行函数
     */
    void reset(std::function<void()> cb);

    /**
     * @brief 将当前协程切到到执⾏状态
     * @details 当前协程和正在运⾏的协程进⾏交换，前者状态变为RUNNING，后者状态变为READY
     */
    void resume();
    
    /**
     * @brief 将当前协程切换到暂停状态
     * @details 当前协程和正在运⾏的协程进⾏交换，前者状态变为READY，后者状态变为RUNNING
     */
    void yield();

    // 设置当前线程正在运行的协程，即设置线程局部变量 t_fiber
    static void SetThis(Fiber *f);
    /**
     * @brief 返回当前线程正在执行的协程
     * @details 如果当前线程还未创建协程，则创建线程的第⼀个协程，即主协程
     * @attention 线程如果要创建协程，应当首先执行此函数，来初始化主协程
     */
    static std::shared_ptr<Fiber> GetThis();
    // 设置调度协程（默认为主协程）
    static void SetSchedulerFiber(Fiber *f);

    // 获得当前协程id
    static uint64_t GetFiberId();
    // 协程入口函数
    static void MainFunc();

    // 获取协程ID
    uint64_t getId() const { return m_id; }
    // 获取协程状态
    State getState() const { return m_state; }

private:
    // id
    uint64_t m_id = 0;
    // 协程栈大小
    uint32_t m_stacksize = 0;
    // 协程状态
    State m_state = READY;
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈指针
    void *m_stack = nullptr;
    // 协程函数
    std::function<void()> m_cb;
    // 是否参与调度器调度
    bool m_run_in_scheduler;
};

}

#endif
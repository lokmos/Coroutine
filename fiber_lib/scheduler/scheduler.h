#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include "fiber_lib/fiber/fiber.h"
#include "fiber_lib/thread/thread.h"
#include <mutex>
#include <vector>

namespace sylar {

/**
    * @brief 调度器类
    * @details 调度器类是一个线程池，用于管理多个线程，每个线程都可以执行任务，协程在线程池中切换
*/
class Scheduler {
public:
    /**
        * @brief 创建调度器
        * @param[in] threads 线程数
        * @param[in] use_caller 是否将当前线程也作为调度线程
        * @param[in] name 名称
    */
    Scheduler(size_t threads=1, bool use_caller=true, const std::string& name="Scheduler");
    virtual ~Scheduler();

    const std::string& getName() const { return m_name; }
    // 获取当前正在运行的调度器（静态方法，属于全局唯一调度器）
    static Scheduler* GetThis();

protected:
    void SetThis();

public:
    // 将任务添加到任务队列中，并选择指定的线程执行任务
    template<class FiberOrCb>
    void scheduleLock(FiberOrCb fc, int thread=-1) {
        bool need_tickle; // 是否需要唤醒线程
        {   
            // 加锁，保护任务队列
            std::lock_guard<std::mutex> lock(m_mutex);
            need_tickle = m_tasks.empty();  // 任务队列为空时，需要唤醒线程

            // 将任务添加到任务队列中
            ScheduleTask task(fc, thread);
            if (task.fiber || task.cb) {
                m_tasks.push_back(task);
            }
        }
        // 唤醒线程
        if (need_tickle) {
            tickle();
        }
    }

    // 启动线程池，开启多个线程来执行任务
    virtual void start();
    // 停止线程池，停止所有线程
    virtual viud stop();

protected:
    // 唤醒线程
    virtual void tickle();
    // 线程执行的核心函数，负责从任务队列中取出任务并执行
    virtual void run();
    // 当没有任务时，线程进入空闲状态时执行的函数
    virtual void idle(); 
    // 判断调度器是否可以关闭（所有任务执行完成，所有线程空闲）
    virtual bool stopping();
    // 判断是否有空闲线程
    bool hasIdleThreads() {return m_idleThreadCount > 0;}

private:
    // 任务结构体，封装了一个协程（fiber）或回调函数（cb），以及该任务需要运行的线程id
    struct ScheduleTask {
        std::shared_ptr<Fiber> fiber; // 协程
        std::function<void()> cb; // 回调函数
        int thread; // 指定任务运行的线程id

        ScheduleTask() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }

        // 使用协程初始化任务
        ScheduleTask(std::shared_ptr<Fiber> f, int thr){
            fiber = f;
            thread = thr;
        }

        // 使用协程指针的指针初始化任务
        ScheduleTask(std::shared_ptr<Fiber>* f, int thr) {
            fiber.swap(*f);
            thread = thr;
        }

        // 使用回调函数初始化任务
        ScheduleTask(std::function<void()> f, int thr){
            cb = f;
            thread = thr;
        }

        // 使用回调函数指针的指针初始化任务
        ShceduleTask(std::function<void()>* f, int thr) {
            cb.swap(*f);
            thread = thr;
        }

        // 重置任务
        void reset() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    std::string m_name;  // 调度器名称
    std::mutex m_mutex;  // 互斥锁，用于保护任务队列
    std::vector<std::shared_ptr<Thread>> m_threads;  // 线程池，存储所有工作线程
    std::vector<ScheduleTask> m_tasks;  // 任务队列，存储待执行的任务
    std::vector<int> m_threadIds;  // 存储工作线程的线程id
    size_t m_threadCount = 0;  // 需要创建的线程数量
    std::atomic<size_t> m_activeThreadCount = {0};  // 活跃线程数量
    std::atomic<size_t> m_idleThreadCount = {0};  // 空闲线程数量

    bool m_useCaller;  // 是否将主线程作为工作线程
    std::shared_ptr<Fiber> m_schedulerFiber;  // 调度器的主协程
    int m_rootThread = -1;  // 主线程id
    bool m_stopping = false;  // 标志调度器是否正在停止
};

}


#endif
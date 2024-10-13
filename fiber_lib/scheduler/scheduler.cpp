#include "scheduler.h"

static bool debug = false;

namespace sylar {

static thread_locl Scheduler* t_scheduler = nullptr;  // 线程局部变量，存储当前线程的调度器指针

// 获取当前线程的调度器
Scheduler* Scheduler::GetThis() {
    return t_scheduler;
}

// 设置当前线程的调度器为当前实例（Scheduler对象）
void Scheduler::SetThis() {
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name) 
    : m_useCaller(use_caller), m_name(name) {
    // 确保线程数大于0，且当前线程没有调度器
    assert(threads > 0 && Scheduler::GetThis() == nullptr);

    SetThis();  // 设置当前线程的调度器为当前实例（Scheduler对象）
    Thread::SetName(name); 

    // 如果use_caller为true，则将主线程作为工作线程
    if (use_caller) {
        threads--; // 主线程作为工作线程时，减去一个线程

        // 创建主协程
        Fiber::GetThis();

        // 创建调度协程，绑定调度器的 `run` 方法，优先级为 0，协程退出后返回主协程
        m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        // 将调度协程设置为当前的协程
        Fiber::SetScheduler(m_schedulerFiber.get());

        m_rootThread = Thread::GetThreadId();  // 获取主线程id
        m_threadIds.push_back(m_rootThread);  // 将主线程id添加到线程id数组中
    }

    m_threadCount = threads;  // 设置线程数量
    if(debug) std::cout << "Scheduler::Scheduler() success\n";  // 调试日志
}

Scheduler::~Scheduler() {
    // 确保调度器在析构时已经停止
    assert(stopping() == true);

    // 清空线程本地的调度器指针
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
    if (debug) std::cout << "Scheduler::~Scheduler() success\n";  // 调试日志
}

// 启动调度器
void Scheduler::start() {
    // 加锁
    std::lock_guard<std::mutex> lock(m_mutex);
    // 如果调度器已经停止，则直接返回
    if (m_stopping) {
        std::cerr << "Scheduler::start() scheduler is stopped\n";
        return;
    }
    assert(m_threads.empty());  // 确保线程池为空
    m_threads.resize(m_threadCount);  // 重置线程池大小
    for (size_t i = 0; i < m_threadCount; ++i) {
        // 为每个线程创建并启动调度器运行的线程，绑定 `run` 函数
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        // 加入线程id数组
        m_threadIds.push_back(m_threads[i]->getId());
    }
    if(debug) std::cout << "Scheduler::start() success\n";  // 调试日志
}

// 调度器的核心函数，负责从任务队列中取出任务并执行
void Scheduler::run() {
    int thread_id = Thread::GetThreadId();  // 获取当前线程id

    SetThis();  // 设置当前线程的调度器为当前实例（Scheduler对象）

    // 如果当前线程id不是主线程id，则需要创建主协程
    if (thread_id != m_rootThread) {
        Fiber::GetThis();
    }

    // 创建空闲协程，当没有任务时，执行空闲协程
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));  

    ScheduleTask task;  // 任务结构体
    while (true) {
        task.reset();
        bool tickle_me = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);  // 加锁
            auto it = m_tasks.begin();

            // 1.遍历任务队列
            while (it != m_tasks.end()) {
                // 如果任务指定了特定的线程且不是当前线程，跳过该任务
				if(it->thread != -1 && it->thread != thread_id)
				{
					it++;
					tickle_me = true;  // 标记需要唤醒其他线程
					continue;
				}

                // 2.取出任务
                assert(it->fiber || it->cb);
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;  // 活跃线程数加1
                break;
            }

            tickle_me = tickle || (it != m_tasks.end());  // 如果任务队列不为空，标记需要唤醒其他线程
        }

        if (tickle_me) {
            tickle();  // 唤醒其他线程
        }

        // 3.执行任务
        if (task.fiber) {
            // resume协程，返回时，要么执行完了，要么yield了，总之都看作任务完成
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (task.fiber->getState() != Fiber::TERM)
                    task.fiber->resume();
            }
            m_activeThreadCount--;
            task.reset();
        }
        else if (task.cb) {
            // 将回调函数封装为协程并执行
			std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
			{
				std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
				cb_fiber->resume();  // 恢复协程的执行
			}
			m_activeThreadCount--;  // 活跃线程数减少
			task.reset();  // 重置任务
        }
        // 4.如果没有任务，执行空闲协程
        else {
            // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那⼀定是调度器停⽌了
            if (idle_fiber->getState() == Fiber::TERM) {
                break;
            }
            m_idleThreadCount++;
            idle_fiber->resume();
            m_idleThreadCount--;
        }
    }
}

// 停止调度器
void Scheduler::stop() {
    if (stopping())
        return;

    m_stopping = true;  // 标记调度器停止
    
    if (m_useCaller)
        assert(GetThis() == this);
    else
        assert(GetThis() != this);

    // 唤醒所有线程
    for (size_t i = 0; i < m_threadCount; ++i) {
        tickle();
    }
    
    if (m_schedulerFiber)
        tickle(); // 唤醒主协程

    if (m_schedulerFiber) {
        m_schedulerFiber->resume();  // 恢复调度协程的执行
    }

    // 等待所有线程执行结束
    std::vector<std::shared_ptr<Thread>> thrs;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		thrs.swap(m_threads);
	}
    for (auto& i : thrs) {
        i->join();
    }
}

// 唤醒线程
void Scheduler::tickle() {
    if (debug) std::cout << "Scheduler::tickle() success\n";  // 调试日志
}

// 空闲协程
void Scheduler::idle() {
    while (!stopping()) {
        sleep(1);
        Fiber::GetThis()->yield();
    }
}

// 判断调度器是否可以停止
bool Scheduler::stopping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;  // 如果任务为空且没有活跃线程，则可以停止
}

}
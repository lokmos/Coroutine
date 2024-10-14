#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "fiber_lib/scheduler.h"
#include "fiber_lib/timer.h"

namespace sylar {
// IOManager类的工作流程
// 1. 注册一个事件 -> 2. 等待事件准备就绪 -> 3. 调度回调 -> 4. 注销事件 -> 5. 执行回调
class IOManager : public Scheduler, public TimerManager {
public:
    // 参照epoll定义事件类型，只关心读写事件，其余事件会归类到这两类
    enum Event {
        NONE = 0x0,    // 没有事件
        READ = 0x1,    // 读事件，对应EPOLLIN
        WRITE = 0x4    // 写事件，对应EPOLLOUT
    }

private:
    /**
    * @brief socket fd上下⽂类
    * @details 每个socket fd都对应⼀个FdContext，包括fd的值，fd上的事件，以及fd的读写事件上下⽂
    */
    struct FdContext {
        // 事件上下文类，保存了这个事件的回调函数以及执行回调函数的调度器
        struct EventContext {
            Scheduler* scheduler = nullptr;    // 事件的调度器
            std::shared_ptr<Fiber> fiber;    // 事件的协程
            std::function<void()> cb;    // 事件的回调函数
        };

        EventContext read;   // 读事件上下文
        EventContext write;  // 写事件上下文
        int fd = 0;   // 文件描述符（事件关联的句柄）
        Event events = NONE; // 事件类型（该fd添加了哪些事件的回调函数，或者说应该关心哪些事件）
        std::mutex mutex; // 事件的锁

        // 获取事件的上下文，返回读或写事件的上下文
        EventContext& getEventContext(Event event);
        // 重置某个事件的上下文
        void resetEventContext(EventContext &ctx);
        // 触发某个事件，调用相应的回调函数
        void triggerEvent(Event event);   
    };

public:
    // IOManager的构造函数，初始化线程数、是否使用调用者线程、名称等参数
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    // 添加一个事件，fd为文件描述符，event为事件类型，cb为回调函数
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // 删除某个事件
    bool delEvent(int fd, Event event);
    // 删除某个事件并触发其回调
    bool cancelEvent(int fd, Event event);
    // 删除所有事件并触发其回调
    bool cancelAll(int fd);

    // 获取当前线程的IOManager对象
    static IOManager* GetThis();

protected:
    // tickle函数，用于唤醒idle中的线程
    void tickle() override;   
    // 判断是否可以停止
    bool stopping() override;   
    // 空闲状态时的处理函数
    void idle() override;
    // 当定时器插入到队列最前面时调用
    void onTimerInsertedAtFront() override;
    // 扩展上下文数组的大小
    void contextResize(size_t size);

private:
    int m_epfd = 0;  // epoll文件描述符，用于事件监听
    int m_tickleFds[2];  // 用于线程之间的通信，fd[0]为读端，fd[1]为写端
    std::atomic<size_t> m_pendingEventCount = {0};  // 待处理事件数量
    std::shared_mutex m_mutex;  // 共享锁，用于保护fd上下文的并发访问
    std::vector<FdContext*> m_fdContexts;  // fd上下文数组
};
}

#endif
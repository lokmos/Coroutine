#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>
#include "ioscheduler.h"

static bool debug = true;

namespace sylar {

// 获取当前线程的 IOManager 实例
IOManager* IOManager::GetThis() {
    // dynamic_cas t用于向下转换：从基类指针或引用转换为派生类的指针或引用
    // 其确保这种转换是安全的，并只有在类型转换是合法的情况下才会成功
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

// 获取某个事件的上下文
IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) {
    assert(event == READ || event == WRITE);
    switch(event) {
        case READ:
            return read;
        case WRITE:
            return write;
    }
    throw std::invalid_argument("getEventContext: unsupported event type");
}

// 重置某个事件的上下文
void IOManager::FdContext::resetEventContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.cb = nullptr;
    ctx.fiber.reset();
}

// 触发某个事件
void IOManager::FdContext::triggerEvent(Event event) {
    // 确保事件已经被注册
    assert(events & event);
    // 从已注册事件中删除该事件
    events = (Event)(events & ~event);
    EventContext &ctx = getEventContext(event);
    if (ctx.cb) {
        ctx.scheduler->scheduler(&ctx.cb);
    }
    else {
        ctx.scheduler->scheduler(&ctx.fiber);
    }
    resetEventContext(ctx);
}

// IOManager 构造函数，初始化 epoll 文件描述符和线程池
IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) : 
Scheduler(threads, use_caller, name), TimerManager() {
    // 创建 epoll 文件描述符
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // 创建管道，用于唤醒 epoll_wait
    int rt = pipe(m_tickleFds);
    assert(!rt);

    // 添加管道读端到 epoll 中
    epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    // 设置管道读端为非阻塞
    rt = fctnl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    // 将管道读端添加到 epoll 监听
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    // 初始化文件描述符上下文数组
    contextResize(32);

    start();
}

// IOManager 析构函数
IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i])
            delete m_fdContexts[i];
    } 
}

// 扩展文件描述符上下文数组的大小
void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size);

    // 初始化新的文件描述符上下文
    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i] == nullptr) {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // 获取文件描述符上下文
    FdContext *fd_ctx = nullptr;
    // 读锁
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if (m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else {
        read_lock.unlock();
        // 写锁
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 2);
        fd_ctx = m_fdContexts[fd];
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // 判断是否已经添加了该事件
    if (fd_ctx->events & event)
        return -1;

    // 添加新事件
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | EPOLLIN | EPOLLOUT;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        std::cerr << "epoll_ctl(" << m_epfd << ", " << op << ", " << fd << ", " << epevent.events << "): " << rt << " (" << errno << ") (" << strerror(errno) << ")" << std::endl;
        return -1;
    }

    ++m_pendingEventCount;
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // 更新事件上下文
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb)
        event_ctx.cb.swap(cb);
    else {
        event_ctx.fiber = Fiber::GetThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;
}

}
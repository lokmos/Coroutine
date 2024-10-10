#ifndef _THREAD_H_
#define _THREAD_H_

#include <mutex>
#include <condition_variable>
#include <functional>

namespace sylar {

// 实现信号量功能，用于线程间同步
class Semaphore {
private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;
public:
    // 信号量初始化为0
    explicit Semaphore(int count_=0) : count(count_) {}

    // P
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        while (count == 0)
            cv.wait(lock);
        count--;
    }

    // V
    void signal() {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();
    }
};

// 线程类，这是一个类，不是真正的线程，其要在某个线程中运行
class Thread {
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();
    pid_t getId() const {return m_id;}
    const std::string& getName() const {return m_name;}
    // 等待线程结束，相当于pthread_join
    void join();
public:
    static pid_t GetThreadId();
    static Thread* GetThis();
    static const std::string& GetName();
    static void SetName(const std::string& name);
private:
    static void* run(void* arg);
private:
    pid_t m_id = -1; // 线程id
    pthread_t m_thread = 0; // POSIX线程句柄，用于管理线程
    std::function<void()> m_cb; // 线程回调函数
    std::string m_name; // 线程名称
    Semaphore m_semaphore; // 信号量，用于线程同步
};

}

#endif
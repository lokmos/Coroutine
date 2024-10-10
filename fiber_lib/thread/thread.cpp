#include "thread.h"
#include <sys/syscall.h>
#include <iostream>
#include <unistd.h>

namespace sylar {
// 线程局部变量，每个线程都有自己独立的t_thread和t_thread_name
// 这些变量用于存储当前线程的Thread对象指针和线程的名称
// 每个线程有自己独立的这两个变量，它们在每个线程的生命周期中都是独立且互不干扰的
static thread_local Thread* t_thread          = nullptr;       // 当前线程的Thread对象指针
static thread_local std::string t_thread_name = "UNKNOWN";     // 当前线程的名称，初始为"UNKNOWN"

pid_t Thread::GetThreadId() {
    return syscall(SYS_gettid);
}

Thread* Thread::GetThis() {
    return t_thread;
}

const std::string& GetName() {
    return t_thread_name;
}

void Thread::SetName(const std::string& name) {
    if (t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string &name) : m_cb(cb), m_name(name){
    // 创建线程，pthread_create的第三个参数为线程启动函数，即Thread::run
    int rt =pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt) {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    // 等待线程初始化完成
    //  m_semaphore.wait()调用在主线程中阻塞主线程的执行，直到新线程调用m_semaphore.signal()为止。
    //  这可以确保主线程在继续执行之前，等待新线程完成其初始化过程。
    m_semaphore.wait();
}

Thread::~Thread() {
    if (m_thread) {
        // 将线程与主线程分类，这种线程不需要pthread_join()，当它终止时，系统会自动回收它的资源，不会占用系统资源。
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join() {
    if (m_thread) {
        int rt =pthread_join(m_thread, nullptr);
        if (rt) {
            std::cerr << "pthread_join thread fail, rt=" << rt << " name=" << m_name;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*)arg;
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = GetThreadId();

    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);

    thread->m_semaphore.signal();

    cb();
    return 0;
}

}
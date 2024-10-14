#include "timer.h"

namespace sylar {

/**
    * @brief 取消一个定时器
    * @details 如果该定时器有一个有效的回调函数，需要将其置空；从定时器管理器(set)中删除该定时器
    * @attention 使用锁，因为多个线程可能同时访问TimerManager
    * @return 如果定时器的回调函数已经为空，则返回 false; 否则返回 true 表示取消成功
 */
bool Timer::cancel() {
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if (m_cb == nullptr) {
        return false;
    }

    m_cb = nullptr;
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it != m_manager->m_timers.end()) {
        m_manager->m_timers.erase(it);
    }
    return true;
}

/**
    * @brief 刷新定时器，认为定时器只可能往后调整
    * @details 如果定时器的回调函数为空，则返回 false; 否则重新设置定时器的超时时间，然后将其插入到定时器管理器(set)中
    * @attention 使用锁，因为多个线程可能同时访问TimerManager
    * @return 如果定时器的回调函数为空，则返回 false; 否则返回 true 表示刷新成功
 */
bool Timer::refresh() {
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if (m_cb == nullptr) {
        return false;
    }

    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }

    m_manager->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

/**
    * @brief 重置定时器时间
    * @details 如果定时器的回调函数为空，则返回 false; 否则重新设置定时器的超时时间，然后将其插入到定时器管理器(set)中
    * @attention 使用锁，因为多个线程可能同时访问TimerManager
    * @param[in] ms 定时器执行间隔时间(毫秒)
    * @param[in] from_now 是否从当前时间开始计算，如果为false，则从上一次触发的时间开始计算
    * @return 如果定时器的回调函数为空，则返回 false; 否则返回 true 表示重置成功
 */
bool Timer::reset(uint64_t ms, bool from_now) {
    if (ms == m_ms && !from_now) {
        return true;
    }

    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);
        if (!m_cb) {
            return false;
        }
        auto it = m_manager->m_timers.find(shared_from_this());
        if (it == m_manager->m_timers.end()) {
            return false;
        }
        m_manager->m_timers.erase(it);
    }

    // 重新计算触发时间并插入定时器
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::millseconds(m_ms);
    m_manager->addTimer(shared_from_this());
    return true;
}

// 定时器构造函数
Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager) {
    auto now = std::chrono::system_clock::now();
    m_next = now + std::chrono::milliseconds(m_ms);
}

// 最小堆比较函数
bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const{
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;  // 比较两个定时器的触发时间，按时间升序排序
}

TimerManager::TimerManager() {
    m_previouseTime = std::chrono::system_clock::now();
}

TimerManager::~TimerManager() {
}

// 构造一个定时器并插入到定时器管理器(set)中
std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);
    return timer;
}

// 如果条件存在，执行回调函数
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    // 这里使用了 std::weak_ptr，它是一个不拥有对象的弱引用。当对象被销毁时，weak_ptr 不会延长对象的生命周期。
    // 通过 weak_cond.lock()，我们可以尝试获取一个对应的 std::shared_ptr，这会检查 weak_cond 所指向的对象是否还存在。
    // 如果 weak_cond 指向的对象已经销毁，lock() 会返回一个空的 shared_ptr，表示条件不满足。
    std::shared_ptr<void> tmp = weak_cond.lock();
    // 如果 tmp 是非空的 shared_ptr，则意味着 weak_cond 所指的对象仍然存在。
    if (tmp) {
        cb();
    }
}

// 创建一个带有条件的定时器
std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}


/**
    * @brief 添加定时器
    * @details 将定时器插入到定时器管理器(set)中，如果插入的定时器位于集合的最前面且尚未唤醒任何线程，则唤醒一个等待的线程来处理新插入的定时器。
    * @attention 使用锁，因为多个线程可能同时访问TimerManager
 */
void TimerManager::addTimer(std::shared_ptr<Timer> timer) {
    bool at_front = false;

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_timers.insert(timer).first;
        at_front = it == timers.begin() && !m_tickled; // 判断是否在最前面并且尚未唤醒
        if (at_front) {
            m_tickled = true;
        }
    }

    if (at_front) {
        onTimerInsertedAtFront(); // 唤醒线程
    }
}

// 返回下一个到期的定时器的时间，如果没有定时器，返回最大值；如果已经到期，返回0
uint64_t TimerManager::getNextTimer() {
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    m_tickled = false;

    if (m_timers.empty()) {
        return ~0ull;
    }

    auto now = std::chrono::system_clock::now();
    auto time = (*m_timers.begin())->m_next;

    if (now >= time)
        return 0;
    else {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
        return static_cast<uint64_t>(duration.count());
    }
}

// 列出所有到期的定时器，并将它们的回调函数添加到 cbs 向量中。对于循环触发的定时器，重新计算它们的下一次触发时间并重新插入集合。
void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
    auto now = std::chrono::system_clock::now();
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    bool rollover = detectClockRollover();  // 检测是否发生时钟回退

    // 如果时钟回退或有定时器超时，处理超时定时器
    while (!m_timers.empty() && (rollover || (*m_timers.begin()->m_next <= now))) {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());
        cbs.push_back(temp->m_cb);

        if (temp->m_recurring) {
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else {
            temp->m_cb = nullptr;
        }
    }
}

bool TimerManager::hasTimer() {
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return !m_timers.empty();
}

// 检测时钟回退，如果当前时间比上次记录时间少1h以上，则认为发生时钟回退
bool TimerManager::detectClockRollover() {
    bool rollover = false;
    auto now = std::chrono::system_clock::now();
    if(now < (m_previouseTime - std::chrono::milliseconds(60 * 60 * 1000)))
    {
        rollover = true;  // 如果当前时间比之前时间少一个小时，表示时钟回退
    }
    m_previouseTime = now;
    return rollover;
}
}
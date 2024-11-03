#include "timer.h"

namespace sylar
{
    Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager)
        : m_recurring(recurring), m_timeout_ms(ms), m_cb(cb), m_manager(manager)
    {
        auto now = std::chrono::system_clock::now();
        m_timeout = now + std::chrono::milliseconds(ms);
    }

    bool Timer::cancel()
    {
        if (m_cb == nullptr)
        {
            return false;
        }
        else
        {
            m_cb = nullptr;
        }

        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        auto it = m_manager->m_timers.find(shared_from_this());
        if (it != m_manager->m_timers.end())
        {
            m_manager->m_timers.erase(it);
        }
        // 找不到就说明已经取消
        //  else{
        //      return false;
        //  }
        return true;
    }

    bool Timer::refresh()
    {
        if (m_cb == nullptr)
        {
            return false;
        }
        else
        {
            // 要操作队列必须上锁
            std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

            auto it = m_manager->m_timers.find(shared_from_this());
            if (it != m_manager->m_timers.end())
            {
                m_manager->m_timers.erase(it);
                m_timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(m_timeout_ms);
                m_manager->m_timers.insert(shared_from_this());
            }
            else
            {
                return false;
            }
            return true;
        }
    }

    bool Timer::reset(uint64_t ms, bool from_now)
    {
        if (m_cb == nullptr)
        {
            return false;
        }

        // 要操作队列必须上锁
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        auto it = m_manager->m_timers.find(shared_from_this());
        if (it != m_manager->m_timers.end())
        {
            m_manager->m_timers.erase(it);
            auto start = from_now ? std::chrono::system_clock::now() : m_timeout - std::chrono::milliseconds(m_timeout_ms);
            m_timeout_ms = ms;
            m_timeout = start + std::chrono::milliseconds(m_timeout_ms);
            m_manager->m_timers.insert(shared_from_this());
            return true;
        }
        else
        {
            return false;
        }
    }

    bool Timer::Comparator::operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs) const
    {
        assert(lhs != nullptr && rhs != nullptr);
        return lhs->m_timeout < rhs->m_timeout;
    }

    TimerManager::TimerManager()
    {
        m_previouseTime = std::chrono::system_clock::now();
    }

    TimerManager::~TimerManager()
    {
    }

    // 添加timer
    std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
    {
        std::shared_ptr<Timer> sp_timer(new Timer(ms, cb, recurring, this));
        addTimer(sp_timer);
        return sp_timer;
    }

    static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
    {
        auto it = weak_cond.lock();
        if (it)
        {
            cb();
        }
    }

    // 添加条件timer
    std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
    {
        return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
    }

    void TimerManager::addTimer(std::shared_ptr<Timer> timer)
    {
        bool at_front = false;
        {
            std::lock_guard<std::shared_mutex> locker(m_mutex);
            auto it = m_timers.insert(timer).first;
            at_front = (it == m_timers.begin()) && !m_tickled;

            // only tickle once till one thread wakes up and runs getNextTime()
            if (at_front)
            {
                m_tickled = true;
            }
        }
        if (at_front)
        {
            onTimerInsertedAtFront();
        }
    }

    bool TimerManager::hasTimer()
    {
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        return !m_timers.empty();
    }

    // 拿到堆中最近的超时时间
    uint64_t TimerManager::getNextTimer()
    {
        // 读共享锁
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);

        // reset m_tickled
        m_tickled = false;

        if (m_timers.empty())
        {
            // 返回最大值
            return ~0ull;
        }

        auto time = (*m_timers.begin())->m_timeout;
        auto now = std::chrono::system_clock::now();
        if (now >= time)
        {
            return 0;
        }
        else
        {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
            return static_cast<uint64_t>(duration.count());
        }
    }

    // 取出所有超时定时器的回调函数
    void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
    {
        std::lock_guard<std::shared_mutex> locker(m_mutex);

        auto now = std::chrono::system_clock::now();

        bool rollover = detectClockRollover();

        while (!m_timers.empty() && rollover || !m_timers.empty() && (*m_timers.begin())->m_timeout <= now)
        {
            auto it = *m_timers.begin();
            m_timers.erase(it);
            cbs.push_back(it->m_cb);

            if (it->m_recurring)
            {
                // 用refresh和addTimer会造成互斥锁的竞争
                // it->refresh();
                // addTimer(it);
                it->m_timeout = now + std::chrono::milliseconds(it->m_timeout_ms);
                m_timers.insert(it);
            }
            else
            {
                // 清理cb
                it->m_cb = nullptr;
            }
        }
    }

    // 每过一个小时自动检测是否有修改系统时间
    bool TimerManager::detectClockRollover()
    {
        bool rollover = false;
        auto now = std::chrono::system_clock::now();
        if (now < (m_previouseTime - std::chrono::milliseconds(60 * 60 * 1000)))
        {
            rollover = true;
        }
        m_previouseTime = now;
        return rollover;
    }
}
#include "scheduler.h"
static bool debug = true;
namespace sylar
{

    static thread_local Scheduler *t_scheduler = nullptr;

    Scheduler *Scheduler::GetThis()
    {
        return t_scheduler;
    }

    void Scheduler::SetThis()
    {
        t_scheduler = this;
    }

    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
        : m_useCaller(use_caller), m_name(name)
    {
        assert(threads > 0 && Scheduler::GetThis() == nullptr);

        SetThis();

        Thread::SetName(m_name);

        // 仅创造调度协程，不构造线程池
        if (use_caller)
        {
            threads--;

            Fiber::GetThis();

            // m_schedulerFiber = std::make_shared<Fiber>(std::bind(Scheduler::start, 0, false));
            m_schedulerFiber = std::make_shared<Fiber>(std::bind(&Scheduler::run, this), 0, false);
            Fiber::SetSchedulerFiber(m_schedulerFiber.get());

            m_rootThread = Thread::GetThreadId();
            m_threadIds.push_back(m_rootThread);
        }
        m_threadCount = threads;
        if (debug)
            std::cout << "Scheduler::Scheduler() success\n";
    }

    Scheduler::~Scheduler()
    {
        assert(stopping() == true);
        if (GetThis() == this)
        {
            t_scheduler = nullptr;
        }
        if (debug)
            std::cout << "Scheduler::~Scheduler() success\n";
    }

    void Scheduler::start()
    {
        std::lock_guard<std::mutex> locker(m_mutex);

        if (m_stopping)
        {
            std::cerr << "Scheduler is stopped" << std::endl;
            return;
        }

        assert(m_threads.empty());
        m_threads.resize(m_threadCount);
        for (size_t i = 0; i < m_threadCount; i++)
        {
            // m_threads.push_back(
            //     std::make_shared<Thread>(
            //         std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
            // 估计是为了可重复使用采用reset
            m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
            m_threadIds.push_back(m_threads[i]->getId());
        }
        if (debug)
            std::cout << "Scheduler::start() success\n";
    }

    // 1.遍历task队列
    // 2.取task到对应thread执行
    // 3.thread执行task
    // 4.等待idle
    void Scheduler::run()
    {
        int thread_id = Thread::GetThreadId();
        if (debug)
            std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;

        ScheduleTask task;
        std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));

        SetThis();

        // ===================================================
        // 运行在新创建的线程 -> 需要创建主协程
        if (thread_id != m_rootThread)
        {
            Fiber::GetThis();
        }
        // ===================================================

        while (1)
        {
            task.reset();
            bool needTickle = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.begin();
                // 1. 一次只取一个任务
                while (it != m_tasks.end())
                {

                    if (it->threadId != -1 && it->threadId == thread_id)
                    {
                        it++;
                        needTickle = true;
                    }

                    // 2.get task
                    task = *it;
                    m_tasks.erase(it);
                    m_activeThreadCount++;
                    break;
                }
                needTickle = needTickle |= (it != m_tasks.end());
            }

            if (needTickle)
            {
                tickle();
            }

            // 3. run task
            if (task.fiber)
            {
                {
                    std::lock_guard<std::mutex> locker(task.fiber->m_mutex);
                    if (task.fiber->getState() != Fiber::TERM)
                    {
                        task.fiber->resume();
                    }
                    m_activeThreadCount--;
                }
            }
            else if (task.cb)
            {
                Fiber *fiber = new Fiber(task.cb);
                fiber->resume();
                m_activeThreadCount--;
            }

            // 4.idle
            else
            {
                if (idle_fiber->getState() == Fiber::TERM)
                {
                    if (debug)
                        std::cout << "Scheduler::run() ends in thread: " + std::to_string(thread_id) + "\n";
                    break;
                }
                m_idleThreadCount++;
                idle_fiber->resume();
                m_idleThreadCount--;
            }
        }
    }

    void Scheduler::stop()
    {
        if (debug)
            std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;

        if (stopping())
        {
            return;
        }

        m_stopping = true;

        //========================================
        if (m_useCaller)
        {
            assert(GetThis() == this);
        }
        else
        {
            assert(GetThis() != this);
        }
        //========================================

        for (size_t i = 0; i < m_threadCount; i++)
        {
            tickle();
        }

        if (m_schedulerFiber)
        {
            tickle();
        }

        if (m_schedulerFiber)
        {
            m_schedulerFiber->resume();
            if (debug)
                std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
        }

        std::vector<std::shared_ptr<Thread>> threads;
        {
            std::lock_guard<std::mutex> locker(m_mutex);
            threads.swap(m_threads);
        }

        for (auto &it : threads)
        {
            it->join();
        }
        if (debug)
            std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
    }

    void Scheduler::tickle()
    {
    }

    void Scheduler::idle()
    {
        while (!stopping())
        {
            if (debug)
                std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;
            sleep(1);
            Fiber::GetThis()->yield();
        }
    }

    bool Scheduler::stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
    }

}
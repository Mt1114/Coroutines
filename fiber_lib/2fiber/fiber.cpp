#include "fiber.h"

static bool debug = true;

namespace sylar
{

    static thread_local Fiber *t_fiber = nullptr;
    static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
    static thread_local Fiber *t_scheduler_fiber = nullptr;

    static std::atomic<uint64_t> fiber_id{0};
    static std::atomic<uint64_t> fiber_count{0};

    void Fiber::SetThis(Fiber *fiber)
    {
        t_fiber = fiber;
    }

    // 设置调度协程（默认为主协程）
    void Fiber::SetSchedulerFiber(Fiber *f)
    {
        t_scheduler_fiber = f;
    }

    Fiber::Fiber()
    {
        SetThis(this);
        m_state = RUNNING;

        if (getcontext(&m_ctx))
        {
            std::cerr << "Fiber() failed\n";
            pthread_exit(NULL);
        }

        m_id = fiber_id++;
        fiber_count++;
        if (debug)
            std::cout << "Fiber(): main id = " << m_id << std::endl;
    }

    Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
        : m_cb(cb),
          m_runInScheduler(run_in_scheduler)
    {
        m_state = READY;
        m_stacksize = stacksize ? stacksize : 128000;
        m_stack = malloc(m_stacksize);

        if (getcontext(&m_ctx))
        {
            std::cerr << "Fiber() failed\n";
            pthread_exit(NULL);
        }

        m_ctx.uc_link = nullptr;
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;
        makecontext(&m_ctx, &Fiber::MainFunc, 0);

        m_id = fiber_id++;
        fiber_count++;
        if (debug)
            std::cout << "Fiber(): child id = " << m_id << std::endl;
    }

    Fiber::~Fiber()
    {
        fiber_count--;
        if (m_stack)
        {
            free(m_stack);
        }
        if (debug)
            std::cout << "~Fiber(): id = " << m_id << std::endl;
    }

    std::shared_ptr<Fiber> Fiber::GetThis()
    {
        if (t_fiber)
        {
            return t_fiber->shared_from_this();
        }

        std::shared_ptr<Fiber> main_fiber(new Fiber());
        t_thread_fiber = main_fiber;
        t_scheduler_fiber = main_fiber.get();

        assert(main_fiber.get() == t_fiber);
        return t_fiber->shared_from_this();
    }

    uint64_t Fiber::GetFiberId()
    {
        if (t_fiber)
        {
            return t_fiber->getId();
        }
        else
            return (uint64_t)-1;
    }

    void Fiber::reset(std::function<void()> cb)
    {
        assert(m_stack != nullptr && m_state == TERM);

        m_cb = cb;
        m_state = READY;

        if (getcontext(&m_ctx))
        {
            std::cerr << "Fiber() failed\n";
            pthread_exit(NULL);
        }

        m_ctx.uc_link = nullptr;
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;
        makecontext(&m_ctx, &Fiber::MainFunc, 0);
    }

    void Fiber::resume()
    {
        assert(m_state == READY);

        SetThis(this);
        m_state = RUNNING;
        
        if (m_runInScheduler)
        {
            if (swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
            {
                std::cerr << "resume() to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
            }
        }
        else
        {
            if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
            {
                std::cerr << "resume() to t_thread_fiber failed\n";
                pthread_exit(NULL);
            }
        }
        // SetThis(this);
    }

    void Fiber::yield()
    {
        assert(m_state == RUNNING || m_state == TERM);

        if (m_state == RUNNING)
            m_state = READY;

        if (m_runInScheduler)
        {
            SetThis(t_scheduler_fiber);
            if (swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
            {
                std::cerr << "yield() to to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
            }
        }
        else
        {
            SetThis(t_thread_fiber.get());
            if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
            {
                std::cerr << "yield() to to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
            }
        }
    }

    void Fiber::MainFunc()
    {
        std::shared_ptr<Fiber> curr = GetThis();
        assert(curr != nullptr && curr->m_cb!=nullptr);

        curr->m_cb();
        curr->m_cb = nullptr;
        curr->m_state = TERM;

        auto raw_ptr = curr.get();
        curr.reset();
        raw_ptr->yield();
    }

}
#include "ioscheduler.h"
#include "sys/epoll.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

static bool debug = true;

namespace sylar
{
    IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(Event event)
    {
        assert(event == READ || event == WRITE);
        switch (event)
        {
        case (READ):
            return read;
        case (WRITE):
            return write;
        }
        throw std::invalid_argument("Unsupported event type");
    }

    void IOManager::FdContext::resetEventContext(EventContext &ctx)
    {
        ctx.cb = nullptr;
        ctx.fiber.reset();
        ctx.scheduler = nullptr;
    }

    void IOManager::FdContext::triggerEvent(Event event)
    {
        assert(events & event);

        events = (Event)(events & ~event);

        EventContext &ctx = getEventContext(event);
        if (ctx.cb)
        {
            // ctx.cb();不能直接触发
            ctx.scheduler->scheduleLock(&ctx.cb);
        }
        else
        {
            // Fiber::GetThis();
            // ctx.fiber->resume();
            ctx.scheduler->scheduleLock(&ctx.fiber);
        }
        resetEventContext(ctx);
    }

    IOManager *IOManager::GetThis()
    {
        return dynamic_cast<IOManager *>(Scheduler::GetThis());
    }

    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
        : Scheduler(threads, use_caller, name), TimerManager()
    {
        m_epfd = epoll_create(5000);
        assert(m_epfd > 0);

        int rt = pipe(m_tickleFds);
        assert(!rt);

        epoll_event ep;
        ep.events = EPOLLIN | EPOLLET;
        ep.data.fd = m_tickleFds[0];

        rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
        assert(!rt);

        rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &ep);
        assert(!rt);

        contextResize(32);

        start();
    }

    IOManager::~IOManager()
    {
        // 挂载了scheduler，需要手动关闭
        stop();
        close(m_epfd);
        close(m_tickleFds[0]);
        close(m_tickleFds[1]);
        for (auto &f_ctx : m_fdContexts)
        {
            if (f_ctx)
                delete f_ctx;
        }
    }

    void IOManager::contextResize(size_t size)
    {
        m_fdContexts.resize(size);
        for (size_t i = 0; i < m_fdContexts.size(); ++i)
        {
            if (m_fdContexts[i] == nullptr)
            {
                m_fdContexts[i] = new FdContext();
                m_fdContexts[i]->fd = i;
            }
        }
    }

    // 1.查找对应的fdContext
    // 2.将event添加到epoll
    // 3.修改fdContext的内容
    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        // 1.查找对应的fdContext
        FdContext *fd_ctx = nullptr;
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if (fd < (int)m_fdContexts.size())
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(m_mutex);
            contextResize(m_fdContexts.size() * 1.5);
            fd_ctx = m_fdContexts[fd];
        }

        // 2.添加event到epoll
        std::lock_guard<std::mutex> locker(fd_ctx->mutex);

        // 已经包含了
        if (fd_ctx->events & event)
        {
            return -1;
        }
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event ep;
        ep.events = fd_ctx->events | event | EPOLLET;
        ep.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &ep);
        if (rt)
        {
            std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }
        m_pendingEventCount++;

        // 3.更新fdContext
        fd_ctx->events = (Event)(fd_ctx->events | event);

        FdContext::EventContext &ctx = fd_ctx->getEventContext(event);
        assert(!ctx.scheduler && !ctx.fiber && !ctx.cb);

        ctx.scheduler = Scheduler::GetThis();
        if (cb)
        {
            ctx.cb.swap(cb);
        }
        else
        {
            ctx.fiber = Fiber::GetThis();
            assert(ctx.fiber->getState() == Fiber::RUNNING);
        }
        return 0;
    }

    bool IOManager::delEvent(int fd, Event event)
    {
        FdContext *fd_ctx = nullptr;
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if (fd < (int)m_fdContexts.size())
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> locker(fd_ctx->mutex);

        // 原本没有，不用删
        if (!(fd_ctx->events & event))
        {
            return false;
        }

        Event new_event = (Event)(fd_ctx->events & ~event);
        int op = new_event ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event ep;
        ep.events = EPOLLET | new_event;
        ep.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &ep);
        if (rt)
        {
            std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        m_pendingEventCount--;
        fd_ctx->events = new_event;

        // FdContext::EventContext &ctx = fd_ctx->getEventContext(fd_ctx->events);
        FdContext::EventContext &ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(ctx);
        return true;
    }

    bool IOManager::cancelEvent(int fd, Event event)
    {
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if ((int)m_fdContexts.size() > fd)
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> locker(fd_ctx->mutex);

        if (!(fd_ctx->events & event))
        {
            return false;
        }

        Event new_event = (Event)(fd_ctx->events & ~event);
        int op = new_event ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event ep;
        ep.events = new_event | EPOLLET;
        ep.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &ep);
        if (rt)
        {
            std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --m_pendingEventCount;

        FdContext::EventContext &ctx = fd_ctx->getEventContext(event);
        fd_ctx->triggerEvent(event);

        return true;
    }

    bool IOManager::cancelAll(int fd)
    {
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        if ((int)m_fdContexts.size() > fd)
        {
            fd_ctx = m_fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> locker(fd_ctx->mutex);

        if (!fd_ctx->events)
        {
            return false;
        }

        epoll_event ep;
        ep.events = fd_ctx->events;
        ep.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, &ep);
        if (rt)
        {
            std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        if (fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            m_pendingEventCount--;
        }
        if (fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            m_pendingEventCount--;
        }

        assert(fd_ctx->events == 0);
        return true;
    }

    void IOManager::tickle()
    {
        if (!hasIdleThreads())
        {
            int rt = write(m_tickleFds[1], "T", 1);
            assert(rt == 1);
        }
    }

    // 1.epoll_wait
    // 2.处理超时定时器
    // 3.处理event任务
    void IOManager::idle()
    {
        static const uint64_t MAX_EVENTS = 256;
        std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

        while (true)
        {
            if (debug)
                std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl;

            if (stopping())
            {
                if (debug)
                    std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
                break;
            }

            int rt = 0;
            // blocked at epoll_wait
            while (true)
            {
                static const uint64_t MAX_TIMOUT = 5000;

                uint64_t timeout = TimerManager::getNextTimer();
                timeout = std::min(timeout, MAX_TIMOUT);

                rt = epoll_wait(m_epfd, events.get(), MAX_EVENTS, (int)timeout);
                // EINTR -> retry
                if (rt < 0 && errno == EINTR)
                {
                    continue;
                }
                else
                {
                    break;
                }
            }

            // 2.处理超时任务
            std::vector<std::function<void()>> cbs;
            listExpiredCb(cbs);
            if (!cbs.empty())
            {
                for (const auto &cb : cbs)
                {
                    Scheduler::scheduleLock(cb);
                }
                cbs.clear();
            }
            // 3.处理event
            for (int i = 0; i < rt; i++)
            {
                epoll_event event = events[i];

                if (event.data.fd == m_tickleFds[0])
                {
                    uint8_t dummy[256];
                    // edge triggered -> exhaust
                    while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0)
                    {
                    }
                    continue;
                }

                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                std::lock_guard<std::mutex> lock(fd_ctx->mutex);
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    // event.events |= (EPOLLIN | EPOLLOUT);
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }

                int real_events = NONE;
                if (event.events & EPOLLIN)
                {
                    real_events |= READ;
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= WRITE;
                }

                if ((fd_ctx->events & real_events) == NONE)
                {
                    continue;
                }

                epoll_event ep;
                int left_events = (fd_ctx->events & ~real_events);
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = left_events | EPOLLET;

                int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
                if (rt2)
                {
                    std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl;
                    continue;
                }

                if (real_events & READ)
                {
                    fd_ctx->triggerEvent(READ);
                    m_pendingEventCount--;
                }
                if (real_events & WRITE)
                {
                    fd_ctx->triggerEvent(WRITE);
                    m_pendingEventCount--;
                }
            }
        }
        Fiber::GetThis()->yield();
    }

    bool IOManager::stopping()
    {
        uint64_t timeout = TimerManager::getNextTimer();
        return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
    }

    void IOManager::onTimerInsertedAtFront()
    {
        tickle();
    }
}
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    namespace
    {
        u32 DeriveWorkerCount(u32 requested)
        {
            if (requested != 0)
            {
                return requested;
            }

            // Leave the main thread a core; always at least one worker.
            const u32 hardware = std::thread::hardware_concurrency();
            return hardware > 1 ? hardware - 1 : 1;
        }
    }

    TaskSystem::TaskSystem(const TaskSystemInfo& info) :
        m_WorkerCount(DeriveWorkerCount(info.WorkerCount))
    {
        m_Workers.reserve(m_WorkerCount);
        for (u32 i = 0; i < m_WorkerCount; ++i)
        {
            m_Workers.emplace_back([this, i] { WorkerLoop(i); });
        }
    }

    TaskSystem::~TaskSystem()
    {
        {
            std::lock_guard lock(m_QueueMutex);
            m_Stopping = true;
        }
        m_WorkAvailable.notify_all();

        for (std::thread& worker : m_Workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    void TaskSystem::WorkerLoop(u32)
    {
        while (true)
        {
            function<void()> job;
            {
                std::unique_lock lock(m_QueueMutex);
                m_WorkAvailable.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });

                if (m_Stopping && m_Queue.empty())
                {
                    return;
                }

                job = std::move(m_Queue.front());
                m_Queue.pop_front();
            }

            job();
        }
    }

    void TaskSystem::ForEachWorker(const function<void(u32 workerIndex)>& fn)
    {
        // One job per worker, each pinned to a distinct worker by a barrier: a
        // worker that grabs a slot blocks until every slot is grabbed, so no
        // worker runs the body twice and every worker runs it once.
        std::mutex barrierMutex;
        std::condition_variable barrierReady;
        u32 arrived = 0;
        u32 nextIndex = 0;
        const u32 workers = m_WorkerCount;

        vector<Task<void>> tasks;
        tasks.reserve(workers);
        for (u32 i = 0; i < workers; ++i)
        {
            tasks.push_back(Submit([&] {
                u32 index;
                {
                    std::unique_lock lock(barrierMutex);
                    index = nextIndex++;
                    ++arrived;
                    barrierReady.notify_all();
                    barrierReady.wait(lock, [&] { return arrived == workers; });
                }
                fn(index);
            }));
        }

        for (Task<void>& task : tasks)
        {
            (void)task.Get();
        }
    }

    void TaskSystem::EnqueueMainThread(function<void()> fn)
    {
        std::lock_guard lock(m_MainThreadMutex);
        m_MainThreadQueue.push_back(std::move(fn));
    }

    void TaskSystem::PumpMainThread()
    {
        // Drain a snapshot so continuations enqueued by other continuations run
        // on the next pump, not this one (bounded work per frame).
        std::deque<function<void()>> pending;
        {
            std::lock_guard lock(m_MainThreadMutex);
            pending.swap(m_MainThreadQueue);
        }

        for (function<void()>& fn : pending)
        {
            fn();
        }
    }

    void TaskSystem::WaitForAll()
    {
        std::unique_lock lock(m_QueueMutex);
        m_WorkDrained.wait(lock, [this] { return m_Queue.empty() && m_ActiveJobs == 0; });
    }
}

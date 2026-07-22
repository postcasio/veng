#include <Veng/Task/TaskSystem.h>

#include <string>

namespace Veng
{
    namespace
    {
        // The worker index of the calling thread, set by WorkerLoop for the
        // lifetime of the thread. Sentinel on the main thread (and any non-worker
        // thread): GetCurrentWorkerIndex asserts it is queried only from a worker.
        thread_local u32 t_CurrentWorkerIndex = TaskSystem::NotAWorker;

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

    TaskSystem::TaskSystem(const TaskSystemInfo& info)
        : m_WorkerCount(DeriveWorkerCount(info.WorkerCount))
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
            const std::scoped_lock lock(m_QueueMutex);
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

    u32 TaskSystem::GetCurrentWorkerIndex()
    {
        VE_ASSERT(t_CurrentWorkerIndex != NotAWorker,
                  "GetCurrentWorkerIndex called off a worker thread — only a job running on a "
                  "TaskSystem worker has a worker index");
        return t_CurrentWorkerIndex;
    }

    u32 TaskSystem::TryGetCurrentWorkerIndex()
    {
        return t_CurrentWorkerIndex;
    }

    void TaskSystem::Enqueue(function<void()> run, string_view name)
    {
        QueuedJob job;
        job.Run = std::move(run);
#if defined(VE_PROFILE) && VE_PROFILE
        job.Name = string(name);
        // Read the clock at submit only while a capture is running; an idle profiler must not put a
        // timestamp on every enqueue in the engine.
        if (Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler();
            profiler != nullptr && profiler->IsRecording())
        {
            job.SubmitTicks = Diagnostics::NowTicks();
        }
#else
        (void)name;
#endif

        {
            const std::scoped_lock lock(m_QueueMutex);
            m_ActiveJobs.fetch_add(1, std::memory_order_relaxed);
            m_QueueDepth.fetch_add(1, std::memory_order_relaxed);
            m_Queue.push_back(std::move(job));
        }
        m_WorkAvailable.notify_one();
    }

    void TaskSystem::RunJob(QueuedJob& job)
    {
#if defined(VE_PROFILE) && VE_PROFILE
        // Job latency (submit → start): one subtraction here, paired with the one clock read at
        // submit, both only while recording. Zero SubmitTicks means the submit was not recorded.
        if (job.SubmitTicks != 0)
        {
            const u64 latencyTicks = Diagnostics::NowTicks() - job.SubmitTicks;
            VE_PROFILE_COUNTER("TaskSystem/JobLatencyNs",
                               static_cast<f64>(Diagnostics::TraceTicksToNanos(latencyTicks)));
        }
        if (!job.Name.empty())
        {
            VE_PROFILE_SCOPE_DYNAMIC(job.Name);
            job.Run();
            return;
        }
        VE_PROFILE_SCOPE("TaskSystem/Job");
#endif
        job.Run();
    }

    void TaskSystem::WorkerLoop(u32 workerIndex)
    {
        t_CurrentWorkerIndex = workerIndex;

#if defined(VE_PROFILE) && VE_PROFILE
        // Name this worker's track when a profiler is installed; a worker started before the
        // profiler attaches lazily instead, and its jobs still land on a per-worker track.
        Diagnostics::ProfilerThreadRegistration registration;
        if (Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler())
        {
            registration =
                profiler->RegisterThread(string("Worker ") + std::to_string(workerIndex));
        }
#endif

        while (true)
        {
            QueuedJob job;
            {
                std::unique_lock lock(m_QueueMutex);
                m_WorkAvailable.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });

                if (m_Stopping && m_Queue.empty())
                {
                    return;
                }

                job = std::move(m_Queue.front());
                m_Queue.pop_front();
                m_QueueDepth.fetch_sub(1, std::memory_order_relaxed);
            }

            RunJob(job);
        }
    }

    void TaskSystem::ForEachWorker(const function<void(u32 workerIndex)>& fn)
    {
        // A barrier pins each job to a distinct worker: each job blocks until all
        // workers have grabbed a slot, so every worker runs fn exactly once.
        std::mutex barrierMutex;
        std::condition_variable barrierReady;
        u32 arrived = 0;
        u32 nextIndex = 0;
        const u32 workers = m_WorkerCount;

        vector<Task<void>> tasks;
        tasks.reserve(workers);
        for (u32 i = 0; i < workers; ++i)
        {
            tasks.push_back(Submit(
                [&]
                {
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
        const std::scoped_lock lock(m_MainThreadMutex);
        m_MainThreadQueue.push_back(std::move(fn));
        m_MainThreadDepth.fetch_add(1, std::memory_order_relaxed);
    }

    void TaskSystem::PumpMainThread()
    {
        VE_PROFILE_SCOPE("TaskSystem/PumpMainThread");

        // Drain a snapshot so continuations enqueued by other continuations run
        // on the next pump, not this one (bounded work per frame).
        std::deque<function<void()>> pending;
        {
            const std::scoped_lock lock(m_MainThreadMutex);
            pending.swap(m_MainThreadQueue);
            m_MainThreadDepth.fetch_sub(static_cast<u32>(pending.size()),
                                        std::memory_order_relaxed);
        }

        for (const function<void()>& fn : pending)
        {
            fn();
        }
    }

    void TaskSystem::WaitForAll()
    {
        std::unique_lock lock(m_QueueMutex);
        m_WorkDrained.wait(
            lock, [this]
            { return m_Queue.empty() && m_ActiveJobs.load(std::memory_order_relaxed) == 0; });
    }
}

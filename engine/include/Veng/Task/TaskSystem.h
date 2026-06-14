#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Assert.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace Veng
{
    // The result a worker hands back. A void job's payload is std::monostate, so
    // a single template covers both value and void jobs.
    template <typename T>
    using TaskPayload = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    class TaskSystem;

    namespace Detail
    {
        // The shared state behind a Task<T>: the slot the worker writes its
        // Result into, the readiness flag, and the registered main-thread
        // continuation. The Task<T> handle and the worker both reference it.
        template <typename T>
        struct TaskState
        {
            using Payload = TaskPayload<T>;

            std::mutex Mutex;
            std::condition_variable Ready;
            bool Done = false;
            optional<Result<Payload>> Value;

            // A Then continuation registered before the result landed. Stored
            // here until the worker finishes, then handed to the main-thread
            // queue so it runs on the pumping thread, never on a worker.
            function<void(Result<Payload>)> Continuation;
        };

        // True when a job's return type is already a Result<U>, so a job can
        // report a recoverable failure rather than only a value.
        template <typename R>
        struct IsResult : std::false_type
        {
        };

        template <typename U>
        struct IsResult<std::expected<U, std::string>> : std::true_type
        {
        };
    }

    template <typename T>
    class Task
    {
    public:
        using Payload = TaskPayload<T>;

        Task() = default;

        // Non-blocking: has the worker written the result yet?
        [[nodiscard]] bool IsReady() const
        {
            if (!m_State)
            {
                return false;
            }

            std::lock_guard lock(m_State->Mutex);
            return m_State->Done;
        }

        // Blocks until the result lands, then moves it out. A failed job
        // surfaces its error here as Result<T>::error() — no exceptions.
        [[nodiscard]] Result<Payload> Get()
        {
            VE_ASSERT(m_State, "Get() on an empty Task");

            std::unique_lock lock(m_State->Mutex);
            m_State->Ready.wait(lock, [this] { return m_State->Done; });
            return std::move(*m_State->Value);
        }

        // Register a continuation that runs on the main thread during
        // TaskSystem::PumpMainThread(), never on a worker. If the result has
        // already landed the continuation is queued onto the pump immediately;
        // otherwise the worker queues it on completion.
        void Then(function<void(Result<Payload>)> fn);

    private:
        friend class TaskSystem;

        Task(TaskSystem& system, Ref<Detail::TaskState<T>> state) :
            m_System(&system),
            m_State(std::move(state))
        {
        }

        TaskSystem* m_System = nullptr;
        Ref<Detail::TaskState<T>> m_State;
    };

    struct TaskSystemInfo
    {
        // 0 = derive from hardware_concurrency() - 1 (leave the main thread a core).
        u32 WorkerCount = 0;
    };

    // A fixed pool of worker threads draining a shared work queue, returning
    // Task<T> result handles. Owned by Application and threaded by reference into
    // consumers — there is no global instance. Submit runs a callable on a
    // worker; PumpMainThread drains continuations on the calling thread.
    class TaskSystem
    {
    public:
        // Sentinel worker index for any thread that is not a TaskSystem worker
        // (the main thread, etc.). GetCurrentWorkerIndex never returns this — it
        // asserts instead.
        static constexpr u32 NotAWorker = static_cast<u32>(-1);

        explicit TaskSystem(const TaskSystemInfo& info = {});
        ~TaskSystem();

        TaskSystem(const TaskSystem&) = delete;
        TaskSystem& operator=(const TaskSystem&) = delete;

        // The index of the worker the calling thread is. Valid only inside a job
        // running on a worker (a Submit-ed callable or a ForEachWorker body);
        // asserts off a worker. The async upload path uses it to select that
        // worker's own transfer command pool (GetTransferCommandBuffer).
        [[nodiscard]] static u32 GetCurrentWorkerIndex();

        // Submit a callable returning T (or void). The callable runs on a worker;
        // the returned Task<T> carries its Result back. A callable that returns
        // Result<T> is unwrapped, so a job may report a recoverable failure.
        template <typename Fn>
        auto Submit(Fn&& fn);

        // Run fn on each worker once, passing the worker's index, for per-thread
        // setup. Blocks until every worker has run it.
        void ForEachWorker(const function<void(u32 workerIndex)>& fn);

        // Drain the main-thread continuation queue, running each Then on the
        // calling thread. Application calls this at the top of Frame(), before
        // BeginFrame().
        void PumpMainThread();

        // Block until the work queue is empty and every in-flight job has
        // finished. Pending continuations are left for the next PumpMainThread.
        void WaitForAll();

        [[nodiscard]] u32 GetWorkerCount() const { return m_WorkerCount; }

    private:
        template <typename T>
        friend class Task;

        // Push a ready continuation onto the main-thread queue.
        void EnqueueMainThread(function<void()> fn);

        // Called on a worker when a job finishes: store the Result, wake any
        // blocked Get(), hand a registered Then to the main-thread queue, and
        // tick the active-job count so WaitForAll can observe completion.
        template <typename T>
        void Finish(Detail::TaskState<T>& state, Result<typename Detail::TaskState<T>::Payload> result)
        {
            function<void()> continuation;
            {
                std::lock_guard lock(state.Mutex);
                state.Value = std::move(result);
                state.Done = true;

                if (state.Continuation)
                {
                    continuation = [fn = std::move(state.Continuation), value = *state.Value]() mutable {
                        fn(std::move(value));
                    };
                    state.Continuation = nullptr;
                }
            }
            state.Ready.notify_all();

            if (continuation)
            {
                EnqueueMainThread(std::move(continuation));
            }

            {
                std::lock_guard lock(m_QueueMutex);
                --m_ActiveJobs;
            }
            m_WorkDrained.notify_all();
        }

        void WorkerLoop(u32 workerIndex);

        u32 m_WorkerCount = 0;
        vector<std::thread> m_Workers;

        std::mutex m_QueueMutex;
        std::condition_variable m_WorkAvailable;
        std::condition_variable m_WorkDrained;
        std::deque<function<void()>> m_Queue;
        u32 m_ActiveJobs = 0;
        bool m_Stopping = false;

        std::mutex m_MainThreadMutex;
        std::deque<function<void()>> m_MainThreadQueue;
    };

    // --- template implementations ------------------------------------------

    template <typename T>
    void Task<T>::Then(function<void(Result<Payload>)> fn)
    {
        VE_ASSERT(m_State, "Then() on an empty Task");
        VE_ASSERT(m_System, "Then() on an empty Task");

        Ref<Detail::TaskState<T>> state = m_State;

        std::unique_lock lock(state->Mutex);
        if (state->Done)
        {
            // Result already landed: hand a ready copy straight to the pump.
            Result<Payload> value = *state->Value;
            lock.unlock();
            m_System->EnqueueMainThread(
                [fn = std::move(fn), value = std::move(value)]() mutable { fn(std::move(value)); });
            return;
        }

        // The worker enqueues this onto the pump when it finishes.
        state->Continuation = std::move(fn);
    }

    template <typename Fn>
    auto TaskSystem::Submit(Fn&& fn)
    {
        using Returned = std::invoke_result_t<Fn>;

        // A job may report a recoverable failure by returning Result<U>; in that
        // case the Task's payload is U. Otherwise the payload is the plain return
        // type (void collapsing to std::monostate).
        if constexpr (Detail::IsResult<Returned>::value)
        {
            using Payload = typename Returned::value_type;
            using State = Detail::TaskState<Payload>;

            auto state = CreateRef<State>();
            {
                std::lock_guard lock(m_QueueMutex);
                ++m_ActiveJobs;
                m_Queue.emplace_back([this, state, fn = std::forward<Fn>(fn)]() mutable {
                    Result<Payload> result = fn();
                    Finish(*state, std::move(result));
                });
            }
            m_WorkAvailable.notify_one();
            return Task<Payload>(*this, std::move(state));
        }
        else if constexpr (std::is_void_v<Returned>)
        {
            using State = Detail::TaskState<void>;

            auto state = CreateRef<State>();
            {
                std::lock_guard lock(m_QueueMutex);
                ++m_ActiveJobs;
                m_Queue.emplace_back([this, state, fn = std::forward<Fn>(fn)]() mutable {
                    fn();
                    Finish(*state, Result<std::monostate>(std::monostate{}));
                });
            }
            m_WorkAvailable.notify_one();
            return Task<void>(*this, std::move(state));
        }
        else
        {
            using State = Detail::TaskState<Returned>;

            auto state = CreateRef<State>();
            {
                std::lock_guard lock(m_QueueMutex);
                ++m_ActiveJobs;
                m_Queue.emplace_back([this, state, fn = std::forward<Fn>(fn)]() mutable {
                    Result<Returned> result = fn();
                    Finish(*state, std::move(result));
                });
            }
            m_WorkAvailable.notify_one();
            return Task<Returned>(*this, std::move(state));
        }
    }
}

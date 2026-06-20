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
    /// @brief Payload type for a job returning T; void jobs collapse to std::monostate.
    template <typename T>
    using TaskPayload = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    class TaskSystem;

    namespace Detail
    {
        /// @brief Shared state between a Task<T> handle and the worker that runs it.
        ///
        /// Holds the Result slot, the readiness flag, and the registered main-thread
        /// continuation. Both sides reference it through a Ref<TaskState<T>>.
        template <typename T>
        struct TaskState
        {
            /// @brief Resolved payload type for T.
            using Payload = TaskPayload<T>;

            /// @brief Guards Done, Value, and Continuation.
            std::mutex Mutex;
            /// @brief Signalled when Done becomes true.
            std::condition_variable Ready;
            /// @brief True once the worker has written Value.
            bool Done = false;
            /// @brief The job result, valid when Done.
            optional<Result<Payload>> Value;
            /// @brief Continuation registered before the result landed.
            ///
            /// Stored here until the worker finishes, then handed to the main-thread
            /// queue so it runs during PumpMainThread(), never on a worker.
            function<void(Result<Payload>)> Continuation;
        };

        /// @brief Trait: true when R is a Result<U>, allowing a job to report recoverable failures.
        template <typename R>
        struct IsResult : std::false_type
        {
        };

        /// @brief Specialization for std::expected<U, std::string>.
        template <typename U>
        struct IsResult<std::expected<U, std::string>> : std::true_type
        {
        };
    }

    /// @brief Handle to an async job submitted to the TaskSystem.
    ///
    /// Provides IsReady() for non-blocking polling, Get() for blocking retrieval,
    /// and Then() for main-thread continuations. A default-constructed Task is empty.
    /// @tparam T  Return type of the job (void is allowed).
    template <typename T>
    class Task
    {
    public:
        /// @brief Resolved payload type (void → std::monostate).
        using Payload = TaskPayload<T>;

        Task() = default;

        /// @brief Non-blocking check: returns true if the worker has written the result.
        [[nodiscard]] bool IsReady() const
        {
            if (!m_State)
            {
                return false;
            }

            std::lock_guard lock(m_State->Mutex);
            return m_State->Done;
        }

        /// @brief Blocks until the result is ready, then moves it out.
        ///
        /// A failed job surfaces its error as Result<Payload>::error() — no exceptions.
        [[nodiscard]] Result<Payload> Get()
        {
            VE_ASSERT(m_State, "Get() on an empty Task");

            std::unique_lock lock(m_State->Mutex);
            m_State->Ready.wait(lock, [this] { return m_State->Done; });
            return std::move(*m_State->Value);
        }

        /// @brief Registers a continuation that runs on the main thread during PumpMainThread().
        ///
        /// If the result has already landed, the continuation is queued immediately;
        /// otherwise the worker queues it on completion. Never runs on a worker thread.
        /// @param fn  Callable accepting Result<Payload>.
        void Then(function<void(Result<Payload>)> fn);

    private:
        friend class TaskSystem;

        Task(TaskSystem& system, Ref<Detail::TaskState<T>> state) :
            m_System(&system),
            m_State(std::move(state))
        {
        }

        /// @brief The owning TaskSystem; null on empty Tasks.
        TaskSystem* m_System = nullptr;
        /// @brief Shared state with the worker.
        Ref<Detail::TaskState<T>> m_State;
    };

    /// @brief Construction parameters for TaskSystem.
    struct TaskSystemInfo
    {
        /// @brief Number of worker threads; 0 derives from hardware_concurrency() - 1.
        u32 WorkerCount = 0;
    };

    /// @brief Fixed pool of worker threads draining a shared work queue.
    ///
    /// Owned by Application and threaded by reference into consumers; there is no global
    /// instance. Submit() runs a callable on a worker; PumpMainThread() drains continuations
    /// on the calling thread.
    class TaskSystem
    {
    public:
        /// @brief Sentinel returned to callers; GetCurrentWorkerIndex() asserts rather than returning it.
        static constexpr u32 NotAWorker = static_cast<u32>(-1);

        /// @brief Constructs the pool and starts the worker threads.
        explicit TaskSystem(const TaskSystemInfo& info = {});

        /// @brief Stops all workers and joins them.
        ~TaskSystem();

        TaskSystem(const TaskSystem&) = delete;
        TaskSystem& operator=(const TaskSystem&) = delete;

        /// @brief Returns the index of the worker thread calling this function.
        ///
        /// Valid only inside a Submit()ed callable or ForEachWorker() body; asserts on
        /// any other thread. The async upload path uses it to select that worker's
        /// transfer command pool.
        [[nodiscard]] static u32 GetCurrentWorkerIndex();

        /// @brief Submits a callable to run on a worker and returns a Task<T> handle.
        ///
        /// A callable returning Result<T> is unwrapped so the job can report a
        /// recoverable failure. Void return is also supported.
        /// @param fn  Callable with no arguments returning T, void, or Result<T>.
        template <typename Fn>
        auto Submit(Fn&& fn);

        /// @brief Runs fn on each worker once with its index; blocks until all have finished.
        ///
        /// Used for per-worker setup (e.g. allocating per-thread command pools).
        /// @param fn  Callable accepting a worker index.
        void ForEachWorker(const function<void(u32 workerIndex)>& fn);

        /// @brief Drains the main-thread continuation queue on the calling thread.
        ///
        /// Application calls this at the top of Frame(), before BeginFrame(), so
        /// off-thread continuations land on the main thread.
        void PumpMainThread();

        /// @brief Blocks until the work queue is empty and every in-flight job has finished.
        ///
        /// Pending continuations are left for the next PumpMainThread() call.
        void WaitForAll();

        /// @brief Returns the number of worker threads in the pool.
        [[nodiscard]] u32 GetWorkerCount() const { return m_WorkerCount; }

    private:
        template <typename T>
        friend class Task;

        /// @brief Pushes a ready continuation onto the main-thread queue.
        void EnqueueMainThread(function<void()> fn);

        /// @brief Called by a worker on job completion: stores the result, wakes Get(), and hands off any Then.
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

        /// @brief Entry function for each worker thread.
        void WorkerLoop(u32 workerIndex);

        /// @brief Number of worker threads.
        u32 m_WorkerCount = 0;
        /// @brief Worker thread handles.
        vector<std::thread> m_Workers;

        /// @brief Guards m_Queue, m_ActiveJobs, m_Stopping.
        std::mutex m_QueueMutex;
        /// @brief Signalled when new work is enqueued.
        std::condition_variable m_WorkAvailable;
        /// @brief Signalled when m_ActiveJobs reaches zero.
        std::condition_variable m_WorkDrained;
        /// @brief Pending job closures.
        std::deque<function<void()>> m_Queue;
        /// @brief Count of in-flight + queued jobs.
        u32 m_ActiveJobs = 0;
        /// @brief Set to true to drain and shut down workers.
        bool m_Stopping = false;

        /// @brief Guards m_MainThreadQueue.
        std::mutex m_MainThreadMutex;
        /// @brief Ready continuations for PumpMainThread().
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

        // A Result<U>-returning job's payload is U; a void job's is std::monostate.
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

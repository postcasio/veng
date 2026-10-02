#pragma once

#include <Veng/Assert.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Tuning for the fixed-timestep simulation accumulator.
    ///
    /// A SimClock steps the Sim phase at TickRate Hz regardless of the frame rate, carrying the
    /// residual frame time forward so the average tick rate matches the wall clock. These are its
    /// only construction inputs; both must be positive.
    struct SimClockInfo
    {
        /// @brief Fixed simulation ticks per second (the step is 1 / TickRate seconds).
        u32 TickRate = 60;
        /// @brief Maximum Sim ticks advanced in one frame, clamping the spiral of death.
        ///
        /// When a frame's accumulated backlog would run more than this many steps, the surplus is
        /// dropped (the accumulator is cleared) rather than chased — a long stall resyncs to the
        /// present instead of running an unbounded catch-up burst.
        u32 MaxTicksPerFrame = 5;
        /// @brief Wall-clock budget for one frame's Sim steps, in milliseconds; unset runs every step
        /// the clamp allows.
        ///
        /// Read only by Run. Once the frame's steps have spent the budget, Run ends the frame and
        /// drops the remaining backlog as a clamp, so a step that costs more than the time it
        /// simulates degrades into time dilation at a bounded frame time instead of a frame time of
        /// MaxTicksPerFrame steps. Must be positive when set.
        optional<f32> MaxSimMillisecondsPerFrame;
    };

    /// @brief One fixed step SimClock::Run hands its step callback, with where it falls in the frame.
    ///
    /// Last and RecordsHistory are decided before the step runs and are binding: a step marked Last
    /// is the frame's final step, and the frame's final two steps are always marked RecordsHistory
    /// (the final step of the previous frame standing in when the frame runs one step).
    struct SimStepInfo
    {
        /// @brief The tick this step advances (FirstTick + Index).
        u64 Tick = 0;
        /// @brief This step's position in the frame's steps, from 0.
        u32 Index = 0;
        /// @brief The fixed step duration in seconds (1 / TickRate).
        f32 Delta = 0.0f;
        /// @brief True on the frame's first step.
        bool First = false;
        /// @brief True on the frame's final step; the frame runs no further step after it.
        bool Last = false;
        /// @brief True when this step's pose may be one of the frame's final two, which interpolation
        /// reads, so the caller records transform history after it.
        ///
        /// Always true on the final two steps. Under a budget it is also set on a step the budget
        /// may make the penultimate one; a step marked so that turns out not to be is a wasted
        /// record, never a missing one.
        bool RecordsHistory = false;
    };

    /// @brief The Sim ticks a single frame runs, plus the frame's interpolation state.
    ///
    /// Returned by SimClock::Advance and SimClock::Run. Steps is 0 when the frame did not accumulate
    /// a full step (frame rate above the tick rate) and up to MaxTicksPerFrame when catching up, or
    /// fewer when the wall-clock budget ended the frame. The ticks the
    /// frame runs are FirstTick .. FirstTick + Steps - 1 (empty when Steps == 0).
    struct SimStep
    {
        /// @brief Number of fixed Sim ticks this frame runs (0 .. MaxTicksPerFrame).
        u32 Steps = 0;
        /// @brief The first tick number this frame runs; the last completed tick + 1.
        u64 FirstTick = 0;
        /// @brief The fixed step duration in seconds (1 / TickRate).
        f32 SimDelta = 0.0f;
        /// @brief Residual fraction into the next tick after this frame's steps, in [0, 1).
        ///
        /// The render/View interpolation alpha: the leftover accumulator over the step. Zero after a
        /// spiral-of-death clamp (the backlog was dropped).
        f32 Alpha = 0.0f;
        /// @brief True when this frame hit the spiral-of-death clamp and dropped backlog ticks.
        ///
        /// The tick advanced fewer ticks than wall-clock elapsed, so the clock now trails real time —
        /// a networked client uses this to detect it has fallen behind the server's tick and re-sync.
        /// Set by the step clamp and by the wall-clock budget alike.
        bool Clamped = false;
        /// @brief True when the wall-clock budget, not the step clamp, ended this frame.
        bool BudgetLimited = false;
        /// @brief Simulation time the clamp dropped this frame, in seconds; zero when unclamped.
        ///
        /// The time the simulation fell behind the wall clock this frame: its sum over frames is how
        /// far a dilated simulation trails real time.
        f32 DroppedSeconds = 0.0f;
    };

    /// @brief A monotonic fixed-timestep tick counter driven by an accumulator.
    ///
    /// Advance folds a frame delta into the accumulator, runs as many whole fixed steps as have
    /// accumulated (clamped against the spiral of death), advances the tick number by that many, and
    /// reports the residual as an interpolation alpha. The simulation reads a numbered, fixed-rate
    /// tick that two machines can agree on; the View/render side reads the alpha to interpolate
    /// between the last two ticks. Reset drops the accumulator without moving the tick — a pause
    /// leaves no tick debt to chase on resume. Pure and device-free: Run takes its wall clock as a
    /// parameter.
    class SimClock
    {
    public:
        /// @brief Constructs a clock with the default 60 Hz tick rate and clamp.
        SimClock() : SimClock(SimClockInfo{}) {}

        /// @brief Constructs the clock at tick 0 with an empty accumulator.
        /// @param info  The tick rate and spiral-of-death clamp.
        explicit SimClock(const SimClockInfo& info)
            : m_TickRate(info.TickRate), m_MaxTicksPerFrame(info.MaxTicksPerFrame),
              m_BudgetSeconds(info.MaxSimMillisecondsPerFrame.has_value()
                                  ? static_cast<f64>(*info.MaxSimMillisecondsPerFrame) / 1000.0
                                  : 0.0)
        {
            VE_ASSERT(m_TickRate > 0, "SimClock: TickRate must be positive");
            VE_ASSERT(m_MaxTicksPerFrame > 0, "SimClock: MaxTicksPerFrame must be positive");
            VE_ASSERT(!info.MaxSimMillisecondsPerFrame.has_value() ||
                          *info.MaxSimMillisecondsPerFrame > 0.0f,
                      "SimClock: MaxSimMillisecondsPerFrame must be positive when set");
        }

        /// @brief Accumulates a frame delta and advances the tick by every whole fixed step it completes.
        ///
        /// Adds @p frameDelta to the accumulator, subtracts one fixed step per tick (up to the clamp),
        /// and advances the tick number by the number of steps. A frame runs 0 steps (frame faster
        /// than the tick rate) to MaxTicksPerFrame (catching up); hitting the clamp drops the
        /// remaining backlog. The returned residual alpha interpolates into the next tick. The caller
        /// runs the steps itself afterwards, so the wall-clock budget does not apply here; a caller
        /// wanting it runs its steps through Run.
        /// @param frameDelta  The wall-clock frame delta in seconds (>= 0).
        /// @return This frame's step count, tick range, fixed delta, and interpolation alpha.
        SimStep Advance(const f32 frameDelta)
        {
            auto step = [](const SimStepInfo&) { return true; };
            auto now = [] { return 0.0; };
            return Step(frameDelta, step, now, false);
        }

        /// @brief Accumulates a frame delta and runs each whole fixed step it completes through @p step.
        ///
        /// The budgeted form of Advance. Each step is handed to @p step in tick order, with whether it
        /// is the frame's first or last and whether its pose must be recorded for interpolation.
        /// With MaxSimMillisecondsPerFrame set, the frame ends at the step whose completion is
        /// predicted to pass the budget, estimating a step's cost from the step before it, and the
        /// remaining backlog is dropped as a clamp. A step is made the last only when the step before
        /// it recorded history, so the frame may run one step past the budget to keep the final two
        /// poses recorded.
        /// @tparam StepFn  Callable as `bool(const SimStepInfo&)`; returns false to end the frame
        ///                 early without a clamp (the backlog is kept, not dropped).
        /// @tparam NowFn   Callable as `f64()`, returning a monotonic wall time in seconds.
        /// @param frameDelta  The wall-clock frame delta in seconds (>= 0).
        /// @param step        Runs one fixed step.
        /// @param now         The wall clock the budget is measured against.
        /// @return The steps run, tick range, fixed delta, interpolation alpha, and dropped time.
        template <class StepFn, class NowFn>
        SimStep Run(const f32 frameDelta, StepFn&& step, NowFn&& now)
        {
            return Step(frameDelta, step, now, m_BudgetSeconds > 0.0);
        }

        /// @brief Drops the accumulator without moving the tick, so a pause leaves no backlog.
        ///
        /// Called on a frame that runs no simulation (a full pause, no active scene): the tick stays
        /// put and the residual is cleared, so resuming does not chase the frames spent paused.
        void Reset() { m_Accumulator = 0.0f; }

        /// @brief Jumps the tick number to @p tick, clearing the accumulator.
        ///
        /// The seed a joining client applies once its first snapshot reveals the server's tick: the
        /// two processes' tick epochs are otherwise unrelated (each clock starts at 0 when its own
        /// process did), so a client that joined a long-running server would stamp input at tick
        /// numbers the server's scheduled input consume never matches. Seeding the client's tick to
        /// the server's (plus its run-ahead lead) aligns the epochs at once; the rate slew then only
        /// corrects the residual jitter. No effect on the tick rate or the fixed step.
        /// @param tick  The tick number to jump to.
        void SetTick(u64 tick)
        {
            m_Tick = tick;
            m_Accumulator = 0.0f;
        }

        /// @brief Returns the last completed tick number.
        [[nodiscard]] u64 GetTick() const { return m_Tick; }

    private:
        /// @brief The shared body of Advance and Run.
        /// @param frameDelta  The wall-clock frame delta in seconds (>= 0).
        /// @param step        Runs one fixed step; false ends the frame without a clamp.
        /// @param now         The wall clock, read only when @p budgeted.
        /// @param budgeted    Whether the wall-clock budget applies to this frame.
        template <class StepFn, class NowFn>
        SimStep Step(const f32 frameDelta, StepFn& step, NowFn& now, const bool budgeted)
        {
            m_Accumulator += frameDelta;

            const f32 simDelta = 1.0f / static_cast<f32>(m_TickRate);
            const u64 firstTick = m_Tick + 1;

            // The steps the accumulator owes, up to the clamp, counted with the same subtractions
            // the loop below makes so the two agree to the bit.
            u32 planned = 0;
            for (f32 owed = m_Accumulator; owed >= simDelta && planned < m_MaxTicksPerFrame;
                 owed -= simDelta)
            {
                ++planned;
            }

            const f64 start = budgeted ? now() : 0.0;
            f64 elapsed = 0.0;
            u32 steps = 0;
            bool stopped = false;
            // The previous frame's final step recorded history, so a frame's first step may be its
            // last without leaving the pose before it unrecorded.
            bool previousRecorded = true;
            while (steps < planned)
            {
                const u32 index = steps;
                // A budgeted frame ends at the step predicted to finish past the budget, and records
                // the step before that one; both are decided before the step runs so a system keyed
                // on the frame's last step and the history recording see a binding answer.
                const bool last = index + 1 == planned || (budgeted && previousRecorded &&
                                                           elapsed + m_StepCost >= m_BudgetSeconds);
                const bool records = last || index + 2 == planned ||
                                     (budgeted && elapsed + 2.0 * m_StepCost >= m_BudgetSeconds);

                const bool proceed = step(SimStepInfo{
                    .Tick = firstTick + index,
                    .Index = index,
                    .Delta = simDelta,
                    .First = index == 0,
                    .Last = last,
                    .RecordsHistory = records,
                });
                m_Accumulator -= simDelta;
                ++steps;
                previousRecorded = records;

                if (budgeted)
                {
                    const f64 spent = now() - start;
                    m_StepCost = spent - elapsed;
                    elapsed = spent;
                }
                if (!proceed)
                {
                    stopped = true;
                    break;
                }
                if (last)
                {
                    break;
                }
            }

            // Spiral-of-death clamp: a backlog the frame did not run — past the per-frame ceiling,
            // or cut by the budget — is dropped, not chased, so a long stall resyncs to the present
            // rather than running an unbounded catch-up burst. A caller ending the frame early keeps
            // its backlog.
            const bool clamped = !stopped && m_Accumulator >= simDelta;
            f32 dropped = 0.0f;
            if (clamped)
            {
                dropped = m_Accumulator;
                m_Accumulator = 0.0f;
            }

            m_Tick += steps;

            return SimStep{
                .Steps = steps,
                .FirstTick = firstTick,
                .SimDelta = simDelta,
                .Alpha = m_Accumulator / simDelta,
                .Clamped = clamped,
                .BudgetLimited = clamped && steps < planned,
                .DroppedSeconds = dropped,
            };
        }

        /// @brief Fixed ticks per second.
        u32 m_TickRate;
        /// @brief Per-frame step ceiling (spiral-of-death clamp).
        u32 m_MaxTicksPerFrame;
        /// @brief The per-frame wall-clock budget for Sim steps, in seconds; 0 when unbudgeted.
        f64 m_BudgetSeconds;
        /// @brief The wall time the most recent budgeted step took, in seconds; the next step's estimate.
        f64 m_StepCost = 0.0;
        /// @brief The last completed tick number.
        u64 m_Tick = 0;
        /// @brief Unspent frame time carried toward the next tick, in seconds.
        f32 m_Accumulator = 0.0f;
    };
}

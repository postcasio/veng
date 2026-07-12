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
    };

    /// @brief The Sim ticks a single frame runs, plus the frame's interpolation state.
    ///
    /// Returned by SimClock::Advance. Steps is 0 when the frame did not accumulate a full step
    /// (frame rate above the tick rate) and up to MaxTicksPerFrame when catching up. The ticks the
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
    };

    /// @brief A monotonic fixed-timestep tick counter driven by an accumulator.
    ///
    /// Advance folds a frame delta into the accumulator, runs as many whole fixed steps as have
    /// accumulated (clamped against the spiral of death), advances the tick number by that many, and
    /// reports the residual as an interpolation alpha. The simulation reads a numbered, fixed-rate
    /// tick that two machines can agree on; the View/render side reads the alpha to interpolate
    /// between the last two ticks. Reset drops the accumulator without moving the tick — a pause
    /// leaves no tick debt to chase on resume. Pure and device-free.
    class SimClock
    {
    public:
        /// @brief Constructs a clock with the default 60 Hz tick rate and clamp.
        SimClock() : SimClock(SimClockInfo{}) {}

        /// @brief Constructs the clock at tick 0 with an empty accumulator.
        /// @param info  The tick rate and spiral-of-death clamp.
        explicit SimClock(const SimClockInfo& info)
            : m_TickRate(info.TickRate), m_MaxTicksPerFrame(info.MaxTicksPerFrame)
        {
            VE_ASSERT(m_TickRate > 0, "SimClock: TickRate must be positive");
            VE_ASSERT(m_MaxTicksPerFrame > 0, "SimClock: MaxTicksPerFrame must be positive");
        }

        /// @brief Accumulates a frame delta and advances the tick by every whole fixed step it completes.
        ///
        /// Adds @p frameDelta to the accumulator, subtracts one fixed step per tick run (up to the
        /// clamp), and advances the tick number by the number of steps. A frame runs 0 steps (frame
        /// faster than the tick rate) to MaxTicksPerFrame (catching up); hitting the clamp drops the
        /// remaining backlog. The returned residual alpha interpolates into the next tick.
        /// @param frameDelta  The wall-clock frame delta in seconds (>= 0).
        /// @return This frame's step count, tick range, fixed delta, and interpolation alpha.
        SimStep Advance(const f32 frameDelta)
        {
            m_Accumulator += frameDelta;

            const f32 simDelta = 1.0f / static_cast<f32>(m_TickRate);
            const u64 firstTick = m_Tick + 1;

            u32 steps = 0;
            while (m_Accumulator >= simDelta && steps < m_MaxTicksPerFrame)
            {
                m_Accumulator -= simDelta;
                ++steps;
            }

            // Spiral-of-death clamp: a backlog past the per-frame ceiling is dropped, not chased, so
            // a long stall resyncs to the present rather than running an unbounded catch-up burst.
            if (steps == m_MaxTicksPerFrame)
            {
                m_Accumulator = 0.0f;
            }

            m_Tick += steps;

            return SimStep{
                .Steps = steps,
                .FirstTick = firstTick,
                .SimDelta = simDelta,
                .Alpha = m_Accumulator / simDelta,
            };
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
        /// @brief Fixed ticks per second.
        u32 m_TickRate;
        /// @brief Per-frame step ceiling (spiral-of-death clamp).
        u32 m_MaxTicksPerFrame;
        /// @brief The last completed tick number.
        u64 m_Tick = 0;
        /// @brief Unspent frame time carried toward the next tick, in seconds.
        f32 m_Accumulator = 0.0f;
    };
}

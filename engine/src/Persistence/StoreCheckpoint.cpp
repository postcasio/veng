#include <Veng/Persistence/StoreCheckpoint.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Persistence/Store.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <chrono>

namespace Veng
{
    StoreCheckpoint::StoreCheckpoint(Info info)
        : m_Runner(*info.Runner), m_StoreSource(std::move(info.StoreSource)),
          m_IntervalSeconds(info.IntervalSeconds)
    {
    }

    void StoreCheckpoint::Update(const f32 delta)
    {
        if (!m_StoreSource || m_StoreSource() == nullptr)
        {
            return;
        }
        m_Accumulator += delta;
        if (m_Accumulator >= m_IntervalSeconds)
        {
            m_Accumulator = 0.0;
            CheckpointNow();
        }
    }

    void StoreCheckpoint::CheckpointNow()
    {
        if (Store* const store = m_StoreSource ? m_StoreSource() : nullptr; store != nullptr)
        {
            (void)Run(*store, false);
        }
    }

    VoidResult StoreCheckpoint::CheckpointAndWait()
    {
        Store* const store = m_StoreSource ? m_StoreSource() : nullptr;
        if (store == nullptr)
        {
            return {};
        }
        VoidResult flushed = Run(*store, true);
        if (!flushed)
        {
            Log::Error("checkpoint flush failed: {}", flushed.error());
        }
        return flushed;
    }

    CheckpointCost StoreCheckpoint::LastCostMs() const
    {
        const Store* const store = m_StoreSource ? m_StoreSource() : nullptr;
        return CheckpointCost{
            .CaptureMs = m_LastCaptureMs,
            .FlushMs = m_LastFlushMs,
            .WriteMs = store != nullptr ? store->GetLastWriteMs() : 0.0,
        };
    }

    VoidResult StoreCheckpoint::Run(Store& store, const bool wait)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        {
            VE_PROFILE_SCOPE("Checkpoint/Capture");
            for (const Unique<World>& world : m_Runner.GetWorlds())
            {
                store.CaptureScene(world->GetScene());
            }
        }
        const Clock::time_point captured = Clock::now();
        VoidResult flushed;
        {
            VE_PROFILE_SCOPE("Checkpoint/Flush");
            if (wait)
            {
                flushed = store.FlushAndWait();
            }
            else
            {
                store.Flush();
            }
        }
        const Clock::time_point finished = Clock::now();
        m_LastCaptureMs = std::chrono::duration<f64, std::milli>(captured - start).count();
        m_LastFlushMs = std::chrono::duration<f64, std::milli>(finished - captured).count();
        return flushed;
    }
}

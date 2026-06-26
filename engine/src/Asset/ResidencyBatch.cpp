#include <Veng/Asset/ResidencyBatch.h>

#include <Veng/Assert.h>
#include <Veng/Task/TaskSystem.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace Veng
{
    namespace
    {
        // The watchdog ceiling on WaitResident's pump loop: a spawn's uploads land in a
        // handful of frames, so a six-figure pump count is orders of magnitude past any real
        // wait. Tripping it means a tracked handle is never finishing — abort with a diagnostic
        // rather than spin forever.
        constexpr usize WaitResidentPumpLimit = 1'000'000;
    }

    bool ResidencyBatch::IsResident() const
    {
        return std::ranges::all_of(m_Pending, [](const Ref<Detail::AssetCacheEntry>& entry)
                                   { return entry->Resource != nullptr; });
    }

    usize ResidencyBatch::ResidentCount() const
    {
        return static_cast<usize>(
            std::ranges::count_if(m_Pending, [](const Ref<Detail::AssetCacheEntry>& entry)
                                  { return entry->Resource != nullptr; }));
    }

    void ResidencyBatch::WaitResident(TaskSystem& tasks)
    {
        usize pumps = 0;
        while (!IsResident())
        {
            VE_ASSERT(pumps < WaitResidentPumpLimit,
                      "ResidencyBatch::WaitResident: {} of {} assets still pending after {} pumps "
                      "— a tracked handle is never becoming resident",
                      TotalCount() - ResidentCount(), TotalCount(), pumps);

            tasks.PumpMainThread();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++pumps;
        }
    }
}

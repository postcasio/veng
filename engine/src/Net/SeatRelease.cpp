#include <Veng/Net/SeatRelease.h>

#include <Veng/Scene/Scene.h>

namespace Veng
{
    void SeatReleaseLog::Record(const SeatRelease& release)
    {
        m_Releases.push_back(release);
    }

    void SeatReleaseLog::Clear()
    {
        m_Releases.clear();
    }

    SeatReleaseLog& EnsureSeatReleaseLog(Scene& scene)
    {
        if (SeatReleaseLog* const log = scene.GetSeatReleaseLog(); log != nullptr)
        {
            return *log;
        }
        scene.SetSeatReleaseLog(CreateUnique<SeatReleaseLog>());
        return *scene.GetSeatReleaseLog();
    }

    std::span<const SeatRelease> SeatReleasesOf(const Scene& scene)
    {
        const SeatReleaseLog* const log = scene.GetSeatReleaseLog();
        return log != nullptr ? log->GetReleases() : std::span<const SeatRelease>{};
    }
}

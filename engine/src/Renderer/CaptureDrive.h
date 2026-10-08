#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <span>

namespace Veng
{
    class Scene;
}

namespace Veng::Renderer
{
    /// @brief What the capture pre-pass knows of one registered viewport, in registration order.
    struct CapturePresenter
    {
        /// @brief Whether the viewport renders this frame (Viewport::WillRender).
        bool WillRender = false;
        /// @brief The scene the viewport presents, or null.
        const Scene* Presented = nullptr;
        /// @brief The scene an in-flight rebind of the viewport will present, or null.
        const Scene* Pending = nullptr;
    };

    /// @brief One scene whose capture surfaces the pre-pass drives, and the viewport driving them.
    struct CaptureClaim
    {
        /// @brief The scene to drive.
        const Scene* World = nullptr;
        /// @brief Index, in the presenter list, of the viewport that claims the scene.
        usize Presenter = 0;
        /// @brief Whether the scene is that viewport's pending destination rather than its
        ///        presented scene, which decides the interpolation fraction it is driven at.
        bool Pending = false;
    };

    /// @brief Decides which scenes' captures a frame drives, and from which viewport.
    ///
    /// A capture feeds a material sampled by a mesh drawn in some view, so only a scene some viewport
    /// will render this frame — or a scene an in-flight rebind is about to put on one, so it arrives
    /// with its captures warm — has its captures driven. A scene shown by several viewports is driven
    /// once, by the first in registration order, so one surface is not pushed twice in a frame. A
    /// viewport that will not render claims nothing, neither its scene nor its pending destination.
    /// @param presenters  The registered viewports, in registration order.
    /// @param claims      Filled with one claim per scene to drive, in claim order; cleared on entry.
    inline void ClaimCaptureScenes(std::span<const CapturePresenter> presenters,
                                   vector<CaptureClaim>& claims)
    {
        claims.clear();
        const auto claimed = [&claims](const Scene* scene)
        {
            return std::ranges::any_of(claims, [scene](const CaptureClaim& claim)
                                       { return claim.World == scene; });
        };
        for (usize index = 0; index < presenters.size(); ++index)
        {
            const CapturePresenter& presenter = presenters[index];
            if (!presenter.WillRender)
            {
                continue;
            }
            if (presenter.Presented != nullptr && !claimed(presenter.Presented))
            {
                claims.push_back({.World = presenter.Presented, .Presenter = index});
            }
            if (presenter.Pending != nullptr && !claimed(presenter.Pending))
            {
                claims.push_back({.World = presenter.Pending, .Presenter = index, .Pending = true});
            }
        }
    }

    /// @brief Whether a capture's drive finds a frame it was not driven on since its last drive.
    ///
    /// A capture whose scene went unrendered for a frame holds the scene as it last saw it, so the
    /// drive that finds the gap restarts the capture's refresh rather than resuming mid-refresh from
    /// content that may since have moved. Driven on consecutive frames, or twice in one, there is no
    /// gap; a capture never driven before has nothing to restart.
    /// @param lastDriven  The frame serial the capture was last driven at; 0 for never.
    /// @param current     The frame serial of this drive (Context::GetFrameSerial).
    /// @return True when at least one frame passed between the two drives.
    [[nodiscard]] constexpr bool CaptureDriveSkippedFrame(const u64 lastDriven, const u64 current)
    {
        return lastDriven != 0 && current > lastDriven + 1;
    }
}

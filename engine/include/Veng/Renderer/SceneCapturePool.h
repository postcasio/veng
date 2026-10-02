#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class SceneCapture;
    struct SceneCaptureInfo;

    /// @brief Holds released scene captures for reuse by the next capture asking for the same
    ///        configuration.
    ///
    /// A SceneCapture owns a whole face renderer — its targets, its pipelines, its bindless slots —
    /// and building one is the expensive half of a capture's life. A world swap tears every capture
    /// surface in the departing world down and builds the arriving world's from scratch, and those
    /// are often the same captures in all but owner (a reflective canopy on the same hull). The pool
    /// sits between the two: a capture whose owner lets go is detached from the drive-list, reset
    /// (SceneCapture::ResetForReuse) and held; a later Take for an identical configuration
    /// (SceneCapture::IsConfiguredFor) hands it over instead of a new one being built.
    ///
    /// The pool holds at most its capacity, dropping the longest-held capture to make room, so a run
    /// that releases captures it never asks for again holds a bounded amount of GPU memory.
    /// Single-threaded, like every renderer object. The held captures die with the pool, so it must
    /// not outlive the context and asset manager they were built against.
    class VE_API SceneCapturePool
    {
    public:
        /// @brief How many released captures a pool holds unless told otherwise.
        static constexpr usize DefaultCapacity = 4;

        /// @brief Constructs an empty pool.
        /// @param capacity  The most captures the pool holds at once; 0 holds none.
        explicit SceneCapturePool(usize capacity = DefaultCapacity);

        /// @brief Drops every held capture.
        ~SceneCapturePool();

        SceneCapturePool(const SceneCapturePool&) = delete;
        SceneCapturePool& operator=(const SceneCapturePool&) = delete;

        /// @brief Hands over a held capture configured as @p info describes, or null when none is.
        ///
        /// The capture comes back detached from any drive-list and reset, exactly as a new one from
        /// SceneCapture::Create(@p info) would be, so the caller registers it the same way.
        /// @param info  The configuration the caller would otherwise build.
        /// @return A held capture of that configuration, or null.
        [[nodiscard]] Unique<SceneCapture> Take(const SceneCaptureInfo& info);

        /// @brief Takes a released capture into the pool, detaching and resetting it.
        ///
        /// Past capacity the longest-held capture is dropped to make room; a capacity of 0 drops
        /// @p capture itself.
        /// @param capture  The capture its owner no longer wants; null is a no-op.
        void Return(Unique<SceneCapture> capture);

        /// @brief Returns how many captures the pool holds.
        [[nodiscard]] usize GetHeldCount() const { return m_Held.size(); }

        /// @brief Returns the most captures the pool holds at once.
        [[nodiscard]] usize GetCapacity() const { return m_Capacity; }

        /// @brief Drops every held capture.
        void Clear();

    private:
        /// @brief The most captures held at once.
        usize m_Capacity = DefaultCapacity;
        /// @brief The held captures, longest-held first.
        vector<Unique<SceneCapture>> m_Held;
    };
}

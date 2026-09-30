#pragma once

#include <Veng/Veng.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Scene/Entity.h>

#include <span>

// Veng/Net/SeatRelease.h — what a world learns when the host releases a remote peer's seat.
//
// A ServerHost releases a peer's seat when the peer leaves the world or its connection is lost, and
// destroys the seat entity as it does. A system that reacts only to the seat dying cannot tell the
// two apart, yet a consumer often wants to: what a seat possessed may be kept briefly for a peer that
// might reconnect, and removed at once for one that left on purpose. So the host records each release
// in the world's scene before destroying the seat, and the world's Sim systems read the record on
// its next tick.

namespace Veng
{
    class Scene;

    /// @brief Why the host released a remote peer's seat from a world.
    enum class SeatReleaseReason : u8
    {
        /// @brief The peer left the world while staying connected: it travelled elsewhere, or asked to leave.
        Left,
        /// @brief The peer's connection was lost (a timeout, a disconnect, a kick), releasing every seat it held.
        ConnectionLost,
    };

    /// @brief One seat the host released from a world, recorded before the seat entity is destroyed.
    struct SeatRelease
    {
        /// @brief The released seat entity. Dead by the time it is read; its id still names the seat.
        Entity Seat = Entity::Null;
        /// @brief The account the seat belonged to (the connection's admitted account).
        Net::AccountId Account;
        /// @brief Why the seat was released.
        SeatReleaseReason Reason = SeatReleaseReason::Left;
    };

    /// @brief The seat releases a world has recorded since its last Sim tick.
    ///
    /// Scene-owned (Scene::SetSeatReleaseLog), so it never serializes and never replicates. The host
    /// records into it between ticks; Scene clears it at the end of every Sim tick, so each Sim system
    /// that reads it during a tick sees every release since the previous tick exactly once, and a
    /// release is gone the tick after. A world that does not tick keeps its releases until it does.
    class VE_API SeatReleaseLog
    {
    public:
        /// @brief Appends one release.
        /// @param release  The release to record.
        void Record(const SeatRelease& release);

        /// @brief Returns the releases recorded since the last Sim tick, in the order they were recorded.
        [[nodiscard]] std::span<const SeatRelease> GetReleases() const { return m_Releases; }

        /// @brief Drops every recorded release. Scene calls this at the end of each Sim tick.
        void Clear();

    private:
        /// @brief The releases recorded since the last Sim tick.
        vector<SeatRelease> m_Releases;
    };

    /// @brief Returns the scene's seat release log, installing an empty one when it has none.
    /// @param scene  The scene whose log to return.
    /// @return The scene's log.
    VE_API SeatReleaseLog& EnsureSeatReleaseLog(Scene& scene);

    /// @brief Returns the seat releases a scene has recorded since its last Sim tick.
    ///
    /// The read a Sim system makes: every release since the world's previous tick, each exactly once
    /// across ticks. Empty when the scene has recorded none or has no log installed.
    /// @param scene  The scene to read.
    /// @return The releases, in the order they were recorded; valid until the scene's next Record or Clear.
    VE_API std::span<const SeatRelease> SeatReleasesOf(const Scene& scene);
}

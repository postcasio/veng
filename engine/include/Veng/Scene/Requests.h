#pragma once

#include <Veng/Veng.h>
#include <Veng/InputRouter.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

// Veng/Scene/Requests.h — the builtin, local-only request components.
//
// A gameplay system cannot reach the application-level operations that open and close worlds,
// bind the transport, or hold an input-focus token across frames: StartHosting / Connect /
// StopNet / RequestExit (and travel) are methods on Application, an InputRouter focus token is
// held by whoever pushed it, and SystemContext carries no Application back-reference. These
// components are the data channel across that gap. A system stamps one onto any world's scene; the
// engine drains it at its frame-safe point and reports the outcome back through the component's
// Status.
//
// The components are **local-only** — none is replicated (no VE_REPLICATED), so a request never
// rides a snapshot; the engine drains them on the local Application only. On a Client-tier world a
// request lowers to its client-side meaning (see the drain).
//
// Consumption semantics (uniform across all five):
//   - Handled  — the engine removes the component; absence is the acknowledgement, and the stamping
//                system may re-stamp freely.
//   - Pending  — the request is not yet handleable; it is left in place and retried next frame. A
//                system withdraws a pending request by removing the component itself.
//   - Failed   — Status is set to Failed and Error carries the reason; the component is left in
//                place for exactly one frame (the next drain removes it) so the stamping system can
//                observe the failure by reading Status before re-stamping. A system that re-stamps
//                unconditionally every frame overwrites Failed before it can be read and loops
//                silently — a stamper must read Status before re-stamping.

namespace Veng
{
    /// @brief How the engine reports a request's outcome; component absence means it was handled.
    ///
    /// A freshly-stamped request is Pending. The drain sets Failed (and fills the request's Error)
    /// when the underlying operation could not be carried out, holding the component one frame so
    /// the stamping system can read the outcome. A handled request is removed rather than marked, so
    /// there is no Handled enumerator — absence from the scene is the acknowledgement.
    enum class RequestStatus : u8
    {
        /// @brief Freshly stamped or not yet handleable; the drain will retry it.
        Pending,
        /// @brief The operation failed; Error carries the reason and the component is held one frame.
        Failed,
    };

    /// @brief Requests that a world travel to a destination — open or join it and optionally present it.
    ///
    /// The travel behaviour behind this component is a separate concern; this component carries the
    /// destination, the opaque arrival payload, the managed viewport to present on, and the
    /// present-or-not choice, and the engine drains it into the travel drive. On a Client-tier world
    /// it lowers to the client travel path.
    struct TravelRequest
    {
        /// @brief The world to travel to.
        Net::WorldKey Destination;
        /// @brief Opaque arrival data threaded into the destination; empty is valid.
        Net::Blob Payload;
        /// @brief The managed viewport index that presents the destination.
        usize ViewportIndex = 0;
        /// @brief True to present the destination on the viewport; false opens/joins it without presenting.
        bool Present = true;
        /// @brief Explicit standing choice for the session record; unset resolves to !Present.
        ///
        /// A presenting travel is the account's gameplay world, a non-presenting one a standing
        /// join restored on reattach; setting this overrides (false opts out of the record).
        optional<bool> Standing;
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };

    /// @brief Requests that the process start hosting the world (bind the transport, mount the server).
    ///
    /// Drains to Application::StartHosting. Fails with the same error that call returns — a net mode
    /// already active, or a transport that could not be opened — and additionally fails with a role
    /// error when stamped in a Client-tier world (a client cannot promote itself to a host).
    struct HostRequest
    {
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };

    /// @brief Requests that the process connect to a server as a client, optionally entering a world.
    ///
    /// Drains to Application::Connect. When Join names a world (a non-default key) the request is the
    /// connect-and-enter front door: connect, then travel to Join carrying Payload. An invalid
    /// (default) Join key is connect-only. Port 0 uses the configured default, matching Connect.
    struct ConnectRequest
    {
        /// @brief The server host to resolve and connect to.
        string Host;
        /// @brief The server port, or 0 to use the configured default.
        u16 Port = 0;
        /// @brief An optional world to travel to after connecting; a default (invalid) key is connect-only.
        Net::WorldKey Join = {};
        /// @brief Opaque arrival data for the post-connect travel; empty is valid.
        Net::Blob Payload;
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };

    /// @brief Requests that the process tear the net mode down, returning to standalone.
    ///
    /// Drains to Application::StopNet. On a Client-tier world this is the client disconnect; on a
    /// Server-tier world it stops hosting. A harmless no-op when no net mode is active, so it is
    /// always handled.
    struct StopNetRequest
    {
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };

    /// @brief Requests that the application exit at the end of the frame.
    ///
    /// Drains to Application::RequestExit; always handled.
    struct ExitRequest
    {
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };

    /// @brief Requests that a seat hold a given input focus, so a system can drive focus capture/release.
    ///
    /// An InputRouter focus token is held by whoever pushed it, which no across-frames stateless
    /// system can be. This component lets a system express the focus a seat should hold as a
    /// drained request; the engine owns a single per-seat request-driven token behind it and
    /// reconciles idempotently: a Gameplay request with no engine-held token pushes gameplay focus
    /// and stores the token, a UI request with a token held pops it, and requesting the state
    /// already held is a no-op success. The engine only ever pops the token it itself pushed, so
    /// the request-driven token composes with — and never disturbs — tokens pushed by an overlay
    /// suspend or a SeatFocusScope. Always handled (removed the same frame); the engine keeps the
    /// token across frames until a UI request releases it, so a system stamps only on the focus
    /// edge, not every frame.
    struct FocusRequest
    {
        /// @brief The seat to affect; Entity::Null resolves to the router's cursor seat.
        Entity Seat = Entity::Null;
        /// @brief The focus the seat should hold: Gameplay captures, UI releases.
        InputFocus Focus = InputFocus::Gameplay;
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };
}

/// @cond DOXYGEN_EXCLUDE
VE_ENUM(::Veng::RequestStatus, 0x0537F53655EF0946ULL)
VE_ENUMERATOR(Pending)
VE_ENUMERATOR(Failed)
VE_ENUM_END();

VE_REFLECT(::Veng::TravelRequest, 0x2FEE0FFD630E51B9ULL)
VE_FIELD(Present, .DisplayName = "Present")
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::HostRequest, 0x69D2797E9D5794ACULL)
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::ConnectRequest, 0x6A45C2885C43F557ULL)
VE_FIELD(Host, .DisplayName = "Host")
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::StopNetRequest, 0x92E7E5D795BBE775ULL)
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::ExitRequest, 0x837C01FFF9673D84ULL)
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::FocusRequest, 0x232F4A7AB3F5F74DULL)
VE_FIELD(Seat, .DisplayName = "Seat")
VE_FIELD(Focus, .DisplayName = "Focus")
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();
/// @endcond

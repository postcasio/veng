#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Requests.h>
#include <Veng/World.h>

// Scene/RequestDrain.h — the engine-internal drive that drains the builtin request components.
//
// Application::Frame calls DrainRequests at its frame-safe point (right after the deferred
// managed-viewport reconfigure, before the input snapshot and the world tick), building a
// RequestDispatch from its own operations. The drive is factored out of Application so it is
// device-free-testable: a unit test drives it over a bare WorldRunner with stub dispatch hooks.

namespace Veng
{
    class WorldRunner;

    /// @brief The outcome a dispatch hook reports for one drained request.
    enum class RequestResult
    {
        /// @brief The operation completed; the drain removes the component.
        Handled,
        /// @brief Not yet handleable; the drain leaves the component Pending to retry next frame.
        Pending,
        /// @brief The operation failed; the drain records Failed + the reason and holds it one frame.
        Failed,
    };

    /// @brief The per-request-type operations the drain dispatches a Pending request to.
    ///
    /// Each hook is invoked only for a Pending request (a held-Failed one is removed without
    /// dispatch); it carries out the request's operation against the local process and returns the
    /// outcome. On RequestResult::Failed it fills the out-parameter error with the reason. The world
    /// argument is the world whose scene carried the request, so a hook can consult the world's net
    /// role (a HostRequest in a Client-tier world fails).
    struct RequestDispatch
    {
        /// @brief Tears the net mode down (client disconnect / server stop); a no-op when standalone.
        function<RequestResult(WorldInstanceId, const StopNetRequest&, string& error)> StopNet;
        /// @brief Starts hosting the world; fails in a Client-tier world or when a net mode is active.
        function<RequestResult(WorldInstanceId, const HostRequest&, string& error)> Host;
        /// @brief Connects as a client and optionally travels to the request's Join world.
        function<RequestResult(WorldInstanceId, const ConnectRequest&, string& error)> Connect;
        /// @brief Travels the world to the request's destination.
        function<RequestResult(WorldInstanceId, const TravelRequest&, string& error)> Travel;
        /// @brief Flags the application to exit at the end of the frame.
        function<RequestResult(WorldInstanceId, const ExitRequest&, string& error)> Exit;
    };

    /// @brief Drains every open world's request components once, in the fixed type order.
    ///
    /// Captures a snapshot of the open-world ids (in id order) before it begins, so a world opened
    /// by an earlier same-frame request is not visited until next frame, then processes the request
    /// types in the order StopNet, Host, Connect, Travel, Exit — teardown before setup, exit last —
    /// across that snapshot. Within a type, worlds are visited in id order, so two requests in two
    /// worlds resolve in world-id order and the later proceeds against the earlier's post-state. Each
    /// scene holds at most one request per type (the components are the queue, depth one per scene);
    /// the drain applies the uniform handled / pending / failed consumption semantics.
    /// @param runner    The runner whose worlds' scenes are scanned.
    /// @param dispatch  The per-type operations a Pending request is dispatched to.
    void DrainRequests(WorldRunner& runner, const RequestDispatch& dispatch);
}

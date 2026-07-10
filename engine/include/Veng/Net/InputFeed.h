#pragma once

#include <Veng/Net/Connection.h>
#include <Veng/Net/Replication.h>
#include <Veng/Veng.h>

// Veng/Net/InputFeed.h — the per-frame input feed that threads Plan 05's buffers around the world
// drive. Plan 06's hosts move the seat lifecycle and the state stream; this is the client→server
// input direction the world loop owns: the client stamps its local seat's resolved input per tick
// and sends the redundant window, the server ingests each connection's redundant packet into a
// jitter buffer and feeds the buffered input into the seat at the matching tick. All device-free —
// pure orchestration over the hosts, buffers, and scenes, so it unit-tests over two in-process
// scenes exactly the way the buffers themselves do. Socket-free: sockets stay behind the Transport
// seam and the .cpp.

namespace Veng
{
    class Scene;
    class ServerHost;
    class TypeRegistry;

    /// @brief Drains each connection's redundant input packets into its jitter buffer (server receive).
    ///
    /// For every established connection, receives every queued unreliable input packet, decodes it,
    /// and ingests it into that connection's InputJitterBuffer (created on demand at @p settings), then
    /// acknowledges the replication server with the packet's piggybacked snapshot ack. A departed
    /// connection's buffer is pruned. Called once per frame, after the host Pump has received the
    /// datagrams. A malformed packet is dropped, never fatal.
    /// @param host      The server host whose connections' input channels are drained.
    /// @param buffers   The per-connection jitter buffers, keyed by connection id (grown/pruned here).
    /// @param settings  The depth tuning a newly-seen connection's jitter buffer is created with.
    /// @param registry  The type registry the ActionState records decode through.
    VE_API void IngestConnectionInputs(ServerHost& host,
                                       unordered_map<Net::ConnectionId, InputJitterBuffer>& buffers,
                                       const InputJitterBuffer::Settings& settings,
                                       const TypeRegistry& registry);

    /// @brief Consumes one buffered input per connection into its seat's PlayerInput (server per-tick).
    ///
    /// For every established connection, consumes the next input from its jitter buffer (slewing toward
    /// the target depth) and writes it into the connection's seat PlayerInput — adding the component if
    /// the seat lacks one — so the control system re-derives Intent from the wire input at this tick.
    /// A connection whose buffer has never received input feeds nothing (its seat holds its prior
    /// input). Called once per server sim step, ahead of the systems.
    /// @param host     The server host resolving each connection's seat (SeatFor).
    /// @param buffers  The per-connection jitter buffers to consume from.
    /// @param world    The server scene the seats live in.
    VE_API void FeedSeatInputs(ServerHost& host,
                               unordered_map<Net::ConnectionId, InputJitterBuffer>& buffers,
                               Scene& world);

    /// @brief Stamps the local input seat's resolved input for a client tick (client per-tick).
    ///
    /// Records the first (SeatInput, PlayerInput) entity's resolved PlayerInput — the locally-owned
    /// seat InputMappingSystem fills — into @p send at @p clientTick, so the redundant send window
    /// carries it. A scene with no local input seat stamps nothing (a spectator/observer client), so
    /// the window stays empty and the send is a header-only ack.
    /// @param send        The client's input send window.
    /// @param world       The client scene holding the local input seat.
    /// @param clientTick  The client sim tick being stamped.
    VE_API void StampLocalSeatInput(InputSendBuffer& send, const Scene& world, u64 clientTick);
}

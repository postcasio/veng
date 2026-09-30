#pragma once

#include <Veng/Net/Connection.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Replication.h>
#include <Veng/Veng.h>
#include <Veng/World.h>

// Veng/Net/InputFeed.h — the per-frame input feed that threads the input buffers around the world
// drive. The hosts move the seat lifecycle and the state stream; this is the client→server input
// direction the world loop owns: the client stamps its local seat's resolved input per tick and
// sends the redundant window, the server ingests each (connection, joined world)'s redundant packet
// into a jitter buffer and feeds the buffered input into that world's seat at the matching tick.
// Because a connection multiplexes N worlds, the jitter buffers key per (connection, JoinId), and an
// inbound packet is demuxed by its JoinId and dropped if the connection was never granted that tag.
// All device-free — pure orchestration over the hosts, buffers, and scenes, so it unit-tests over
// in-process scenes exactly the way the buffers themselves do. Socket-free: sockets stay behind the
// Transport seam and the .cpp.

namespace Veng
{
    class Scene;
    class ServerHost;
    class TypeRegistry;

    /// @brief How far behind its own tick the view a seat's current input was issued against was drawn.
    ///
    /// Written onto a connection's seat by FeedSeatInputs beside the PlayerInput it feeds, from the
    /// view delay the client stamped on that input (TickedInput::ViewDelayTicks), and read back
    /// through SeatViewTick. A seat fed locally carries none, since its view is the present.
    /// Runtime-only: it carries no reflected field, so it never serializes and never rides the wire.
    struct InputViewDelay
    {
        /// @brief The delay in (fractional) ticks; never negative.
        f32 Ticks = 0.0f;
    };

    /// @brief Returns the (fractional) tick a seat's view was drawn at when it issued its input for @p inputTick.
    ///
    /// The server-side read of the client's view tick: @p inputTick minus the seat's InputViewDelay,
    /// or @p inputTick itself for a seat carrying none (a listen host's own seat, which draws the
    /// present). Meaningful for the tick being fed — the scheduled feed consumes the client's input
    /// for tick T at server tick T, so a Sim system passes its own tick. Hand the result to a
    /// RewindScope to judge that input against the world as the seat saw it.
    /// @param scene      The scene holding the seat.
    /// @param seat       The seat whose input is being judged.
    /// @param inputTick  The tick whose input is being judged.
    /// @return The view tick, at most @p inputTick.
    [[nodiscard]] VE_API f64 SeatViewTick(const Scene& scene, Entity seat, u64 inputTick);

    /// @brief Packs a (connection, join) pair into the key the per-world jitter buffers are stored by.
    ///
    /// The two id spaces are disjoint on the wire but share a process, so the jitter buffers key on
    /// both: a connection's input for one joined world is a distinct stream from its input for another.
    /// @param conn  The connection id.
    /// @param join  The connection's JoinId for the world the input targets.
    /// @return The packed 64-bit buffer key.
    [[nodiscard]] inline u64 InputBufferKey(Net::ConnectionId conn, Net::JoinId join)
    {
        return (static_cast<u64>(conn) << 16) | static_cast<u64>(join);
    }

    /// @brief Drains each connection's redundant input packets into its per-world jitter buffers (server receive).
    ///
    /// For every established connection, receives every queued unreliable packet, decodes its
    /// world-multiplexing envelope (dropping a short/garbage frame or one tagged with a JoinId the
    /// connection was never granted), decodes the input payload, and ingests it into the
    /// (connection, join)'s InputJitterBuffer (created on demand at @p settings), then acknowledges
    /// that world's replication server with the packet's piggybacked snapshot ack. A departed
    /// connection's or join's buffers are pruned. Called once per frame, after the host Pump has
    /// received the datagrams. A malformed packet is dropped, never fatal.
    /// @param host      The server host whose connections' input channels are drained.
    /// @param buffers   The per-(connection, join) jitter buffers, keyed by InputBufferKey (grown/pruned here).
    /// @param settings  The depth tuning a newly-seen (connection, join)'s jitter buffer is created with.
    /// @param registry  The type registry the ActionState records decode through.
    VE_API void IngestConnectionInputs(ServerHost& host,
                                       unordered_map<u64, InputJitterBuffer>& buffers,
                                       const InputJitterBuffer::Settings& settings,
                                       const TypeRegistry& registry);

    /// @brief Consumes one buffered input per join in a world into its seat's PlayerInput (server per-tick).
    ///
    /// For every (connection, join) bound to @p worldId, consumes the next input from its jitter
    /// buffer (slewing toward the target depth) and writes it into that join's seat PlayerInput —
    /// adding the component if the seat lacks one — so the control system re-derives Intent from the
    /// wire input at this tick, and writes the input's view delay into the seat's InputViewDelay. A
    /// join whose buffer has never received input feeds nothing. Called once per Sim step of
    /// @p worldId, ahead of the systems.
    /// @param host     The server host resolving each join's seat and replication server.
    /// @param buffers  The per-(connection, join) jitter buffers to consume from.
    /// @param worldId  The hosted world whose joins are fed.
    /// @param scene    The world's scene the seats live in.
    VE_API void FeedSeatInputs(ServerHost& host, unordered_map<u64, InputJitterBuffer>& buffers,
                               WorldInstanceId worldId, Scene& scene);

    /// @brief Consumes each join's input scheduled for @p serverTick into its seat (scheduled feed).
    ///
    /// The ahead-of-server variant: for every (connection, join) bound to @p worldId it consumes the
    /// input the client stamped at @p serverTick (InputJitterBuffer::ConsumeForTick, falling back to
    /// the underrun coast when the client's input for that tick has not arrived), writes it into the
    /// seat's PlayerInput and its view delay into the seat's InputViewDelay, and rides the buffer's
    /// remaining depth back as that connection's input-timing feedback on that world's next snapshot
    /// header (ReplicationServer::SetInputFeedback) — the closed loop the client's per-JoinId
    /// tick-offset controller trims its lead against. Called once per Sim step of @p worldId at its
    /// tick.
    /// @param host        The server host resolving each join's seat and replication server.
    /// @param buffers     The per-(connection, join) jitter buffers to consume from.
    /// @param worldId     The hosted world whose joins are fed.
    /// @param scene       The world's scene the seats live in.
    /// @param serverTick  The current server sim tick whose matching client input is consumed.
    VE_API void FeedSeatInputs(ServerHost& host, unordered_map<u64, InputJitterBuffer>& buffers,
                               WorldInstanceId worldId, Scene& scene, u64 serverTick);

    /// @brief Stamps the local input seat's resolved input for a client tick (client per-tick).
    ///
    /// Records the first (SeatInput, PlayerInput) entity's resolved PlayerInput — the locally-owned
    /// seat InputMappingSystem fills — into @p send at @p clientTick, so the redundant send window
    /// carries it, together with the tick the scene's remote entities were last drawn at
    /// (RemotePlaybackTick; the present when the scene draws nothing in the past). A scene with no
    /// local input seat stamps nothing (a spectator/observer client), so the window stays empty and
    /// the send is a header-only ack.
    /// @param send        The client's input send window.
    /// @param world       The client scene holding the local input seat.
    /// @param clientTick  The client sim tick being stamped.
    VE_API void StampLocalSeatInput(InputSendBuffer& send, const Scene& world, u64 clientTick);
}

VE_TYPE(::Veng::InputViewDelay, 0xA9C8B5ADD5402283ULL);

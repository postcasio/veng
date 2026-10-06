#include <Veng/Net/InputFeed.h>

#include <Veng/Net/Host.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Net/WorldEnvelope.h>

#include <algorithm>
#include <iterator>
#include <span>

namespace Veng
{
    namespace
    {
        // The wire input fills the seat's PlayerInput; the control system re-derives Intent from it
        // unchanged. A remote seat carries no SeatInput, so nothing else writes it.
        void WriteSeatInput(Scene& scene, const Entity seat, const ActionState& state,
                            const f32 viewDelayTicks)
        {
            PlayerInput& input = scene.Has<PlayerInput>(seat) ? scene.Get<PlayerInput>(seat)
                                                              : scene.Add<PlayerInput>(seat);
            input.State = state;
            InputViewDelay& delay = scene.Has<InputViewDelay>(seat)
                                        ? scene.Get<InputViewDelay>(seat)
                                        : scene.Add<InputViewDelay>(seat);
            delay.Ticks = viewDelayTicks;
        }
    }

    f64 SeatViewTick(const Scene& scene, const Entity seat, const u64 inputTick)
    {
        const InputViewDelay* delay =
            scene.IsAlive(seat) ? scene.TryGet<InputViewDelay>(seat) : nullptr;
        const f64 ticks = delay != nullptr ? static_cast<f64>(std::max(delay->Ticks, 0.0f)) : 0.0;
        return static_cast<f64>(inputTick) - ticks;
    }

    void IngestConnectionInputs(ServerHost& host, unordered_map<u64, InputJitterBuffer>& buffers,
                                const InputJitterBuffer::Settings& settings,
                                const TypeRegistry& registry)
    {
        Net::Server& server = host.Server();
        const std::span<const Net::ConnectionId> connections = server.Connections();

        for (const Net::ConnectionId id : connections)
        {
            Net::Connection& connection = server.Get(id);
            while (const optional<vector<u8>> packet =
                       connection.Receive(Net::Channel::UnreliableSequenced))
            {
                // Peel the world-multiplexing envelope; a short/garbage frame or a JoinId this
                // connection was never granted is dropped before any routing.
                const optional<Net::WorldEnvelope> env = Net::DecodeWorldEnvelope(*packet);
                if (!env || env->Join == Net::ControlJoinId || !host.IsGranted(id, env->Join))
                {
                    continue;
                }

                const Result<InputPacket> decoded = DecodeInputPacket(env->Payload, registry);
                if (!decoded)
                {
                    continue;
                }

                InputJitterBuffer& buffer =
                    buffers.try_emplace(InputBufferKey(id, env->Join), InputJitterBuffer(settings))
                        .first->second;
                buffer.Ingest(*decoded);
                ReplicationServer& replication = host.ReplicationForJoin(id, env->Join);
                replication.Acknowledge(id, decoded->AckedServerTick);
                replication.AcknowledgeComponentState(id, decoded->AckedStateSequence);
            }
        }

        // Prune a departed connection's or join's buffers so a reused id never inherits stale input.
        for (auto it = buffers.begin(); it != buffers.end();)
        {
            const auto conn = static_cast<Net::ConnectionId>(it->first >> 16);
            const auto join = static_cast<Net::JoinId>(it->first & 0xFFFFu);
            it = host.IsGranted(conn, join) ? std::next(it) : buffers.erase(it);
        }
    }

    void FeedSeatInputs(ServerHost& host, unordered_map<u64, InputJitterBuffer>& buffers,
                        const WorldInstanceId worldId, Scene& scene)
    {
        for (const Net::ConnectionId id : host.Server().Connections())
        {
            for (const Net::JoinId join : host.JoinsFor(id))
            {
                if (host.WorldForJoin(id, join) != worldId)
                {
                    continue;
                }
                const auto it = buffers.find(InputBufferKey(id, join));
                if (it == buffers.end())
                {
                    continue;
                }
                const optional<ActionState> consumed = it->second.Consume();
                if (!consumed)
                {
                    continue;
                }

                const Entity seat = host.SeatFor(id, join);
                if (seat.IsNull() || !scene.IsAlive(seat))
                {
                    continue;
                }

                WriteSeatInput(scene, seat, *consumed, it->second.GetLastViewDelayTicks());
            }
        }
    }

    namespace
    {
        // The buffered-depth cushion the feedback loop steers each client's lead toward: a couple of
        // ticks of input held ahead of the consume front absorbs jitter without over-leading.
        constexpr i32 DesiredInputCushion = 2;
    }

    void FeedSeatInputs(ServerHost& host, unordered_map<u64, InputJitterBuffer>& buffers,
                        const WorldInstanceId worldId, Scene& scene, const u64 serverTick)
    {
        for (const Net::ConnectionId id : host.Server().Connections())
        {
            for (const Net::JoinId join : host.JoinsFor(id))
            {
                if (host.WorldForJoin(id, join) != worldId)
                {
                    continue;
                }
                const auto it = buffers.find(InputBufferKey(id, join));
                if (it == buffers.end())
                {
                    continue;
                }
                const optional<ActionState> consumed = it->second.ConsumeForTick(serverTick);

                // Ride the remaining buffered depth back as this connection's timing feedback: a deep
                // buffer means the client leads by more than it needs and can slew its tick back.
                ReplicationServer& replication = host.ReplicationForJoin(id, join);
                replication.SetInputFeedback(id, static_cast<i32>(it->second.Depth()) -
                                                     DesiredInputCushion);

                // Confirm which client input tick this state reflects: the reconciler on the client
                // compares its recorded prediction at this tick against the resulting snapshot.
                replication.SetLastConsumedInputTick(id, it->second.LastConsumedTick());

                if (!consumed)
                {
                    continue;
                }

                const Entity seat = host.SeatFor(id, join);
                if (seat.IsNull() || !scene.IsAlive(seat))
                {
                    continue;
                }

                WriteSeatInput(scene, seat, *consumed, it->second.GetLastViewDelayTicks());
            }
        }
    }

    void StampLocalSeatInput(InputSendBuffer& send, const Scene& world, const u64 clientTick)
    {
        // The local input seat is a (SeatInput, PlayerInput) entity — the one InputMappingSystem
        // resolves from local devices. Prefer the locally-owned one when a peer holds several, and
        // fall back to the first so a single-seat client is unchanged. A client with none (a
        // spectator) stamps nothing.
        const optional<f64> viewTick = RemotePlaybackTick(world);
        bool stamped = false;
        const PlayerInput* firstInput = nullptr;
        world.Each<SeatInput, PlayerInput>(
            [&](const Entity seat, const SeatInput&, const PlayerInput& input)
            {
                if (stamped)
                {
                    return;
                }
                if (IsLocallyOwned(world, seat))
                {
                    send.Stamp(clientTick, input.State, viewTick);
                    stamped = true;
                    return;
                }
                if (firstInput == nullptr)
                {
                    firstInput = &input;
                }
            });
        if (!stamped && firstInput != nullptr)
        {
            send.Stamp(clientTick, firstInput->State, viewTick);
        }
    }
}

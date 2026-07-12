#include <Veng/Net/InputFeed.h>

#include <Veng/Net/Host.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>
#include <iterator>
#include <span>

namespace Veng
{
    void IngestConnectionInputs(ServerHost& host,
                                unordered_map<Net::ConnectionId, InputJitterBuffer>& buffers,
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
                const Result<InputPacket> decoded = DecodeInputPacket(*packet, registry);
                if (!decoded)
                {
                    continue;
                }

                InputJitterBuffer& buffer =
                    buffers.try_emplace(id, InputJitterBuffer(settings)).first->second;
                buffer.Ingest(*decoded);
                host.Replication().Acknowledge(id, decoded->AckedServerTick);
            }
        }

        // Prune a departed connection's buffer so a reused id never inherits stale input.
        for (auto it = buffers.begin(); it != buffers.end();)
        {
            const bool live = std::ranges::find(connections, it->first) != connections.end();
            it = live ? std::next(it) : buffers.erase(it);
        }
    }

    void FeedSeatInputs(ServerHost& host,
                        unordered_map<Net::ConnectionId, InputJitterBuffer>& buffers, Scene& world)
    {
        for (const Net::ConnectionId id : host.Server().Connections())
        {
            const auto it = buffers.find(id);
            if (it == buffers.end())
            {
                continue;
            }
            const optional<ActionState> consumed = it->second.Consume();
            if (!consumed)
            {
                continue;
            }

            const Entity seat = host.SeatFor(id);
            if (seat.IsNull() || !world.IsAlive(seat))
            {
                continue;
            }

            // The wire input fills the seat's PlayerInput; the control system re-derives Intent from
            // it unchanged. A remote seat carries no SeatInput, so nothing else writes this component.
            PlayerInput& input = world.Has<PlayerInput>(seat) ? world.Get<PlayerInput>(seat)
                                                              : world.Add<PlayerInput>(seat);
            input.State = *consumed;
        }
    }

    namespace
    {
        // The buffered-depth cushion the feedback loop steers each client's lead toward: a couple of
        // ticks of input held ahead of the consume front absorbs jitter without over-leading.
        constexpr i32 DesiredInputCushion = 2;
    }

    void FeedSeatInputs(ServerHost& host,
                        unordered_map<Net::ConnectionId, InputJitterBuffer>& buffers, Scene& world,
                        const u64 serverTick)
    {
        for (const Net::ConnectionId id : host.Server().Connections())
        {
            const auto it = buffers.find(id);
            if (it == buffers.end())
            {
                continue;
            }
            const optional<ActionState> consumed = it->second.ConsumeForTick(serverTick);

            // Ride the remaining buffered depth back as this connection's timing feedback: a deep
            // buffer means the client leads by more than it needs and can slew its tick back.
            host.Replication().SetInputFeedback(id, static_cast<i32>(it->second.Depth()) -
                                                        DesiredInputCushion);

            // Confirm which client input tick this state reflects: the reconciler on the client
            // compares its recorded prediction at this tick against the resulting snapshot.
            host.Replication().SetLastConsumedInputTick(id, it->second.LastConsumedTick());

            if (!consumed)
            {
                continue;
            }

            const Entity seat = host.SeatFor(id);
            if (seat.IsNull() || !world.IsAlive(seat))
            {
                continue;
            }

            PlayerInput& input = world.Has<PlayerInput>(seat) ? world.Get<PlayerInput>(seat)
                                                              : world.Add<PlayerInput>(seat);
            input.State = *consumed;
        }
    }

    void StampLocalSeatInput(InputSendBuffer& send, const Scene& world, const u64 clientTick)
    {
        // The local input seat is the first (SeatInput, PlayerInput) entity — the one InputMappingSystem
        // resolves from local devices. A client with none (a spectator) stamps nothing.
        bool stamped = false;
        world.Each<SeatInput, PlayerInput>(
            [&](const Entity, const SeatInput&, const PlayerInput& input)
            {
                if (!stamped)
                {
                    send.Stamp(clientTick, input.State);
                    stamped = true;
                }
            });
    }
}

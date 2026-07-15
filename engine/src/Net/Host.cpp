#include <Veng/Net/Host.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include "Handshake.h"
#include <Veng/Net/WorldEnvelope.h>

#include <algorithm>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>

namespace Veng
{
    namespace
    {
        // The default prediction policy: the pawn plus every descendant in its Hierarchy subtree that
        // carries replicated state. A purely client-local (view) child carries none, so it is left as
        // it is — only the pawn and the replicated attachments it drags along are predicted.
        vector<Entity> DefaultPredictionSet(const Scene& scene, const Entity pawn)
        {
            vector<Entity> set;
            if (pawn.IsNull() || !scene.IsAlive(pawn))
            {
                return set;
            }

            vector<TypeId> replicated;
            for (const auto& [id, info] : scene.GetTypeRegistry().All())
            {
                if (info.Replicated)
                {
                    replicated.push_back(id);
                }
            }
            const auto carriesReplicated = [&](const Entity entity)
            {
                for (const TypeId id : replicated)
                {
                    if (scene.TryGetComponent(entity, id) != nullptr)
                    {
                        return true;
                    }
                }
                return false;
            };

            // The pawn always predicts; a descendant joins only when it carries replicated state.
            set.push_back(pawn);
            vector<Entity> stack;
            scene.ForEachChild(pawn, [&](const Entity child) { stack.push_back(child); });
            while (!stack.empty())
            {
                const Entity entity = stack.back();
                stack.pop_back();
                if (scene.IsAlive(entity) && carriesReplicated(entity))
                {
                    set.push_back(entity);
                }
                scene.ForEachChild(entity, [&](const Entity child) { stack.push_back(child); });
            }
            return set;
        }

        // The local input seat's resolved input — the first (SeatInput, PlayerInput) entity, the one
        // InputMappingSystem fills from local devices (the StampLocalSeatInput seat). Null when the
        // client carries no local input seat (a spectator).
        const PlayerInput* FindLocalSeatInput(const Scene& scene)
        {
            const PlayerInput* found = nullptr;
            scene.Each<SeatInput, PlayerInput>(
                [&](const Entity, const SeatInput&, const PlayerInput& input)
                {
                    if (found == nullptr)
                    {
                        found = &input;
                    }
                });
            return found;
        }

        // The client→server per-world readiness message: a single-byte payload the ServerHost gates
        // its per-join stream on. Rides the world-multiplexing envelope tagged with its JoinId, so its
        // id need only stay clear of the replication Spawn/Despawn message ids inside a world payload.
        constexpr u8 ClientReadyMessageId = 32;

        vector<u8> EncodeClientReady()
        {
            return vector<u8>{ClientReadyMessageId};
        }

        bool IsClientReady(std::span<const u8> payload)
        {
            return payload.size() == 1 && payload[0] == ClientReadyMessageId;
        }
    }

    // ---- ServerHost ----------------------------------------------------------------------------

    struct ServerHost::State
    {
        // One hosted world: its scene, seat rule, and its own replication instance and wire-id space.
        // Its existence, presence refcount, keep-warm dwell, and idle reap are the WorldDirectory's; the
        // host holds only the replication state keyed by WorldInstanceId, dropped when the directory
        // reaps the world.
        struct HostedWorld
        {
            WorldInstanceId Id;
            Net::WorldKey Key;
            Scene* World = nullptr;
            AssetId LevelId;
            Net::ContentDigest Digest;
            Ref<Prefab> SeatPrefab;
            AssetId SeatPrefabId;
            ReplicationServer Replication;
            NetIdAllocator Allocator;
            Net::InterestSettings InterestSettings;
            Net::InterestPolicy InterestPolicy;
        };

        // One (connection, join): the joined world, the seat spawned in it, the readiness gate, and
        // the per-join interest bookkeeping.
        struct JoinState
        {
            Net::JoinId Join = Net::ControlJoinId;
            WorldInstanceId World;
            Entity Seat = Entity::Null;
            bool Ready = false;
            Net::InterestState Interest;
        };

        // One connection's joins, its per-connection JoinId allocator, and the WorldKey → JoinId
        // dedupe so a repeat join of the same key is idempotent.
        struct ConnectionState
        {
            std::map<Net::JoinId, JoinState> Joins; // ordered, so the first is the current-join
            std::unordered_map<Net::WorldKey, Net::JoinId> KeyToJoin;
            Net::JoinId NextJoin = 1; // monotonic per connection, never reused, one reserved
        };

        Net::ServerInfo InfoServer;
        AssetManager* Assets = nullptr;
        Unique<Net::Server> Server;

        // The hosted worlds' replication state keyed by WorldInstanceId value; the primary is the initial
        // world the no-arg accessors resolve. The WorldKey → bucket map, presence, dwell, and reap live
        // in the directory below; this map is the host's replication complement, dropped on reap.
        std::unordered_map<u64, HostedWorld> Worlds;
        WorldInstanceId Primary;

        // The role-neutral get-or-place directory: owned (built from the ServerHostInfo hooks) unless the
        // caller borrowed a shared one. Directory always points at the live one.
        Unique<WorldDirectory> OwnedDirectory;
        WorldDirectory* Directory = nullptr;

        std::unordered_map<Net::ConnectionId, ConnectionState> Connections;
        vector<Net::NetEvent> Events;

        HostedWorld& WorldOf(WorldInstanceId id)
        {
            const auto it = Worlds.find(id.Value);
            VE_ASSERT(it != Worlds.end(), "no hosted world for id {}", id.Value);
            return it->second;
        }

        [[nodiscard]] HostedWorld* TryWorldOf(WorldInstanceId id)
        {
            const auto it = Worlds.find(id.Value);
            return it != Worlds.end() ? &it->second : nullptr;
        }

        // The current-join convenience: a connection's first (lowest) granted join, or nullptr.
        [[nodiscard]] JoinState* CurrentJoinState(Net::ConnectionId id)
        {
            const auto it = Connections.find(id);
            if (it == Connections.end() || it->second.Joins.empty())
            {
                return nullptr;
            }
            return &it->second.Joins.begin()->second;
        }

        [[nodiscard]] JoinState* JoinStateOf(Net::ConnectionId id, Net::JoinId join)
        {
            const auto it = Connections.find(id);
            if (it == Connections.end())
            {
                return nullptr;
            }
            const auto jit = it->second.Joins.find(join);
            return jit != it->second.Joins.end() ? &jit->second : nullptr;
        }

        // The connection's interest set for a joined world this snapshot: the spatial query around its
        // pawn ∪ the always-relevant entities ∪ the policy hook ∪ the entities it owns. Empty optional
        // when interest is off (Radius 0), so Generate replicates the whole world.
        optional<set<NetId>> ComputeInterest(HostedWorld& world, Net::ConnectionId id,
                                             JoinState& join)
        {
            Scene& scene = *world.World;
            if (world.InterestSettings.Radius <= 0.0f)
            {
                return std::nullopt;
            }

            const Entity seatEntity = join.Seat;
            Entity pawn = Entity::Null;
            if (!seatEntity.IsNull() && scene.IsAlive(seatEntity))
            {
                if (const auto* possesses = scene.TryGet<Possesses>(seatEntity))
                {
                    pawn = possesses->Pawn;
                }
            }

            vec3 viewerPos(0.0f);
            if (!pawn.IsNull() && scene.IsAlive(pawn))
            {
                if (const auto* transform = scene.TryGet<Transform>(pawn))
                {
                    viewerPos = transform->Position;
                }
            }

            const vector<Net::InterestCandidate> spatial = Net::GatherSpatialCandidates(
                scene, viewerPos,
                world.InterestSettings.Radius * world.InterestSettings.LeaveMultiplier);
            const vector<NetId> alwaysRelevant = Net::GatherAlwaysRelevant(scene);

            // The policy hook's entities plus every entity this connection owns — a connection always
            // sees what it owns (its pawn and attachments), the predicted set, regardless of distance.
            vector<NetId> extra;
            if (world.InterestPolicy)
            {
                for (const Entity entity : world.InterestPolicy(scene, seatEntity, pawn))
                {
                    if (!entity.IsNull() && scene.IsAlive(entity) && scene.Has<NetIdentity>(entity))
                    {
                        extra.push_back(scene.Get<NetIdentity>(entity).Id);
                    }
                }
            }
            const TypeId authorityId = TypeIdOf<Authority>();
            for (auto [entity, identity] : scene.View<NetIdentity>())
            {
                if (identity.Id == InvalidNetId)
                {
                    continue;
                }
                const auto* authority =
                    static_cast<const Authority*>(scene.TryGetComponent(entity, authorityId));
                if (authority != nullptr && authority->Owner == id)
                {
                    extra.push_back(identity.Id);
                }
            }

            return Net::UpdateInterest(spatial, alwaysRelevant, extra, world.InterestSettings,
                                       join.Interest);
        }

        // Spawns a seat into a joined world: a Viewer seat with Authority{ Server, Owner = id } and no
        // SeatInput (its input arrives from the wire). Returns the seat's freshly assigned wire id,
        // drawn from that world's allocator.
        u32 SpawnSeat(HostedWorld& world, Net::ConnectionId id, Entity& outSeat)
        {
            Scene& scene = *world.World;
            Entity seat = Entity::Null;
            if (world.SeatPrefab)
            {
                Prefab::SpawnResult spawned = world.SeatPrefab->SpawnInto(scene, *Assets);
                seat = spawned.Roots.empty() ? scene.CreateEntity() : spawned.Roots.front();
            }
            else
            {
                seat = scene.CreateEntity();
            }

            // A seat is a Viewer with a Possesses link; a remote seat carries no local device
            // assignment, so its PlayerInput is fed from the wire, never resolved from devices.
            if (!scene.Has<Viewer>(seat))
            {
                scene.Add<Viewer>(seat);
            }
            if (!scene.Has<Possesses>(seat))
            {
                scene.Add<Possesses>(seat);
            }
            if (scene.Has<SeatInput>(seat))
            {
                scene.Remove<SeatInput>(seat);
            }

            const Authority owner{.Tier = Tier::Server, .Owner = id};
            if (scene.Has<Authority>(seat))
            {
                scene.Get<Authority>(seat) = owner;
            }
            else
            {
                scene.Add<Authority>(seat, owner);
            }

            const u32 netId = world.Allocator.Next();
            if (scene.Has<NetIdentity>(seat))
            {
                scene.Get<NetIdentity>(seat).Id = netId;
            }
            else
            {
                scene.Add<NetIdentity>(seat).Id = netId;
            }

            outSeat = seat;
            return netId;
        }

        // Wraps a factory resolution into a hosted world's replication state, if not already present.
        // The directory recorded the bucket; this is the host's replication complement.
        void EnsureHostedWorld(const Net::WorldKey& key, const ServerWorldResolution& resolved)
        {
            if (Worlds.contains(resolved.WorldId.Value))
            {
                return;
            }
            Worlds.emplace(resolved.WorldId.Value,
                           HostedWorld{.Id = resolved.WorldId,
                                       .Key = key,
                                       .World = resolved.World,
                                       .LevelId = resolved.LevelId,
                                       .Digest = resolved.Digest,
                                       .SeatPrefab = resolved.SeatPrefab,
                                       .SeatPrefabId = resolved.SeatPrefabId,
                                       .Replication = ReplicationServer(resolved.Replication),
                                       .InterestSettings = resolved.Interest,
                                       .InterestPolicy = resolved.InterestPolicy});
        }

        // Resolves a join request through the directory in the fixed order (idempotent hit, then
        // authorize → per-connection cap → get-or-place → hosted cap → factory), then assigns a JoinId
        // and spawns the seat. Sends the reply (accept or deny) enveloped at the join-control tier. The
        // opaque payload rides into the resolution and is echoed (the bucket's recorded params) in the
        // accept.
        void ResolveJoin(Net::ConnectionId id, const Net::JoinRequestMessage& request)
        {
            ConnectionState& conn = Connections[id];

            const auto deny = [&](Net::JoinDenyReason reason)
            {
                const vector<u8> payload = Net::EncodeJoinDeny(
                    Net::JoinDenyMessage{.RequestToken = request.RequestToken, .Reason = reason});
                (void)Server->Get(id).Send(Net::Channel::ReliableOrdered,
                                           Net::EncodeWorldEnvelope(Net::ControlJoinId, payload));
                Log::Warn("ServerHost denying join for connection {}: reason {}", id,
                          static_cast<u32>(reason));
            };

            const auto sendAccept = [&](Net::JoinId joinId, const HostedWorld& world, NetId seatNet)
            {
                const vector<u8> payload = Net::EncodeJoinAccept(
                    Net::JoinAcceptMessage{.RequestToken = request.RequestToken,
                                           .Join = joinId,
                                           .LevelId = world.LevelId.Value,
                                           .WorldDigest = world.Digest,
                                           .SeatNetId = seatNet,
                                           .Payload = Directory->PayloadOf(world.Id)});
                (void)Server->Get(id).Send(Net::Channel::ReliableOrdered,
                                           Net::EncodeWorldEnvelope(Net::ControlJoinId, payload));
            };

            // Idempotent: a repeat join of the same key re-accepts with the existing JoinId.
            if (const auto existing = conn.KeyToJoin.find(request.Key);
                existing != conn.KeyToJoin.end())
            {
                const JoinState& join = conn.Joins.at(existing->second);
                HostedWorld& world = WorldOf(join.World);
                const NetId seatNet = world.World->Has<NetIdentity>(join.Seat)
                                          ? world.World->Get<NetIdentity>(join.Seat).Id
                                          : InvalidNetId;
                sendAccept(join.Join, world, seatNet);
                return;
            }

            const WorldResolveResult resolve = Directory->Resolve(
                id, request.Key, request.Payload, static_cast<u32>(conn.Joins.size()));
            if (resolve.Outcome == WorldResolveOutcome::Denied)
            {
                deny(resolve.Reason);
                return;
            }
            if (resolve.Outcome == WorldResolveOutcome::Opened)
            {
                EnsureHostedWorld(request.Key, *resolve.Opened);
            }

            const WorldInstanceId worldId = resolve.World;
            HostedWorld& world = WorldOf(worldId);
            const Net::JoinId joinId = conn.NextJoin;
            conn.NextJoin += 1;

            Entity seat = Entity::Null;
            const u32 seatNetId = SpawnSeat(world, id, seat);
            world.Replication.AddConnection(id);
            if (world.SeatPrefabId.IsValid())
            {
                world.Replication.SetEntityPrefab(seatNetId, world.SeatPrefabId);
            }
            conn.Joins.emplace(joinId, JoinState{.Join = joinId, .World = worldId, .Seat = seat});
            conn.KeyToJoin.emplace(request.Key, joinId);

            // Report the live join as presence to the directory, which owns the refcount and dwell.
            Directory->AddJoin(worldId);

            sendAccept(joinId, world, seatNetId);
            Log::Info("ServerHost accepted connection {} into world {} as join {}", id,
                      worldId.Value, joinId);
        }

        // Releases a join from its world: destroys the seat and reports the presence drop to the
        // directory, which starts the idle dwell for a bucket that just emptied.
        void ReleaseJoin(Net::ConnectionId id, JoinState& join, f64 now)
        {
            HostedWorld* world = TryWorldOf(join.World);
            if (world == nullptr)
            {
                return;
            }
            if (!join.Seat.IsNull() && world->World->IsAlive(join.Seat))
            {
                world->World->DestroyEntity(join.Seat);
            }
            world->Replication.RemoveConnection(id);
            Directory->RemoveJoin(join.World, now);
        }

        // Sends a directed-travel control message to a connection (a travel reply, or unprompted).
        void SendDirectedTravel(Net::ConnectionId id, Net::JoinId leave, const Net::WorldKey& key,
                                const Net::TravelPayload& payload)
        {
            const vector<u8> message = Net::EncodeDirectedTravel(
                Net::DirectedTravelMessage{.Leave = leave, .Join = key, .Payload = payload});
            (void)Server->Get(id).Send(Net::Channel::ReliableOrdered,
                                       Net::EncodeWorldEnvelope(Net::ControlJoinId, message));
        }

        // Reaps idle buckets through the directory (its CloseWorld hook + optional runner teardown),
        // then drops each reaped world's host-side replication state.
        void ReapIdleWorlds(f64 now)
        {
            for (const WorldInstanceId id : Directory->ReapIdle(now))
            {
                Worlds.erase(id.Value);
            }
        }
    };

    ServerHost::ServerHost(Unique<State> state) : m_State(std::move(state)) {}

    ServerHost::~ServerHost() = default;

    Result<Unique<ServerHost>> ServerHost::Create(const ServerHostInfo& info)
    {
        auto state = CreateUnique<State>();
        state->Assets = &info.Assets;
        state->Primary = info.WorldId;

        // Consume a borrowed directory when given one (the Application-shared path); otherwise build a
        // private one from the info's caps and policy hooks (the self-contained ServerHost).
        if (info.Directory != nullptr)
        {
            state->Directory = info.Directory;
        }
        else
        {
            state->OwnedDirectory = WorldDirectory::Create(WorldDirectoryInfo{
                .MaxHostedWorlds = info.MaxHostedWorlds,
                .MaxJoinedWorldsPerConnection = info.MaxJoinedWorldsPerConnection,
                .MaxPlayersPerInstance = info.MaxPlayersPerInstance,
                .IdleKeepWarmDwell = info.IdleKeepWarmDwell,
                .Authorize = info.Authorize,
                .WorldFactory = info.WorldFactory,
                .Placement = info.Placement,
                .CloseWorld = info.CloseWorld,
            });
            state->Directory = state->OwnedDirectory.get();
        }

        state->Worlds.emplace(info.WorldId.Value,
                              State::HostedWorld{.Id = info.WorldId,
                                                 .Key = info.Key,
                                                 .World = &info.World,
                                                 .LevelId = info.LevelId,
                                                 .Digest = info.Digest,
                                                 .SeatPrefab = info.SeatPrefab,
                                                 .SeatPrefabId = info.SeatPrefabId,
                                                 .Replication = ReplicationServer(info.Replication),
                                                 .InterestSettings = info.Interest,
                                                 .InterestPolicy = info.InterestPolicy});
        state->Directory->Register(info.Key, info.WorldId);

        Net::ServerInfo serverInfo = info.Server;
        Result<Unique<Net::Server>> server = Net::Server::Create(serverInfo);
        if (!server.has_value())
        {
            return std::unexpected(server.error());
        }
        state->Server = std::move(*server);

        return Unique<ServerHost>(new ServerHost(std::move(state)));
    }

    void ServerHost::AddWorld(const ServerWorldInfo& world)
    {
        m_State->Worlds.insert_or_assign(
            world.WorldId.Value,
            State::HostedWorld{.Id = world.WorldId,
                               .Key = world.Key,
                               .World = &world.World,
                               .LevelId = world.LevelId,
                               .Digest = world.Digest,
                               .SeatPrefab = world.SeatPrefab,
                               .SeatPrefabId = world.SeatPrefabId,
                               .Replication = ReplicationServer(world.Replication),
                               .InterestSettings = world.Interest,
                               .InterestPolicy = world.InterestPolicy});
        // Register the world as a never-reaped bucket of its key; re-adding the same id is idempotent.
        m_State->Directory->Register(world.Key, world.WorldId);
    }

    void ServerHost::Pump(f64 now, u64 tick)
    {
        State& s = *m_State;
        s.Events.clear();

        // Assign wire ids to entities the spawn rule added this tick (the pawns), per world from its
        // own allocator, then generate and queue each ready (connection, join)'s stream from its
        // world's replication instance — each message wrapped in its JoinId envelope so the peer
        // demuxes it to the right world.
        for (auto& [value, world] : s.Worlds)
        {
            AssignServerNetIds(*world.World, world.Allocator);
        }
        for (auto& [id, conn] : s.Connections)
        {
            for (auto& [joinId, join] : conn.Joins)
            {
                if (!join.Ready)
                {
                    continue;
                }
                State::HostedWorld& world = s.WorldOf(join.World);
                const optional<set<NetId>> interest = s.ComputeInterest(world, id, join);
                for (const ReplicationMessage& message : world.Replication.Generate(
                         id, *world.World, tick, interest ? &*interest : nullptr))
                {
                    (void)s.Server->Get(id).Send(message.Channel,
                                                 Net::EncodeWorldEnvelope(joinId, message.Bytes));
                }
            }
        }

        // Receive + connection handshake + flush + reap dead peers.
        s.Server->Pump(now);

        for (const Net::NetEvent& event : s.Server->Events())
        {
            if (event.Type == Net::NetEventType::Connected)
            {
                s.Connections.try_emplace(event.Id);
            }
            else if (event.Type == Net::NetEventType::Disconnected)
            {
                const auto it = s.Connections.find(event.Id);
                if (it != s.Connections.end())
                {
                    for (auto& [joinId, join] : it->second.Joins)
                    {
                        s.ReleaseJoin(event.Id, join, now);
                    }
                    s.Connections.erase(it);
                }
            }
            s.Events.push_back(event);
        }

        // Resolve join requests and fold each per-world ClientReady into its readiness gate. Both ride
        // the world-multiplexing envelope: a join-control tier (ControlJoinId) frame is a join
        // request; a world-tagged frame is world data (the ClientReady), gated on the granted set.
        for (const Net::ConnectionId id : s.Server->Connections())
        {
            for (const vector<u8>& message : s.Server->ReliableAppMessages(id))
            {
                const optional<Net::WorldEnvelope> env = Net::DecodeWorldEnvelope(message);
                if (!env)
                {
                    continue; // short/garbage frame — dropped before any routing
                }
                if (env->Join == Net::ControlJoinId)
                {
                    const optional<Net::JoinMessageType> type = Net::PeekJoinType(env->Payload);
                    if (type == Net::JoinMessageType::JoinRequest)
                    {
                        if (const optional<Net::JoinRequestMessage> request =
                                Net::DecodeJoinRequest(env->Payload))
                        {
                            s.ResolveJoin(id, *request);
                        }
                    }
                    else if (type == Net::JoinMessageType::TravelRequest)
                    {
                        // A client travel request: the server directs the resulting join. In this plan it
                        // echoes the requested key; a server-driven placement would resolve a different
                        // one. The connection leaves its current (presenting) join once the new is ready.
                        if (const optional<Net::TravelRequestMessage> request =
                                Net::DecodeTravelRequest(env->Payload))
                        {
                            const State::JoinState* current = s.CurrentJoinState(id);
                            const Net::JoinId leave =
                                current != nullptr ? current->Join : Net::ControlJoinId;
                            s.SendDirectedTravel(id, leave, request->Key, request->Payload);
                        }
                    }
                    continue;
                }
                // World-tagged data on the reliable channel: only the per-world ClientReady. Gate on
                // the granted set — a tag the connection was never granted is dropped, not routed.
                if (State::JoinState* join = s.JoinStateOf(id, env->Join);
                    join != nullptr && IsClientReady(env->Payload))
                {
                    join->Ready = true;
                }
            }
        }

        s.ReapIdleWorlds(now);
    }

    Net::Server& ServerHost::Server()
    {
        return *m_State->Server;
    }

    ReplicationServer& ServerHost::Replication()
    {
        return m_State->WorldOf(m_State->Primary).Replication;
    }

    ReplicationServer& ServerHost::ReplicationFor(Net::ConnectionId id)
    {
        const State::JoinState* join = m_State->CurrentJoinState(id);
        return m_State->WorldOf(join != nullptr ? join->World : m_State->Primary).Replication;
    }

    ReplicationServer& ServerHost::ReplicationForJoin(Net::ConnectionId id, Net::JoinId join)
    {
        const State::JoinState* state = m_State->JoinStateOf(id, join);
        return m_State->WorldOf(state != nullptr ? state->World : m_State->Primary).Replication;
    }

    ReplicationServer& ServerHost::ReplicationForWorld(WorldInstanceId world)
    {
        return m_State->WorldOf(world).Replication;
    }

    WorldInstanceId ServerHost::WorldFor(Net::ConnectionId id) const
    {
        const State::JoinState* join = m_State->CurrentJoinState(id);
        return join != nullptr ? join->World : WorldInstanceId{};
    }

    WorldInstanceId ServerHost::WorldForJoin(Net::ConnectionId id, Net::JoinId join) const
    {
        const State::JoinState* state = m_State->JoinStateOf(id, join);
        return state != nullptr ? state->World : WorldInstanceId{};
    }

    NetIdAllocator& ServerHost::Allocator()
    {
        return m_State->WorldOf(m_State->Primary).Allocator;
    }

    NetIdAllocator& ServerHost::AllocatorForWorld(WorldInstanceId world)
    {
        return m_State->WorldOf(world).Allocator;
    }

    Entity ServerHost::SeatFor(Net::ConnectionId id) const
    {
        const State::JoinState* join = m_State->CurrentJoinState(id);
        return join != nullptr ? join->Seat : Entity::Null;
    }

    Entity ServerHost::SeatFor(Net::ConnectionId id, Net::JoinId join) const
    {
        const State::JoinState* state = m_State->JoinStateOf(id, join);
        return state != nullptr ? state->Seat : Entity::Null;
    }

    Net::JoinId ServerHost::CurrentJoin(Net::ConnectionId id) const
    {
        const State::JoinState* join = m_State->CurrentJoinState(id);
        return join != nullptr ? join->Join : Net::ControlJoinId;
    }

    vector<Net::JoinId> ServerHost::JoinsFor(Net::ConnectionId id) const
    {
        vector<Net::JoinId> joins;
        const auto it = m_State->Connections.find(id);
        if (it != m_State->Connections.end())
        {
            for (const auto& [joinId, join] : it->second.Joins)
            {
                joins.push_back(joinId);
            }
        }
        return joins;
    }

    bool ServerHost::IsGranted(Net::ConnectionId id, Net::JoinId join) const
    {
        return m_State->JoinStateOf(id, join) != nullptr;
    }

    bool ServerHost::IsReady(Net::ConnectionId id) const
    {
        const State::JoinState* join = m_State->CurrentJoinState(id);
        return join != nullptr && join->Ready;
    }

    bool ServerHost::IsReady(Net::ConnectionId id, Net::JoinId join) const
    {
        const State::JoinState* state = m_State->JoinStateOf(id, join);
        return state != nullptr && state->Ready;
    }

    void ServerHost::DirectTravel(const Net::ConnectionId connection, const Net::JoinId leave,
                                  const Net::WorldKey& key, const Net::TravelPayload& payload)
    {
        m_State->SendDirectedTravel(connection, leave, key, payload);
    }

    usize ServerHost::HostedWorldCount() const
    {
        return m_State->Directory->WorldCount();
    }

    std::span<const Net::NetEvent> ServerHost::Events() const
    {
        return m_State->Events;
    }

    // ---- ClientHost ----------------------------------------------------------------------------

    struct ClientHost::State
    {
        Net::Client* Client = nullptr;
        AssetManager* Assets = nullptr;
        Net::WorldKey AutoJoinKey;
        bool AutoJoin = true;
        bool AutoJoinRequested = false;
        function<Net::ContentDigest(const Net::WorldKey&)> WorldDigest;
        function<Scene*(AssetId)> LoadLevel;
        function<Ref<Prefab>(AssetId)> ResolvePrefab;
        function<void(Scene&, Entity)> OnPossession;
        PredictionPolicy Policy;
        Net::ReplayTick Replay;
        Net::ReconcileTolerances Tolerances;
        Net::TickSyncSettings TickSyncSettings;
        Net::QuantizationSettings Quantization;
        function<Net::QuantizationSettings(const Net::WorldKey&)> WorldQuantization;
        function<void(Net::JoinId)> OnLeaveWorld;
        function<void(const Net::WorldKey&, Net::JoinDenyReason)> OnTravelDenied;

        // One joined world's whole client state — replication, identity map, prediction, and clock,
        // all scoped per JoinId.
        struct JoinClient
        {
            Net::JoinId Join = Net::ControlJoinId;
            Net::TravelPayload
                Payload; // the params the reply echoed, for the game's reconstruction
            Unique<ReplicationClient> Replication;
            Scene* World = nullptr; // borrowed (a WorldRunner world); must outlive the host
            u32 SeatNetId = InvalidNetId;
            Entity Seat = Entity::Null;
            Entity WiredPawn = Entity::Null;
            bool Ready = false;
            Net::PredictionHistory History;
            vector<Entity> Predicted;
            u64 LastServerTick = 0;
            Net::TickOffsetEstimator TickSync;
        };

        // A requested-but-unaccepted join, correlated to its reply by the token. Payload rides the join
        // request; LeaveOnReady names a join to leave once this one is ready (a make-before-break
        // directed travel), or ControlJoinId for an ordinary join.
        struct PendingJoin
        {
            u32 Token = 0;
            Net::WorldKey Key;
            Net::TravelPayload Payload;
            Net::JoinId LeaveOnReady = Net::ControlJoinId;
            bool Sent = false;
        };

        std::map<Net::JoinId, JoinClient> Joins; // ordered, so the first is the current-join
        vector<PendingJoin> Pending;
        u32 NextToken = 1;
        Net::JoinId CurrentJoin = Net::ControlJoinId;

        [[nodiscard]] JoinClient* CurrentJoinClient()
        {
            const auto it = Joins.find(CurrentJoin);
            return it != Joins.end() ? &it->second : nullptr;
        }

        [[nodiscard]] const JoinClient* CurrentJoinClient() const
        {
            const auto it = Joins.find(CurrentJoin);
            return it != Joins.end() ? &it->second : nullptr;
        }

        [[nodiscard]] JoinClient* JoinClientOf(Net::JoinId join)
        {
            const auto it = Joins.find(join);
            return it != Joins.end() ? &it->second : nullptr;
        }

        // Keeps a join's local presentation pointed at its own seat's possessed pawn. The seat's
        // Possesses arrives from the stream and re-resolves as its pawn binds, so this re-checks each
        // Pump and fires OnPossession only when the wired pawn actually changes.
        void WireSeat(JoinClient& jc)
        {
            if (jc.SeatNetId == InvalidNetId || jc.World == nullptr)
            {
                return;
            }
            const Entity seat = jc.Replication->Map().Lookup(jc.SeatNetId);
            if (seat.IsNull() || !jc.World->IsAlive(seat))
            {
                return;
            }
            jc.Seat = seat;

            const Possesses* possesses = jc.World->TryGet<Possesses>(seat);
            if (possesses == nullptr)
            {
                return;
            }
            const Entity pawn = possesses->Pawn;
            // A named pawn that has not yet spawned locally is not wired until it binds.
            if (!pawn.IsNull() && !jc.World->IsAlive(pawn))
            {
                return;
            }
            if (pawn == jc.WiredPawn)
            {
                return;
            }
            Repredict(jc, pawn);
            jc.WiredPawn = pawn;
            if (OnPossession)
            {
                OnPossession(*jc.World, pawn);
            }
        }

        // Swaps a join's predicted set on a possession change: demote the current set back to Remote
        // (interpolated) and stop tracking it, then promote the pawn's new set to Predicted and track
        // it. The recorded history is dropped — its captures reference the superseded set.
        void Repredict(JoinClient& jc, const Entity pawn)
        {
            jc.History.Clear();
            for (const Entity entity : jc.Predicted)
            {
                if (jc.World->IsAlive(entity))
                {
                    if (Authority* authority = jc.World->TryGet<Authority>(entity);
                        authority != nullptr && authority->Tier == Tier::Predicted)
                    {
                        authority->Tier = Tier::Remote;
                    }
                }
                jc.History.Untrack(entity);
            }
            jc.Predicted.clear();

            if (pawn.IsNull() || !jc.World->IsAlive(pawn))
            {
                return;
            }
            jc.Predicted = Policy ? Policy(*jc.World, pawn) : DefaultPredictionSet(*jc.World, pawn);
            for (const Entity entity : jc.Predicted)
            {
                if (!jc.World->IsAlive(entity))
                {
                    continue;
                }
                if (jc.World->Has<Authority>(entity))
                {
                    jc.World->Get<Authority>(entity).Tier = Tier::Predicted;
                }
                else
                {
                    jc.World->Add<Authority>(entity, Authority{.Tier = Tier::Predicted});
                }
                jc.History.Track(entity);
            }
        }

        // Leaves a join: drops its stream and state and notifies the caller (who frees its scene).
        void LeaveJoin(Net::JoinId join)
        {
            const auto it = Joins.find(join);
            if (it == Joins.end())
            {
                return;
            }
            Joins.erase(it);
            if (CurrentJoin == join)
            {
                CurrentJoin = Joins.empty() ? Net::ControlJoinId : Joins.begin()->first;
            }
            if (OnLeaveWorld)
            {
                OnLeaveWorld(join);
            }
        }

        // Validates the reply's echoed world digest against the client's own reconstruction, loads the
        // join's scene, and — on success — installs the JoinClient and acks its per-world ClientReady.
        // A digest mismatch is rejected loudly: no JoinClient is installed, so no stream ever applies.
        // Once installed (the make-before-break "ready"), a directed travel leaves its departed join.
        void HandleJoinAccept(const Net::JoinAcceptMessage& accept)
        {
            const auto pending = std::ranges::find_if(Pending, [&](const PendingJoin& p)
                                                      { return p.Token == accept.RequestToken; });
            if (pending == Pending.end())
            {
                return; // unknown or duplicate reply
            }
            const Net::WorldKey key = pending->Key;
            const Net::JoinId leaveOnReady = pending->LeaveOnReady;
            Pending.erase(pending);

            if (Joins.contains(accept.Join))
            {
                return; // already installed (a resent accept)
            }

            const AssetId levelId{.Value = accept.LevelId};
            const Net::ContentDigest expected =
                WorldDigest ? WorldDigest(key) : Net::ContentDigest{};
            if (!(expected == accept.WorldDigest))
            {
                Log::Error(
                    "ClientHost rejecting join {}: world digest mismatch (server {:016X}{:016X}"
                    ", client {:016X}{:016X})",
                    accept.Join, accept.WorldDigest.Hi, accept.WorldDigest.Lo, expected.Hi,
                    expected.Lo);
                return;
            }

            Scene* scene = LoadLevel ? LoadLevel(levelId) : nullptr;
            if (scene == nullptr)
            {
                return; // load failed — no join installed
            }

            JoinClient jc;
            jc.Join = accept.Join;
            jc.Payload = accept.Payload;
            jc.SeatNetId = accept.SeatNetId;
            jc.World = scene;
            jc.TickSync = Net::TickOffsetEstimator(TickSyncSettings);
            jc.Replication = CreateUnique<ReplicationClient>(ResolvePrefab);
            // The per-key spatial envelope: WorldQuantization, when set, yields this key's grid so two
            // hosted worlds with different envelopes each decode correctly on one client; unset threads
            // the shared grid onto every join.
            jc.Replication->SetQuantization(WorldQuantization ? WorldQuantization(key)
                                                              : Quantization);
            jc.Ready = true;
            Joins.emplace(accept.Join, std::move(jc));
            if (CurrentJoin == Net::ControlJoinId)
            {
                CurrentJoin = accept.Join;
            }

            // Ack this world's ClientReady, enveloped with its JoinId, opening its stream.
            (void)Client->Server().Send(Net::Channel::ReliableOrdered,
                                        Net::EncodeWorldEnvelope(accept.Join, EncodeClientReady()));

            // Make-before-break: the destination is ready, so leave the departed join now — the old
            // world stayed live until this moment, and a denied join would have skipped this entirely.
            if (leaveOnReady != Net::ControlJoinId)
            {
                LeaveJoin(leaveOnReady);
            }
        }
    };

    ClientHost::ClientHost(Unique<State> state) : m_State(std::move(state)) {}

    ClientHost::~ClientHost() = default;

    Unique<ClientHost> ClientHost::Create(const ClientHostInfo& info)
    {
        auto state = CreateUnique<State>();
        state->Client = &info.Client;
        state->Assets = &info.Assets;
        state->AutoJoinKey = info.WorldKey;
        state->AutoJoin = info.AutoJoin;
        state->WorldDigest = info.WorldDigest;
        state->LoadLevel = info.LoadLevel;
        state->ResolvePrefab = info.ResolvePrefab;
        state->OnPossession = info.OnPossession;
        state->Policy = info.Prediction;
        state->Replay = info.Replay;
        state->Tolerances = info.Tolerances;
        state->TickSyncSettings = info.TickSync;
        state->Quantization = info.Quantization;
        state->WorldQuantization = info.WorldQuantization;
        state->OnLeaveWorld = info.OnLeaveWorld;
        state->OnTravelDenied = info.OnTravelDenied;
        return Unique<ClientHost>(new ClientHost(std::move(state)));
    }

    void ClientHost::Join(const Net::WorldKey& key, const Net::TravelPayload& payload)
    {
        State& s = *m_State;
        const u32 token = s.NextToken;
        s.NextToken += 1;
        s.Pending.push_back(State::PendingJoin{.Token = token, .Key = key, .Payload = payload});
    }

    void ClientHost::Travel(const Net::WorldKey& key, const Net::TravelPayload& payload)
    {
        State& s = *m_State;
        const vector<u8> message =
            Net::EncodeTravelRequest(Net::TravelRequestMessage{.Key = key, .Payload = payload});
        (void)s.Client->Server().Send(Net::Channel::ReliableOrdered,
                                      Net::EncodeWorldEnvelope(Net::ControlJoinId, message));
    }

    void ClientHost::Leave(const Net::JoinId join)
    {
        m_State->LeaveJoin(join);
    }

    void ClientHost::Pump(f64 now)
    {
        State& s = *m_State;
        s.Client->Pump(now);

        if (s.Client->State() != Net::ClientState::Connected)
        {
            return;
        }

        // Auto-join the configured world once, the moment the connection is up.
        if (s.AutoJoin && !s.AutoJoinRequested)
        {
            Join(s.AutoJoinKey);
            s.AutoJoinRequested = true;
        }

        // Send any not-yet-sent join requests, enveloped at the join-control tier.
        for (State::PendingJoin& pending : s.Pending)
        {
            if (!pending.Sent)
            {
                const vector<u8> payload = Net::EncodeJoinRequest(Net::JoinRequestMessage{
                    .Key = pending.Key, .RequestToken = pending.Token, .Payload = pending.Payload});
                (void)s.Client->Server().Send(
                    Net::Channel::ReliableOrdered,
                    Net::EncodeWorldEnvelope(Net::ControlJoinId, payload));
                pending.Sent = true;
            }
        }

        // Drain the reliable channel: a join-control frame is a join reply; a world-tagged frame is
        // that world's spawn/despawn stream, demuxed to its ReplicationClient (dropped if ungranted).
        for (const vector<u8>& message : s.Client->ReliableAppMessages())
        {
            const optional<Net::WorldEnvelope> env = Net::DecodeWorldEnvelope(message);
            if (!env)
            {
                continue;
            }
            if (env->Join == Net::ControlJoinId)
            {
                const optional<Net::JoinMessageType> type = Net::PeekJoinType(env->Payload);
                if (type == Net::JoinMessageType::JoinAccept)
                {
                    if (const optional<Net::JoinAcceptMessage> accept =
                            Net::DecodeJoinAccept(env->Payload))
                    {
                        s.HandleJoinAccept(*accept);
                    }
                }
                else if (type == Net::JoinMessageType::JoinDeny)
                {
                    if (const optional<Net::JoinDenyMessage> deny =
                            Net::DecodeJoinDeny(env->Payload))
                    {
                        // A denied make-before-break join leaves the client where it was (the departed
                        // join is never left); surface the reason so a caller can snap back and report.
                        const auto pending =
                            std::ranges::find_if(s.Pending, [&](const State::PendingJoin& p)
                                                 { return p.Token == deny->RequestToken; });
                        if (pending != s.Pending.end())
                        {
                            if (pending->LeaveOnReady != Net::ControlJoinId && s.OnTravelDenied)
                            {
                                s.OnTravelDenied(pending->Key, deny->Reason);
                            }
                            s.Pending.erase(pending);
                        }
                        Log::Warn("ClientHost join denied: reason {}",
                                  static_cast<u32>(deny->Reason));
                    }
                }
                else if (type == Net::JoinMessageType::DirectedTravel)
                {
                    // The server directs a travel: join the named world (carrying the payload) and, once
                    // that join is ready, leave the departed one — make-before-break, resolved in
                    // HandleJoinAccept. The request sends next Pump like any queued join.
                    if (const optional<Net::DirectedTravelMessage> directed =
                            Net::DecodeDirectedTravel(env->Payload))
                    {
                        const u32 token = s.NextToken;
                        s.NextToken += 1;
                        s.Pending.push_back(State::PendingJoin{.Token = token,
                                                               .Key = directed->Join,
                                                               .Payload = directed->Payload,
                                                               .LeaveOnReady = directed->Leave});
                    }
                }
                continue;
            }
            State::JoinClient* jc = s.JoinClientOf(env->Join);
            if (jc != nullptr && jc->World != nullptr)
            {
                jc->Replication->ApplyReliable(env->Payload, *jc->World, *s.Assets);
            }
        }

        // Drain the unreliable channel: each snapshot is demuxed by its JoinId to the right world's
        // ReplicationClient, reconciled against that world's own prediction and clock.
        while (const optional<vector<u8>> packet =
                   s.Client->Server().Receive(Net::Channel::UnreliableSequenced))
        {
            const optional<Net::WorldEnvelope> env = Net::DecodeWorldEnvelope(*packet);
            if (!env || env->Join == Net::ControlJoinId)
            {
                continue;
            }
            State::JoinClient* jc = s.JoinClientOf(env->Join);
            if (jc == nullptr || jc->World == nullptr)
            {
                continue;
            }
            const SnapshotApplyResult applied =
                jc->Replication->ApplySnapshot(env->Payload, *jc->World);
            if (applied.HeaderValid && applied.ServerTick > jc->LastServerTick)
            {
                jc->LastServerTick = applied.ServerTick;
                jc->TickSync.SetFeedbackTrim(static_cast<f32>(applied.InputFeedback));
                if (!jc->History.Tracked().empty() && applied.LastConsumedInputTick > 0)
                {
                    (void)Net::Reconcile(*jc->World, jc->History,
                                         jc->Replication->PredictedRecords(),
                                         applied.LastConsumedInputTick, s.Replay, s.Tolerances);
                }
            }
        }

        for (auto& [joinId, jc] : s.Joins)
        {
            s.WireSeat(jc);
        }
    }

    vector<Net::JoinId> ClientHost::Joins() const
    {
        vector<Net::JoinId> joins;
        for (const auto& [joinId, jc] : m_State->Joins)
        {
            joins.push_back(joinId);
        }
        return joins;
    }

    Net::JoinId ClientHost::CurrentJoinId() const
    {
        return m_State->CurrentJoin;
    }

    Scene* ClientHost::World() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        return jc != nullptr ? jc->World : nullptr;
    }

    Scene* ClientHost::World(Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->World : nullptr;
    }

    ReplicationClient& ClientHost::Replication()
    {
        State::JoinClient* jc = m_State->CurrentJoinClient();
        VE_ASSERT(jc != nullptr, "ClientHost::Replication() before any world joined");
        return *jc->Replication;
    }

    ReplicationClient& ClientHost::Replication(Net::JoinId join)
    {
        State::JoinClient* jc = m_State->JoinClientOf(join);
        VE_ASSERT(jc != nullptr, "ClientHost::Replication called with unknown join {}", join);
        return *jc->Replication;
    }

    Entity ClientHost::Seat() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        return jc != nullptr ? jc->Seat : Entity::Null;
    }

    Entity ClientHost::Seat(Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->Seat : Entity::Null;
    }

    Entity ClientHost::PossessedPawn() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        return jc != nullptr ? jc->WiredPawn : Entity::Null;
    }

    const Net::TravelPayload& ClientHost::JoinPayload(const Net::JoinId join) const
    {
        static const Net::TravelPayload empty;
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->Payload : empty;
    }

    Net::PredictionHistory& ClientHost::History()
    {
        State::JoinClient* jc = m_State->CurrentJoinClient();
        VE_ASSERT(jc != nullptr, "ClientHost::History() before any world joined");
        return jc->History;
    }

    const Net::PredictionHistory& ClientHost::History() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        VE_ASSERT(jc != nullptr, "ClientHost::History() before any world joined");
        return jc->History;
    }

    Net::PredictionHistory& ClientHost::History(Net::JoinId join)
    {
        State::JoinClient* jc = m_State->JoinClientOf(join);
        VE_ASSERT(jc != nullptr, "ClientHost::History called with unknown join {}", join);
        return jc->History;
    }

    void ClientHost::RecordPrediction(const u64 tick)
    {
        RecordPrediction(m_State->CurrentJoin, tick);
    }

    void ClientHost::RecordPrediction(const Net::JoinId join, const u64 tick)
    {
        State::JoinClient* jc = m_State->JoinClientOf(join);
        if (jc == nullptr || jc->World == nullptr || jc->History.Tracked().empty())
        {
            return;
        }
        const PlayerInput* input = FindLocalSeatInput(*jc->World);
        jc->History.Record(tick, input != nullptr ? *input : PlayerInput{}, *jc->World);
    }

    bool ClientHost::IsJoined() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        return jc != nullptr && jc->Ready;
    }

    bool ClientHost::IsJoined(Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr && jc->Ready;
    }

    u64 ClientHost::LastServerTick() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        return jc != nullptr ? jc->LastServerTick : 0;
    }

    u64 ClientHost::LastServerTick(Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->LastServerTick : 0;
    }

    const Net::TickOffsetEstimator& ClientHost::TickSync() const
    {
        const State::JoinClient* jc = m_State->CurrentJoinClient();
        VE_ASSERT(jc != nullptr, "ClientHost::TickSync() before any world joined");
        return jc->TickSync;
    }

    const Net::TickOffsetEstimator& ClientHost::TickSync(Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        VE_ASSERT(jc != nullptr, "ClientHost::TickSync called with unknown join {}", join);
        return jc->TickSync;
    }

    f32 ClientHost::ObserveTickSync(const u64 clientTick)
    {
        return ObserveTickSync(m_State->CurrentJoin, clientTick);
    }

    f32 ClientHost::ObserveTickSync(const Net::JoinId join, const u64 clientTick)
    {
        State& s = *m_State;
        State::JoinClient* jc = s.JoinClientOf(join);
        if (jc == nullptr || s.Client->State() != Net::ClientState::Connected ||
            jc->LastServerTick == 0)
        {
            return 1.0f;
        }
        return jc->TickSync.Observe(s.Client->Server().RttEstimate(), clientTick,
                                    jc->LastServerTick);
    }

    void ClientHost::SetTickSyncFeedback(const f32 trimTicks)
    {
        if (State::JoinClient* jc = m_State->CurrentJoinClient())
        {
            jc->TickSync.SetFeedbackTrim(trimTicks);
        }
    }
}

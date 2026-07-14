#include <Veng/Net/Host.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

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

        // The client→server readiness message: a single-byte reliable frame the Server surfaces as an
        // app reliable message (its id sits clear of the ControlMessageType 1–4 and the replication
        // Spawn/Despawn 16/17 ranges, so no layer mistakes it for its own).
        constexpr u8 ClientReadyMessageId = 32;

        vector<u8> EncodeClientReady()
        {
            return vector<u8>{ClientReadyMessageId};
        }

        bool IsClientReady(std::span<const u8> message)
        {
            return message.size() == 1 && message[0] == ClientReadyMessageId;
        }
    }

    // ---- ServerHost ----------------------------------------------------------------------------

    struct ServerHost::State
    {
        // One hosted world: its scene, seat rule, and its own replication instance and wire-id space.
        // The replication and allocator are per world, so a world's baselines and NetIds never cross
        // into a peer's.
        struct HostedWorld
        {
            WorldInstanceId Id;
            Scene* World = nullptr;
            AssetId LevelId;
            Ref<Prefab> SeatPrefab;
            AssetId SeatPrefabId;
            ReplicationServer Replication;
            NetIdAllocator Allocator;
            Net::InterestSettings InterestSettings;
            Net::InterestPolicy InterestPolicy;
        };

        // One connection's world binding, seat, readiness gate, and interest bookkeeping.
        struct SeatState
        {
            WorldInstanceId World;
            Entity Seat = Entity::Null;
            bool Ready = false;
            Net::InterestState Interest;
        };

        AssetManager* Assets = nullptr;
        Unique<Net::Server> Server;

        // The hosted worlds keyed by WorldInstanceId value; the primary is the default routing target
        // and what the no-arg accessors resolve. SelectWorld routes an accepted connection to one of
        // these; unset, every connection binds to the primary.
        std::unordered_map<u64, HostedWorld> Worlds;
        WorldInstanceId Primary;
        function<WorldInstanceId(Net::ConnectionId)> SelectWorld;

        std::unordered_map<Net::ConnectionId, SeatState> Connections;
        vector<Net::NetEvent> Events;

        HostedWorld& WorldOf(WorldInstanceId id)
        {
            const auto it = Worlds.find(id.Value);
            VE_ASSERT(it != Worlds.end(), "no hosted world for id {}", id.Value);
            return it->second;
        }

        // The connection's interest set this snapshot: the spatial query around its pawn ∪ the
        // always-relevant entities ∪ the policy hook ∪ the entities it owns. Empty optional when
        // interest is off (Radius 0), so Generate replicates the whole world.
        optional<set<NetId>> ComputeInterest(HostedWorld& world, Net::ConnectionId id,
                                             SeatState& seat)
        {
            Scene& scene = *world.World;
            if (world.InterestSettings.Radius <= 0.0f)
            {
                return std::nullopt;
            }

            const Entity seatEntity = seat.Seat;
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
                                       seat.Interest);
        }

        // Spawns a seat into the connection's world: a Viewer seat with Authority{ Server, Owner = id }
        // and no SeatInput (its input arrives from the wire). Returns the seat's freshly assigned wire
        // id, drawn from that world's allocator.
        u32 SpawnSeat(HostedWorld& world, Net::ConnectionId id)
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

            Connections[id] = SeatState{.World = world.Id, .Seat = seat, .Ready = false};
            return netId;
        }

        Net::AcceptPayload OnAccept(Net::ConnectionId id)
        {
            const WorldInstanceId target = SelectWorld ? SelectWorld(id) : Primary;
            HostedWorld& world = WorldOf(target);
            const u32 seatNetId = SpawnSeat(world, id);
            world.Replication.AddConnection(id);
            if (world.SeatPrefabId.IsValid())
            {
                world.Replication.SetEntityPrefab(seatNetId, world.SeatPrefabId);
            }
            return Net::AcceptPayload{.LevelId = world.LevelId.Value, .SeatNetId = seatNetId};
        }
    };

    ServerHost::ServerHost(Unique<State> state) : m_State(std::move(state)) {}

    ServerHost::~ServerHost() = default;

    Result<Unique<ServerHost>> ServerHost::Create(const ServerHostInfo& info)
    {
        auto state = CreateUnique<State>();
        state->Assets = &info.Assets;
        state->Primary = info.WorldId;
        state->SelectWorld = info.SelectWorld;
        state->Worlds.emplace(info.WorldId.Value,
                              State::HostedWorld{.Id = info.WorldId,
                                                 .World = &info.World,
                                                 .LevelId = info.LevelId,
                                                 .SeatPrefab = info.SeatPrefab,
                                                 .SeatPrefabId = info.SeatPrefabId,
                                                 .Replication = ReplicationServer(info.Replication),
                                                 .InterestSettings = info.Interest,
                                                 .InterestPolicy = info.InterestPolicy});

        State* raw = state.get();
        Net::ServerInfo serverInfo = info.Server;
        serverInfo.OnAccept = [raw](Net::ConnectionId id) { return raw->OnAccept(id); };

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
                               .World = &world.World,
                               .LevelId = world.LevelId,
                               .SeatPrefab = world.SeatPrefab,
                               .SeatPrefabId = world.SeatPrefabId,
                               .Replication = ReplicationServer(world.Replication),
                               .InterestSettings = world.Interest,
                               .InterestPolicy = world.InterestPolicy});
    }

    void ServerHost::Pump(f64 now, u64 tick)
    {
        State& s = *m_State;
        s.Events.clear();

        // Assign wire ids to entities the spawn rule added this tick (the pawns), per world from its
        // own allocator, then generate and queue each ready connection's stream from its world's
        // replication instance — the Pump below flushes the queued sends.
        for (auto& [value, world] : s.Worlds)
        {
            AssignServerNetIds(*world.World, world.Allocator);
        }
        for (auto& [id, seat] : s.Connections)
        {
            if (!seat.Ready)
            {
                continue;
            }
            State::HostedWorld& world = s.WorldOf(seat.World);
            const optional<set<NetId>> interest = s.ComputeInterest(world, id, seat);
            for (const ReplicationMessage& message : world.Replication.Generate(
                     id, *world.World, tick, interest ? &*interest : nullptr))
            {
                (void)s.Server->Get(id).Send(message.Channel, message.Bytes);
            }
        }

        // Receive + handshake (OnAccept spawns seats) + flush + reap.
        s.Server->Pump(now);

        // Tear down a disconnected connection's seat in its world; surface every lifecycle event.
        for (const Net::NetEvent& event : s.Server->Events())
        {
            if (event.Type == Net::NetEventType::Disconnected)
            {
                const auto it = s.Connections.find(event.Id);
                if (it != s.Connections.end())
                {
                    State::HostedWorld& world = s.WorldOf(it->second.World);
                    const Entity seat = it->second.Seat;
                    if (!seat.IsNull() && world.World->IsAlive(seat))
                    {
                        world.World->DestroyEntity(seat);
                    }
                    world.Replication.RemoveConnection(event.Id);
                    s.Connections.erase(it);
                }
            }
            s.Events.push_back(event);
        }

        // A ClientReady opens a connection's stream.
        for (const Net::ConnectionId id : s.Server->Connections())
        {
            for (const vector<u8>& message : s.Server->ReliableAppMessages(id))
            {
                if (IsClientReady(message))
                {
                    if (const auto it = s.Connections.find(id); it != s.Connections.end())
                    {
                        it->second.Ready = true;
                    }
                }
            }
        }
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
        const auto it = m_State->Connections.find(id);
        const WorldInstanceId world =
            it != m_State->Connections.end() ? it->second.World : m_State->Primary;
        return m_State->WorldOf(world).Replication;
    }

    ReplicationServer& ServerHost::ReplicationForWorld(WorldInstanceId world)
    {
        return m_State->WorldOf(world).Replication;
    }

    WorldInstanceId ServerHost::WorldFor(Net::ConnectionId id) const
    {
        const auto it = m_State->Connections.find(id);
        return it != m_State->Connections.end() ? it->second.World : WorldInstanceId{};
    }

    NetIdAllocator& ServerHost::Allocator()
    {
        return m_State->WorldOf(m_State->Primary).Allocator;
    }

    Entity ServerHost::SeatFor(Net::ConnectionId id) const
    {
        const auto it = m_State->Connections.find(id);
        return it != m_State->Connections.end() ? it->second.Seat : Entity::Null;
    }

    bool ServerHost::IsReady(Net::ConnectionId id) const
    {
        const auto it = m_State->Connections.find(id);
        return it != m_State->Connections.end() && it->second.Ready;
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
        function<Scene*(AssetId)> LoadLevel;
        function<void(Scene&, Entity)> OnPossession;
        PredictionPolicy Policy;
        Net::ReplayTick Replay;
        Net::ReconcileTolerances Tolerances;
        Unique<ReplicationClient> Replication;

        // Borrowed: the caller owns the join scene (a WorldRunner world); the host applies the stream
        // into it and never destroys it.
        Scene* World = nullptr;
        u32 SeatNetId = InvalidNetId;
        Entity Seat = Entity::Null;
        Entity WiredPawn = Entity::Null;
        bool Joined = false;

        // The client-side prediction ring and the entities currently promoted to Tier::Predicted (the
        // possessed pawn plus the policy's subtree). The set is swapped on every possession change.
        Net::PredictionHistory History;
        vector<Entity> Predicted;

        // The tick-offset controller: the freshest server tick a snapshot carried and the estimator
        // that folds it (with the connection's RTT) into the client's target lead. The world drive
        // reads the target each frame to seed and slew its sim clock ahead of the server.
        u64 LastServerTick = 0;
        Net::TickOffsetEstimator TickSync;

        // Keeps the local presentation pointed at the own seat's possessed pawn. The seat's Possesses
        // arrives from the stream and re-resolves as its pawn binds, so this re-checks each Pump and
        // fires OnPossession only when the wired pawn actually changes.
        void WireSeat()
        {
            if (SeatNetId == InvalidNetId || !World)
            {
                return;
            }
            const Entity seat = Replication->Map().Lookup(SeatNetId);
            if (seat.IsNull() || !World->IsAlive(seat))
            {
                return;
            }
            Seat = seat;

            const Possesses* possesses = World->TryGet<Possesses>(seat);
            if (possesses == nullptr)
            {
                return;
            }
            const Entity pawn = possesses->Pawn;
            // A named pawn that has not yet spawned locally is not wired until it binds.
            if (!pawn.IsNull() && !World->IsAlive(pawn))
            {
                return;
            }
            if (pawn == WiredPawn)
            {
                return;
            }
            Repredict(pawn);
            WiredPawn = pawn;
            if (OnPossession)
            {
                OnPossession(*World, pawn);
            }
        }

        // Swaps the predicted set on a possession change: demote the current set back to Remote
        // (interpolated) and stop tracking it, then promote the pawn's new set to Predicted and track
        // it. The recorded history is dropped — its captures reference the superseded set.
        void Repredict(const Entity pawn)
        {
            History.Clear();
            for (const Entity entity : Predicted)
            {
                if (World->IsAlive(entity))
                {
                    if (Authority* authority = World->TryGet<Authority>(entity);
                        authority != nullptr && authority->Tier == Tier::Predicted)
                    {
                        authority->Tier = Tier::Remote;
                    }
                }
                History.Untrack(entity);
            }
            Predicted.clear();

            if (pawn.IsNull() || !World->IsAlive(pawn))
            {
                return;
            }
            Predicted = Policy ? Policy(*World, pawn) : DefaultPredictionSet(*World, pawn);
            for (const Entity entity : Predicted)
            {
                if (!World->IsAlive(entity))
                {
                    continue;
                }
                if (World->Has<Authority>(entity))
                {
                    World->Get<Authority>(entity).Tier = Tier::Predicted;
                }
                else
                {
                    World->Add<Authority>(entity, Authority{.Tier = Tier::Predicted});
                }
                History.Track(entity);
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
        state->LoadLevel = info.LoadLevel;
        state->OnPossession = info.OnPossession;
        state->Policy = info.Prediction;
        state->Replay = info.Replay;
        state->Tolerances = info.Tolerances;
        state->TickSync = Net::TickOffsetEstimator(info.TickSync);
        state->Replication = CreateUnique<ReplicationClient>(info.ResolvePrefab);
        return Unique<ClientHost>(new ClientHost(std::move(state)));
    }

    void ClientHost::Pump(f64 now)
    {
        State& s = *m_State;
        s.Client->Pump(now);

        if (s.Client->State() != Net::ClientState::Connected)
        {
            return;
        }

        // On the accept: load the level (server-authoritative entities skipped) and ack readiness.
        if (!s.World)
        {
            s.SeatNetId = s.Client->SeatNetId();
            s.World = s.LoadLevel(s.Client->LevelId());
            (void)s.Client->Server().Send(Net::Channel::ReliableOrdered, EncodeClientReady());
            s.Joined = true;
        }
        if (!s.World)
        {
            return;
        }

        // The baseline spawn stream + steady-state spawn/despawn (reliable), then snapshots (unreliable).
        for (const vector<u8>& message : s.Client->ReliableAppMessages())
        {
            s.Replication->ApplyReliable(message, *s.World, *s.Assets);
        }
        while (const optional<vector<u8>> snapshot =
                   s.Client->Server().Receive(Net::Channel::UnreliableSequenced))
        {
            const SnapshotApplyResult applied = s.Replication->ApplySnapshot(*snapshot, *s.World);
            if (applied.HeaderValid && applied.ServerTick > s.LastServerTick)
            {
                s.LastServerTick = applied.ServerTick;
                // The newest snapshot's feedback closes the tick-offset loop: the controller trims
                // its target lead by how early/late the server saw this client's input running.
                s.TickSync.SetFeedbackTrim(static_cast<f32>(applied.InputFeedback));

                // Reconcile the predicted set against this snapshot's authoritative record: compare
                // at the consumed-input tick, and on a mismatch restore + replay + smooth. A no-op
                // when nothing is predicted, or before the server has confirmed any input (client
                // ticks start at 1, so a zero consumed-input tick means "nothing to reconcile yet").
                if (!s.History.Tracked().empty() && applied.LastConsumedInputTick > 0)
                {
                    (void)Net::Reconcile(*s.World, s.History, s.Replication->PredictedRecords(),
                                         applied.LastConsumedInputTick, s.Replay, s.Tolerances);
                }
            }
        }

        s.WireSeat();
    }

    Scene* ClientHost::World() const
    {
        return m_State->World;
    }

    ReplicationClient& ClientHost::Replication()
    {
        return *m_State->Replication;
    }

    Entity ClientHost::Seat() const
    {
        return m_State->Seat;
    }

    Entity ClientHost::PossessedPawn() const
    {
        return m_State->WiredPawn;
    }

    Net::PredictionHistory& ClientHost::History()
    {
        return m_State->History;
    }

    const Net::PredictionHistory& ClientHost::History() const
    {
        return m_State->History;
    }

    void ClientHost::RecordPrediction(const u64 tick)
    {
        State& s = *m_State;
        if (!s.World || s.History.Tracked().empty())
        {
            return;
        }
        // Record the local input seat's resolved input — the same snapshot StampLocalSeatInput sends —
        // alongside the predicted set's post-movement state, so a replay re-runs the server's input.
        const PlayerInput* input = FindLocalSeatInput(*s.World);
        s.History.Record(tick, input != nullptr ? *input : PlayerInput{}, *s.World);
    }

    bool ClientHost::IsJoined() const
    {
        return m_State->Joined;
    }

    u64 ClientHost::LastServerTick() const
    {
        return m_State->LastServerTick;
    }

    const Net::TickOffsetEstimator& ClientHost::TickSync() const
    {
        return m_State->TickSync;
    }

    f32 ClientHost::ObserveTickSync(const u64 clientTick)
    {
        State& s = *m_State;
        if (s.Client->State() != Net::ClientState::Connected || s.LastServerTick == 0)
        {
            return 1.0f;
        }
        return s.TickSync.Observe(s.Client->Server().RttEstimate(), clientTick, s.LastServerTick);
    }

    void ClientHost::SetTickSyncFeedback(const f32 trimTicks)
    {
        m_State->TickSync.SetFeedbackTrim(trimTicks);
    }
}

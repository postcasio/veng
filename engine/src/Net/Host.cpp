#include <Veng/Net/Host.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Log.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/Components.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include "Handshake.h"
#include <Veng/Net/WorldEnvelope.h>

#include <algorithm>
#include <deque>
#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Veng
{
    namespace
    {
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
            u32 SimTickRate = 60;
            Net::ContentDigest Digest;
            Ref<Prefab> SeatPrefab;
            AssetId SeatPrefabId;
            ReplicationServer Replication;
            NetIdAllocator Allocator;
            Net::InterestSettings InterestSettings;
            Net::InterestPolicy InterestPolicy;
            // Total replication bytes emitted for this world (snapshots + spawns/despawns), summed
            // across every connection over the host's lifetime — the per-world traffic instrument.
            u64 ReplicationBytes = 0;
        };

        // One (connection, join): the joined world, the seat spawned in it, the readiness gate, and
        // the per-join interest bookkeeping.
        struct JoinState
        {
            Net::JoinId Join = Net::ControlJoinId;
            WorldInstanceId World;
            Net::WorldKey Key;
            Entity Seat = Entity::Null;
            bool Ready = false;
            // How this join entered the account's session record, so its leave (a standing removal)
            // and the disconnect pose capture (the gameplay join's seat) resolve without a re-lookup.
            Net::SessionDurability Durability = Net::SessionDurability::None;
            Net::InterestState Interest;
        };

        // One connection's admitted account, its joins, its per-connection JoinId allocator, and the
        // WorldKey → JoinId dedupe so a repeat join of the same key is idempotent.
        struct ConnectionState
        {
            Net::AccountId Account;                 // bound at admission, stable for the connection
            std::map<Net::JoinId, JoinState> Joins; // ordered, so the first is the current-join
            std::unordered_map<Net::WorldKey, Net::JoinId> KeyToJoin;
            Net::JoinId NextJoin = 1; // monotonic per connection, never reused, one reserved
            // The gameplay entry a reattach directed the connection back to: when its join request
            // for that key lands, the record is refreshed from here (Params and the captured Pose),
            // not from the request payload — the round trip must not clobber the captured pose.
            optional<Net::SessionGameplayEntry> PendingReattachGameplay;
            // Queued outbound game messages (already framed), flushed into the reliable stream at
            // Pump; the send-time cap bounds them so a caller outrunning the pump fails loudly.
            std::deque<vector<u8>> OutboundMessages;
            usize OutboundBytes = 0;
        };

        // One inbound game message queued for the frame-safe delivery point.
        struct InboundMessage
        {
            Net::ConnectionId From = Net::ServerConnectionId;
            Net::ChannelId Channel = Net::InvalidChannelId;
            Net::Blob Payload;
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

        // The host-tier session registry: owned (built from the ServerHostInfo session hooks) unless
        // the caller borrowed a shared one. Sessions always points at the live one.
        Unique<Net::SessionRegistry> OwnedSessions;
        Net::SessionRegistry* Sessions = nullptr;

        std::unordered_map<Net::ConnectionId, ConnectionState> Connections;
        vector<Net::NetEvent> Events;

        // The game message channel: registered handlers, the frame-safe inbound queue, the one-shot
        // unregistered-channel warnings, and the local account loopback sends resolve to.
        std::unordered_map<Net::ChannelId, function<void(Net::ConnectionId, const Net::Blob&)>>
            Channels;
        vector<InboundMessage> Inbound;
        std::unordered_set<Net::ChannelId> UnregisteredWarned;
        Net::AccountId LocalAccount;

        // One admitted account's presented profile, plus the connection that presented it. The
        // owner is what makes an overlapping reconnect safe: a teardown clears the entry only while
        // the departing connection still owns it, so a fresh connection's profile survives a stale
        // connection's reap. The local account's entry is owned by ServerConnectionId, which no
        // connection ever holds, so nothing clears it.
        struct ProfileEntry
        {
            Net::ConnectionId Owner = Net::ServerConnectionId;
            Net::Blob Profile;
        };

        std::unordered_map<Net::AccountId, ProfileEntry> Profiles;

        [[nodiscard]] const Net::Blob* ProfileOf(const Net::AccountId& account) const
        {
            const auto it = Profiles.find(account);
            return it != Profiles.end() ? &it->second.Profile : nullptr;
        }

        // Binds an admitted connection's profile to its account; an empty profile ("none
        // presented") drops any entry the account held, so absence has one spelling.
        void BindProfile(Net::ConnectionId id, const Net::AccountId& account,
                         const Net::Blob* profile)
        {
            if (!account.IsValid())
            {
                return;
            }
            if (profile == nullptr || profile->Bytes.empty())
            {
                Profiles.erase(account);
                return;
            }
            Profiles[account] = ProfileEntry{.Owner = id, .Profile = *profile};
        }

        void ReleaseProfile(Net::ConnectionId id, const Net::AccountId& account)
        {
            const auto it = Profiles.find(account);
            if (it != Profiles.end() && it->second.Owner == id)
            {
                Profiles.erase(it);
            }
        }

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
        // SeatInput (its input arrives from the wire), stamped with the connection's account
        // (SeatAccount — a non-replicated builtin, so the id stays server-local). Returns the seat's
        // freshly assigned wire id, drawn from that world's allocator.
        u32 SpawnSeat(HostedWorld& world, Net::ConnectionId id, const Net::AccountId& account,
                      Entity& outSeat)
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
                (void)scene.Remove<SeatInput>(seat);
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

            const SeatAccount seatAccount{.Account = account};
            if (scene.Has<SeatAccount>(seat))
            {
                scene.Get<SeatAccount>(seat) = seatAccount;
            }
            else
            {
                scene.Add<SeatAccount>(seat, seatAccount);
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

        // Associates each marked, provenance-carrying, host-authoritative entity in a hosted world
        // with the prefab it was spawned from, so its Spawn rides as a prefab id and a joiner
        // instantiates it through the ordinary prefab path instead of receiving its bare replicated
        // leaves. Re-asserted every pump over the marked set only (never a scan of the scene), which
        // is what picks up an entity marked before the world had any join to replicate to.
        void AssociateMarkedSpawns(HostedWorld& world)
        {
            const Scene& scene = *world.World;
            for (auto [entity, source, identity, mark] :
                 scene.View<PrefabSource, NetIdentity, NetSpawn>())
            {
                if (identity.Id == InvalidNetId || !source.Prefab.IsValid())
                {
                    continue;
                }
                const auto* authority = scene.TryGet<Authority>(entity);
                if (authority != nullptr && authority->Tier != Tier::Server)
                {
                    continue;
                }
                world.Replication.SetEntityPrefab(identity.Id, source.Prefab);
            }
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
                                       .SimTickRate = resolved.SimTickRate,
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
                                           .SimTickRate = world.SimTickRate,
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

            const WorldResolveResult resolve =
                Directory->Resolve(Net::JoinRequestInfo{.Connection = id,
                                                        .Account = conn.Account,
                                                        .Key = request.Key,
                                                        .Payload = request.Payload,
                                                        .Profile = ProfileOf(conn.Account)},
                                   static_cast<u32>(conn.Joins.size()));
            if (resolve.Outcome == WorldResolveOutcome::Denied)
            {
                deny(resolve.Reason);
                return;
            }
            if (resolve.Outcome == WorldResolveOutcome::Opened)
            {
                EnsureHostedWorld(request.Key, *resolve.Opened);
            }
            else if (!Worlds.contains(resolve.World.Value))
            {
                // A converged bucket this host never wrapped — a local (standalone) travel opened it
                // through the shared directory. Wrap it on demand from the recorded resolution so
                // the join replicates it like any hosted world; a bucket with no record (registered
                // outside the host) cannot be replicated and is refused.
                if (const ServerWorldResolution* recorded = Directory->ResolutionOf(resolve.World))
                {
                    EnsureHostedWorld(request.Key, *recorded);
                }
                else
                {
                    deny(Net::JoinDenyReason::NoSuchWorld);
                    return;
                }
            }

            const WorldInstanceId worldId = resolve.World;
            HostedWorld& world = WorldOf(worldId);
            const Net::JoinId joinId = conn.NextJoin;
            conn.NextJoin += 1;

            Entity seat = Entity::Null;
            const u32 seatNetId = SpawnSeat(world, id, conn.Account, seat);
            world.Replication.AddConnection(id);
            if (world.SeatPrefabId.IsValid())
            {
                world.Replication.SetEntityPrefab(seatNetId, world.SeatPrefabId);
            }
            conn.Joins.emplace(joinId, JoinState{.Join = joinId,
                                                 .World = worldId,
                                                 .Key = request.Key,
                                                 .Seat = seat,
                                                 .Durability = request.Durability});
            conn.KeyToJoin.emplace(request.Key, joinId);

            // Report the live join as presence to the directory, which owns the refcount and dwell,
            // recording the account as a member of the bucket (the MembersOf primitive).
            Directory->AddJoin(worldId, conn.Account);

            // The session record follows as a side effect of the join, per its resolved durability:
            // a standing join enters the standing list, a gameplay join becomes the account's
            // gameplay entry, an opted-out join enters nothing. A reattach's gameplay re-join is
            // refreshed from the stashed entry so the round trip keeps the captured pose.
            if (request.Durability == Net::SessionDurability::Standing)
            {
                Sessions->RecordStandingJoin(conn.Account, request.Key);
            }
            else if (request.Durability == Net::SessionDurability::Gameplay)
            {
                if (conn.PendingReattachGameplay.has_value() &&
                    conn.PendingReattachGameplay->Key == request.Key)
                {
                    Sessions->RecordGameplay(conn.Account, request.Key,
                                             conn.PendingReattachGameplay->Params,
                                             conn.PendingReattachGameplay->Pose);
                }
                else
                {
                    Sessions->RecordGameplay(conn.Account, request.Key, request.Payload,
                                             request.Payload);
                }
                conn.PendingReattachGameplay.reset();
            }

            sendAccept(joinId, world, seatNetId);
            Log::Info("ServerHost accepted connection {} into world {} as join {}", id,
                      worldId.Value, joinId);
        }

        // Releases a join from its world: destroys the seat and reports the presence drop (and the
        // account's membership drop) to the directory, which starts the idle dwell for a bucket that
        // just emptied.
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
            const auto conn = Connections.find(id);
            Directory->RemoveJoin(join.World, now,
                                  conn != Connections.end() ? conn->second.Account
                                                            : Net::AccountId{});
        }

        // Handles a client's leave notice: releases the named join's seat + presence and surfaces a
        // WorldLeft event, the connection staying live. Idempotent — an unknown join is dropped.
        void HandleLeave(Net::ConnectionId id, Net::JoinId join, f64 now)
        {
            const auto connIt = Connections.find(id);
            if (connIt == Connections.end())
            {
                return;
            }
            ConnectionState& conn = connIt->second;
            const auto joinIt = conn.Joins.find(join);
            if (joinIt == conn.Joins.end())
            {
                return;
            }
            // An explicit leave of a standing join withdraws it from the record — unlike a
            // disconnect, which keeps every entry for the reattach.
            if (joinIt->second.Durability == Net::SessionDurability::Standing)
            {
                Sessions->RemoveStandingJoin(conn.Account, joinIt->second.Key);
            }
            ReleaseJoin(id, joinIt->second, now);
            conn.Joins.erase(joinIt);
            std::erase_if(conn.KeyToJoin,
                          [join](const auto& entry) { return entry.second == join; });
            Events.push_back(
                Net::NetEvent{.Type = Net::NetEventType::WorldLeft, .Id = id, .Join = join});
        }

        // Sends a directed-travel control message to a connection (a travel reply, a reattach
        // restore, or unprompted).
        void SendDirectedTravel(Net::ConnectionId id, Net::JoinId leave, const Net::WorldKey& key,
                                const Net::Blob& payload, const Net::Blob& pose, bool present,
                                Net::SessionDurability durability)
        {
            const vector<u8> message =
                Net::EncodeDirectedTravel(Net::DirectedTravelMessage{.Leave = leave,
                                                                     .Join = key,
                                                                     .Payload = payload,
                                                                     .Pose = pose,
                                                                     .Present = present,
                                                                     .Durability = durability});
            (void)Server->Get(id).Send(Net::Channel::ReliableOrdered,
                                       Net::EncodeWorldEnvelope(Net::ControlJoinId, message));
        }

        // Restores an admitted account's session on reconnect: the policy transform rewrites the
        // record, the standing joins are re-issued as non-presenting directed travels (no leave
        // arm), and the gameplay entry is resolved through the directory — get-or-place by key,
        // the factory re-running with the recorded params when the key misses — then directed with
        // the recorded pose. A gameplay resolve failure clears the entry, so the client lands
        // wherever its front door puts it.
        void ReattachAccount(Net::ConnectionId id, const Net::AccountId& account)
        {
            if (!account.IsValid())
            {
                return;
            }
            Sessions->EnsureLoaded(account);
            const optional<Net::SessionRecord> record = Sessions->BeginReattach(account);
            if (!record.has_value())
            {
                return;
            }

            for (const Net::WorldKey& key : record->StandingJoins)
            {
                SendDirectedTravel(id, Net::ControlJoinId, key, Net::Blob{}, Net::Blob{},
                                   /*present=*/false, Net::SessionDurability::Standing);
            }

            if (record->Gameplay.Key == Net::WorldKey{})
            {
                return;
            }
            const WorldResolveResult resolve =
                Directory->Resolve(Net::JoinRequestInfo{.Connection = id,
                                                        .Account = account,
                                                        .Key = record->Gameplay.Key,
                                                        .Payload = record->Gameplay.Params,
                                                        .Profile = ProfileOf(account)},
                                   /*heldWorlds=*/0);
            if (resolve.Outcome == WorldResolveOutcome::Denied)
            {
                Log::Warn("ServerHost reattach for connection {} denied its gameplay world "
                          "(reason {}); clearing to the front door",
                          id, static_cast<u32>(resolve.Reason));
                Sessions->ClearGameplay(account);
                return;
            }
            if (resolve.Outcome == WorldResolveOutcome::Opened)
            {
                EnsureHostedWorld(record->Gameplay.Key, *resolve.Opened);
            }
            Connections[id].PendingReattachGameplay = record->Gameplay;
            SendDirectedTravel(id, Net::ControlJoinId, record->Gameplay.Key,
                               record->Gameplay.Params, record->Gameplay.Pose, /*present=*/true,
                               Net::SessionDurability::Gameplay);
        }

        // Refreshes every connected account's gameplay pose into the registry — the pre-save
        // capture window the debounced checkpoint opens.
        void RefreshGameplayPoses()
        {
            for (const auto& [id, conn] : Connections)
            {
                for (const auto& [joinId, join] : conn.Joins)
                {
                    if (join.Durability == Net::SessionDurability::Gameplay)
                    {
                        Sessions->CaptureGameplayPose(conn.Account, join.World, join.Seat);
                    }
                }
            }
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

        // Validates and queues one outbound game message for a connection: payload bound and
        // outbound cap enforced at send (fail-loud, never a silent cap), the framed bytes flushed
        // into the reliable stream at Pump.
        VoidResult QueueMessage(Net::ConnectionId to, Net::ChannelId channel, Net::Blob&& payload)
        {
            VE_ASSERT(channel != Net::InvalidChannelId, "game message sent on the invalid channel");
            if (payload.Bytes.size() > Net::MaxMessagePayloadSize)
            {
                return std::unexpected(
                    fmt::format("message payload of {} bytes exceeds the {}-byte bound",
                                payload.Bytes.size(), Net::MaxMessagePayloadSize));
            }
            const auto it = Connections.find(to);
            if (it == Connections.end())
            {
                return std::unexpected(fmt::format("connection {} is not established", to));
            }
            ConnectionState& conn = it->second;
            vector<u8> frame = Net::EncodeWorldEnvelope(Net::ControlJoinId,
                                                        Net::EncodeGameMessage(channel, payload));
            if (conn.OutboundMessages.size() >= Net::MaxOutboundMessages ||
                conn.OutboundBytes + frame.size() > Net::MaxOutboundMessageBytes)
            {
                return std::unexpected(
                    fmt::format("connection {}'s outbound message queue is full ({} messages, {} "
                                "bytes queued)",
                                to, conn.OutboundMessages.size(), conn.OutboundBytes));
            }
            conn.OutboundBytes += frame.size();
            conn.OutboundMessages.push_back(std::move(frame));
            return {};
        }

        // Flushes every connection's queued game messages into its reliable-ordered stream, in send
        // order. A connection the server no longer holds drops its queue (the connection died; the
        // at-most-once contract stands).
        void FlushOutboundMessages()
        {
            for (auto& [id, conn] : Connections)
            {
                if (conn.OutboundMessages.empty())
                {
                    continue;
                }
                const std::span<const Net::ConnectionId> live = Server->Connections();
                if (std::ranges::find(live, id) == live.end())
                {
                    conn.OutboundMessages.clear();
                    conn.OutboundBytes = 0;
                    continue;
                }
                for (const vector<u8>& frame : conn.OutboundMessages)
                {
                    (void)Server->Get(id).Send(Net::Channel::ReliableOrdered, frame);
                }
                conn.OutboundMessages.clear();
                conn.OutboundBytes = 0;
            }
        }

        // Drops a connection that exceeded the per-pump inbound message budget: the flooder is
        // disconnected with the logged reason and its queued messages are discarded, so it cannot
        // starve the peers sharing the pump.
        void DropFlooder(Net::ConnectionId id, u32 messages, usize bytes)
        {
            Log::Warn("ServerHost dropping connection {}: inbound message flood ({} messages, {} "
                      "bytes in one pump)",
                      id, messages, bytes);
            (void)Server->Disconnect(id, Net::DisconnectReason::Kicked);
            std::erase_if(Inbound,
                          [id](const InboundMessage& message) { return message.From == id; });
        }
    };

    ServerHost::ServerHost(Unique<State> state) : m_State(std::move(state)) {}

    ServerHost::~ServerHost() = default;

    Result<Unique<ServerHost>> ServerHost::Create(const ServerHostInfo& info)
    {
        auto state = CreateUnique<State>();
        state->Assets = &info.Assets;
        state->Primary = info.WorldId;
        state->LocalAccount = info.LocalAccount;
        // The local player performs no connect, so its profile is bound here instead — owned by
        // ServerConnectionId, which no connection holds, so no teardown clears it.
        state->BindProfile(Net::ServerConnectionId, info.LocalAccount, &info.LocalProfile);

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

        // Consume a borrowed session registry when given one (the Application-shared path);
        // otherwise build a private one from the info's session hooks over the initial world's
        // type registry (the self-contained ServerHost).
        if (info.Sessions != nullptr)
        {
            state->Sessions = info.Sessions;
        }
        else
        {
            state->OwnedSessions = Net::SessionRegistry::Create(Net::SessionRegistryInfo{
                .Types = &info.World.GetTypeRegistry(),
                .TransformOnReattach = info.TransformOnReattach,
                .CaptureTravelPose = info.CaptureTravelPose,
                .LoadSession = info.LoadSession,
                .SaveSession = info.SaveSession,
            });
            state->Sessions = state->OwnedSessions.get();
        }

        state->Worlds.emplace(info.WorldId.Value,
                              State::HostedWorld{.Id = info.WorldId,
                                                 .Key = info.Key,
                                                 .World = &info.World,
                                                 .LevelId = info.LevelId,
                                                 .SimTickRate = info.SimTickRate,
                                                 .Digest = info.Digest,
                                                 .SeatPrefab = info.SeatPrefab,
                                                 .SeatPrefabId = info.SeatPrefabId,
                                                 .Replication = ReplicationServer(info.Replication),
                                                 .InterestSettings = info.Interest,
                                                 .InterestPolicy = info.InterestPolicy});
        state->Directory->Register(info.Key, info.WorldId);

        const Net::ServerInfo serverInfo = info.Server;
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
                               .SimTickRate = world.SimTickRate,
                               .Digest = world.Digest,
                               .SeatPrefab = world.SeatPrefab,
                               .SeatPrefabId = world.SeatPrefabId,
                               .Replication = ReplicationServer(world.Replication),
                               .InterestSettings = world.Interest,
                               .InterestPolicy = world.InterestPolicy});
        // Register the world as a never-reaped bucket of its key; re-adding the same id is idempotent.
        m_State->Directory->Register(world.Key, world.WorldId);
    }

    void ServerHost::Pump(f64 now, u64 /*tick*/)
    {
        State& s = *m_State;
        s.Events.clear();

        // Assign wire ids to entities the spawn rule added this tick (the pawns), per world from its
        // own allocator, then generate and queue each ready (connection, join)'s stream from its
        // world's replication instance — each message wrapped in its JoinId envelope so the peer
        // demuxes it to the right world. Each world's snapshot cadence and ack baselines stamp in
        // that world's own sim tick (its Scene's change tick, the tick its writes were stamped at),
        // not a shared host tick, so a world running below the host pump rate qualifies its writes
        // as deltas against its own tick and its join's client-side estimator tracks its own clock.
        for (auto& [value, world] : s.Worlds)
        {
            AssignServerNetIds(*world.World, world.Allocator);
            s.AssociateMarkedSpawns(world);
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
                const u64 worldTick = world.World->GetChangeTick();
                const optional<set<NetId>> interest = s.ComputeInterest(world, id, join);
                for (const ReplicationMessage& message : world.Replication.Generate(
                         id, *world.World, worldTick, interest ? &*interest : nullptr))
                {
                    world.ReplicationBytes += message.Bytes.size();
                    (void)s.Server->Get(id).Send(message.Channel,
                                                 Net::EncodeWorldEnvelope(joinId, message.Bytes));
                }
            }
        }

        // Flush queued game messages into each connection's reliable stream before the transport
        // pumps, so a message accepted this frame goes out with it.
        s.FlushOutboundMessages();

        // Receive + connection handshake + flush + reap dead peers.
        s.Server->Pump(now);

        for (const Net::NetEvent& event : s.Server->Events())
        {
            if (event.Type == Net::NetEventType::Connected)
            {
                // Bind the admitted account for the connection's lifetime; every player-keyed
                // decision below (authorize, seat stamp, directory membership) reads it from here.
                s.Connections.try_emplace(event.Id).first->second.Account = event.Account;
                // The account's profile is bound before the reattach below, so a resolve driven by
                // it already sees the profile the fresh connection presented.
                s.BindProfile(event.Id, event.Account, s.Server->ProfileFor(event.Id));
                // Reconnecting is reattaching: an admitted account with a record has its standing
                // joins re-issued and its gameplay world resolved back through the directory.
                s.ReattachAccount(event.Id, event.Account);
            }
            else if (event.Type == Net::NetEventType::Disconnected)
            {
                const auto it = s.Connections.find(event.Id);
                if (it != s.Connections.end())
                {
                    // Capture the departing account's gameplay pose before the seat is destroyed,
                    // then save the record — the disconnect is a durability point.
                    for (auto& [joinId, join] : it->second.Joins)
                    {
                        if (join.Durability == Net::SessionDurability::Gameplay)
                        {
                            s.Sessions->CaptureGameplayPose(it->second.Account, join.World,
                                                            join.Seat);
                        }
                    }
                    s.Sessions->Save(it->second.Account);
                    for (auto& [joinId, join] : it->second.Joins)
                    {
                        s.ReleaseJoin(event.Id, join, now);
                    }
                    s.ReleaseProfile(event.Id, it->second.Account);
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
            // The per-pump inbound message budget: a connection flooding game messages is dropped
            // (with its queued messages) rather than starving the peers sharing this pump.
            u32 inboundMessages = 0;
            usize inboundBytes = 0;
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
                    if (type == Net::JoinMessageType::GameMessage)
                    {
                        inboundMessages += 1;
                        inboundBytes += message.size();
                        if (inboundMessages > Net::MaxInboundMessagesPerPump ||
                            inboundBytes > Net::MaxInboundMessageBytesPerPump)
                        {
                            s.DropFlooder(id, inboundMessages, inboundBytes);
                            break;
                        }
                        if (optional<Net::GameMessageFrame> frame =
                                Net::DecodeGameMessage(env->Payload))
                        {
                            // Queued, not dispatched: delivery is frame-safe (DeliverMessages).
                            s.Inbound.push_back(
                                State::InboundMessage{.From = id,
                                                      .Channel = frame->Envelope.Channel,
                                                      .Payload = std::move(frame->Payload)});
                        }
                        continue;
                    }
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
                        // A client travel request: the server directs the resulting join, echoing the
                        // requested key (a server-driven placement would resolve a different one) and
                        // the request's presentation/durability. The connection leaves its current
                        // (presenting) join once the new is ready.
                        if (const optional<Net::TravelRequestMessage> request =
                                Net::DecodeTravelRequest(env->Payload))
                        {
                            const State::JoinState* current = s.CurrentJoinState(id);
                            const Net::JoinId leave =
                                current != nullptr ? current->Join : Net::ControlJoinId;
                            s.SendDirectedTravel(id, leave, request->Key, request->Payload,
                                                 Net::Blob{}, request->Present,
                                                 request->Durability);
                        }
                    }
                    else if (type == Net::JoinMessageType::LeaveNotice)
                    {
                        // A client leaving one joined world: tear down its seat and presence, the
                        // connection staying live (distinct from a disconnect, which reaps every join).
                        if (const optional<Net::LeaveNoticeMessage> notice =
                                Net::DecodeLeaveNotice(env->Payload))
                        {
                            s.HandleLeave(id, notice->Join, now);
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

        // The debounced durability checkpoint: dirty records are saved, live gameplay poses
        // refreshed just before.
        s.Sessions->Checkpoint(now, [&s] { s.RefreshGameplayPoses(); });

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

    u64 ServerHost::ReplicationBytesForWorld(WorldInstanceId world) const
    {
        const State::HostedWorld* hosted = m_State->TryWorldOf(world);
        return hosted != nullptr ? hosted->ReplicationBytes : 0;
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

    Net::AccountId ServerHost::AccountFor(Net::ConnectionId id) const
    {
        const auto it = m_State->Connections.find(id);
        return it != m_State->Connections.end() ? it->second.Account : Net::AccountId{};
    }

    Net::ConnectionId ServerHost::ConnectionFor(const Net::AccountId& account) const
    {
        if (account.IsValid())
        {
            for (const auto& [id, conn] : m_State->Connections)
            {
                if (conn.Account == account)
                {
                    return id;
                }
            }
        }
        return Net::ServerConnectionId;
    }

    const Net::Blob* ServerHost::ProfileOf(Net::AccountId account) const
    {
        return m_State->ProfileOf(account);
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
                                  const Net::WorldKey& key, const Net::Blob& payload,
                                  const Net::Blob& pose, const bool present,
                                  const optional<bool> standing)
    {
        m_State->SendDirectedTravel(connection, leave, key, payload, pose, present,
                                    Net::ResolveSessionDurability(present, standing));
    }

    Net::SessionRegistry& ServerHost::Sessions()
    {
        return *m_State->Sessions;
    }

    void ServerHost::RegisterChannel(const Net::ChannelId channel,
                                     function<void(Net::ConnectionId, const Net::Blob&)> onMessage)
    {
        VE_ASSERT(channel != Net::InvalidChannelId, "RegisterChannel on the invalid channel id");
        m_State->Channels.insert_or_assign(channel, std::move(onMessage));
    }

    VoidResult ServerHost::Send(const Net::ConnectionId to, const Net::ChannelId channel,
                                Net::Blob payload)
    {
        return m_State->QueueMessage(to, channel, std::move(payload));
    }

    VoidResult ServerHost::Send(const Net::AccountId to, const Net::ChannelId channel,
                                Net::Blob payload)
    {
        State& s = *m_State;
        const Net::ConnectionId connection = ConnectionFor(to);
        if (connection != Net::ServerConnectionId)
        {
            return s.QueueMessage(connection, channel, std::move(payload));
        }
        // The listen host's own player holds no connection; its messages loop back to the local
        // registered handler, queued for the same frame-safe delivery as wire-borne ones.
        if (s.LocalAccount.IsValid() && to == s.LocalAccount)
        {
            VE_ASSERT(channel != Net::InvalidChannelId, "game message sent on the invalid channel");
            if (payload.Bytes.size() > Net::MaxMessagePayloadSize)
            {
                return std::unexpected(
                    fmt::format("message payload of {} bytes exceeds the {}-byte bound",
                                payload.Bytes.size(), Net::MaxMessagePayloadSize));
            }
            s.Inbound.push_back(State::InboundMessage{.From = Net::ServerConnectionId,
                                                      .Channel = channel,
                                                      .Payload = std::move(payload)});
            return {};
        }
        return std::unexpected("account has no live connection");
    }

    VoidResult ServerHost::SendToWorldMembers(const WorldInstanceId world,
                                              const Net::ChannelId channel,
                                              const Net::Blob& payload)
    {
        State& s = *m_State;
        const State::HostedWorld* hosted = s.TryWorldOf(world);
        if (hosted == nullptr)
        {
            return std::unexpected(fmt::format("world {} is not hosted", world.Value));
        }
        usize failed = 0;
        string firstError;
        for (const Net::AccountId& member : s.Directory->MembersOf(hosted->Key))
        {
            if (VoidResult sent = Send(member, channel, payload); !sent)
            {
                failed += 1;
                if (firstError.empty())
                {
                    firstError = std::move(sent.error());
                }
            }
        }
        if (failed > 0)
        {
            return std::unexpected(fmt::format("{} member send(s) failed: {}", failed, firstError));
        }
        return {};
    }

    void ServerHost::DeliverMessages()
    {
        State& s = *m_State;
        // Swap the queue out so a handler sending (or looping back) during delivery appends to a
        // fresh queue for the next frame rather than extending this iteration.
        const vector<State::InboundMessage> inbound = std::move(s.Inbound);
        s.Inbound.clear();
        for (const State::InboundMessage& message : inbound)
        {
            const auto it = s.Channels.find(message.Channel);
            if (it == s.Channels.end())
            {
                if (s.UnregisteredWarned.insert(message.Channel).second)
                {
                    Log::Warn("ServerHost dropping message on unregistered channel {:016X}",
                              message.Channel);
                }
                continue;
            }
            it->second(message.From, message.Payload);
        }
    }

    usize ServerHost::HostedWorldCount() const
    {
        return m_State->Directory->WorldCount();
    }

    bool ServerHost::IsReplicatingWorld(const WorldInstanceId world) const
    {
        return m_State->TryWorldOf(world) != nullptr;
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
        function<Net::ContentDigest(const Net::WorldKey&, const Net::Blob&)> WorldDigest;
        function<Scene*(AssetId)> LoadLevel;
        function<Scene*()> OpenEmptyWorld;
        function<Ref<Prefab>(AssetId)> ResolvePrefab;
        function<void(Scene&, Entity)> OnPossession;
        PredictionPolicy Policy;
        Net::ReplayTick Replay;
        Net::ReconcileTolerances Tolerances;
        Net::TickSyncSettings TickSyncSettings;
        Net::QuantizationSettings Quantization;
        function<Net::QuantizationSettings(const Net::WorldKey&)> WorldQuantization;
        function<Net::ReconcileTolerances(const Net::WorldKey&)> WorldTolerances;
        function<void(Net::JoinId)> OnLeaveWorld;
        function<void(const Net::WorldKey&, Net::JoinDenyReason)> OnTravelDenied;

        // The single-source anchor registry shared across this client's joins: one live join binds a
        // claimant at a time, and a second binding an already-bound claimant is a fatal assert.
        AnchorBindings Anchors;

        // One joined world's whole client state — replication, identity map, prediction, and clock,
        // all scoped per JoinId.
        struct JoinClient
        {
            Net::JoinId Join = Net::ControlJoinId;
            Net::WorldKey Key;    // the key the join was requested and granted for
            Net::Blob Payload;    // the params the reply echoed, for the game's reconstruction
            Net::Blob Pose;       // the arrival pose a directed travel carried, or empty
            bool Present = false; // whether this join presents (a standing re-join does not)
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
            // Resolved per key at join from WorldTolerances (or the shared value): a world whose linear
            // unit is not the metre reconciles against its own scale, not the shared metre grid.
            Net::ReconcileTolerances Tolerances;
        };

        // A requested-but-unaccepted join, correlated to its reply by the token. Payload rides the join
        // request; LeaveOnReady names a join to leave once this one is ready (a make-before-break
        // directed travel), or ControlJoinId for an ordinary join.
        struct PendingJoin
        {
            u32 Token = 0;
            Net::WorldKey Key;
            Net::Blob Payload;
            // The arrival pose a directed travel carried; empty for an ordinary join.
            Net::Blob Pose;
            Net::JoinId LeaveOnReady = Net::ControlJoinId;
            // The live scene an adopt-in-place join binds to; null for an ordinary level-loading join.
            Scene* AdoptScene = nullptr;
            // Presentation + session-record class, resolved at the call site (or carried by the
            // directed travel) and echoed on the join request.
            bool Present = false;
            Net::SessionDurability Durability = Net::SessionDurability::Standing;
            bool Sent = false;
        };

        std::map<Net::JoinId, JoinClient> Joins; // ordered, so the first is the current-join
        vector<PendingJoin> Pending;
        u32 NextToken = 1;
        Net::JoinId CurrentJoin = Net::ControlJoinId;

        // One inbound game message queued for the frame-safe delivery point (the sender is always
        // the server, so only the channel and blob are recorded).
        struct InboundMessage
        {
            Net::ChannelId Channel = Net::InvalidChannelId;
            Net::Blob Payload;
        };

        // The game message channel: registered handlers, the frame-safe inbound queue, the queued
        // outbound frames (flushed at Pump, capped at send), and the one-shot warnings.
        std::unordered_map<Net::ChannelId, function<void(const Net::Blob&)>> Channels;
        vector<InboundMessage> Inbound;
        std::deque<vector<u8>> OutboundMessages;
        usize OutboundBytes = 0;
        std::unordered_set<Net::ChannelId> UnregisteredWarned;

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

            // Publish this join's own seat so the scene-level IsLocallyOwned predicate tells it from
            // the peers' replicated seats. The marker is local-only — never replicated — so a peer's
            // seat that reaches this client through the stream carries none.
            if (!jc.World->Has<LocalSeat>(seat))
            {
                jc.World->Add<LocalSeat>(seat);
            }

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
                    if (auto* authority = jc.World->TryGet<Authority>(entity);
                        authority != nullptr && authority->Tier == Tier::Predicted)
                    {
                        authority->Tier = Tier::Remote;
                    }
                    // Drop the physics-rollback marker with the prediction stance that placed it.
                    if (jc.World->Has<Predicted>(entity))
                    {
                        (void)jc.World->Remove<Predicted>(entity);
                    }
                }
                jc.History.Untrack(entity);
            }
            jc.Predicted.clear();

            if (pawn.IsNull() || !jc.World->IsAlive(pawn))
            {
                return;
            }
            jc.Predicted =
                Policy ? Policy(*jc.World, pawn) : DefaultPredictedEntities(*jc.World, pawn);
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
                // A predicted character's physics state rolls back: mark it so the mover re-seats its
                // capsule onto the reconciled pose. Only a character carries the marker — a predicted
                // entity with no capsule has nothing to re-seat, and everything else in the world is
                // deterministic-from-tick or static.
                if (jc.World->Has<CharacterController>(entity) && !jc.World->Has<Predicted>(entity))
                {
                    jc.World->Add<Predicted>(entity);
                }
                jc.History.Track(entity);
            }
        }

        // Leaves a join: removes its footprint from the scene (destroy wire-owned, release adopted,
        // demote predicted), tells the server to tear down the seat, drops the per-join state, and
        // notifies the caller. The scene itself is left standing — a peer join may still present it.
        void LeaveJoin(Net::JoinId join)
        {
            const auto it = Joins.find(join);
            if (it == Joins.end())
            {
                return;
            }
            JoinClient& jc = it->second;

            // Demote the predicted set back to Remote and drop its history, then tear down the join's
            // spawned/adopted set in the scene.
            if (jc.World != nullptr)
            {
                // Drop the local-seat marker before the wire-owned teardown, so a seat that outlives
                // its wire id (an adopted claimant) stops answering as locally owned.
                if (!jc.Seat.IsNull() && jc.World->IsAlive(jc.Seat) &&
                    jc.World->Has<LocalSeat>(jc.Seat))
                {
                    (void)jc.World->Remove<LocalSeat>(jc.Seat);
                }
                Repredict(jc, Entity::Null);
                jc.Replication->Leave(*jc.World);
            }

            // Tell the server we are leaving so it tears down this join's seat and drops the presence.
            if (Client->State() == Net::ClientState::Connected)
            {
                const vector<u8> notice =
                    Net::EncodeLeaveNotice(Net::LeaveNoticeMessage{.Join = join});
                (void)Client->Server().Send(Net::Channel::ReliableOrdered,
                                            Net::EncodeWorldEnvelope(Net::ControlJoinId, notice));
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
            Scene* const adoptScene = pending->AdoptScene;
            Net::Blob arrivalPose = std::move(pending->Pose);
            const bool present = pending->Present;
            Pending.erase(pending);

            if (Joins.contains(accept.Join))
            {
                return; // already installed (a resent accept)
            }

            const AssetId levelId{.Value = accept.LevelId};
            const Net::ContentDigest expected =
                WorldDigest ? WorldDigest(key, accept.Payload) : Net::ContentDigest{};
            if (!(expected == accept.WorldDigest))
            {
                Log::Error(
                    "ClientHost rejecting join {}: world digest mismatch (server {:016X}{:016X}"
                    ", client {:016X}{:016X})",
                    accept.Join, accept.WorldDigest.Hi, accept.WorldDigest.Lo, expected.Hi,
                    expected.Lo);
                return;
            }

            // Adopt-in-place: the reply's level is ignored — the standing scene is the client's
            // reconstruction, and the stream applies into it. An ordinary join loads the named
            // level; a level-less reply (a data world) installs an empty stream-populated scene
            // through OpenEmptyWorld, never touching LoadLevel.
            Scene* scene = nullptr;
            if (adoptScene != nullptr)
            {
                scene = adoptScene;
            }
            else if (!levelId.IsValid())
            {
                scene = OpenEmptyWorld ? OpenEmptyWorld() : nullptr;
            }
            else
            {
                scene = LoadLevel ? LoadLevel(levelId) : nullptr;
            }
            if (scene == nullptr)
            {
                return; // load failed — no join installed
            }

            JoinClient jc;
            jc.Join = accept.Join;
            jc.Key = key;
            jc.Payload = accept.Payload;
            jc.Pose = std::move(arrivalPose);
            jc.Present = present;
            jc.SeatNetId = accept.SeatNetId;
            jc.World = scene;
            // Each join's controller runs at the hosted world's own rate (the reply carries it), so
            // a slow data world's RTT converts into leads in its own ticks; the shared settings keep
            // supplying the margin and slew knobs (and the rate for a reply carrying none).
            Net::TickSyncSettings tickSync = TickSyncSettings;
            if (accept.SimTickRate > 0)
            {
                tickSync.TickRate = accept.SimTickRate;
            }
            jc.TickSync = Net::TickOffsetEstimator(tickSync);
            jc.Replication = CreateUnique<ReplicationClient>(ResolvePrefab);
            // The per-key spatial envelope: WorldQuantization, when set, yields this key's grid so two
            // hosted worlds with different envelopes each decode correctly on one client; unset threads
            // the shared grid onto every join.
            jc.Replication->SetQuantization(WorldQuantization ? WorldQuantization(key)
                                                              : Quantization);
            // The per-key reconcile tolerances, resolved the same way and for the same reason: a world
            // in a different linear unit needs its own compare/snap grid, or its client reconciles
            // unbounded drift as "matched".
            jc.Tolerances = WorldTolerances ? WorldTolerances(key) : Tolerances;
            // Scope this join's anchor adoptions to the client-shared registry, so the single-source
            // invariant is enforced across every join sharing a scene.
            jc.Replication->SetAdoption(accept.Join, Anchors);
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
        state->OpenEmptyWorld = info.OpenEmptyWorld;
        state->ResolvePrefab = info.ResolvePrefab;
        state->OnPossession = info.OnPossession;
        state->Policy = info.Prediction;
        state->Replay = info.Replay;
        state->Tolerances = info.Tolerances;
        state->TickSyncSettings = info.TickSync;
        state->Quantization = info.Quantization;
        state->WorldQuantization = info.WorldQuantization;
        state->WorldTolerances = info.WorldTolerances;
        state->OnLeaveWorld = info.OnLeaveWorld;
        state->OnTravelDenied = info.OnTravelDenied;
        return Unique<ClientHost>(new ClientHost(std::move(state)));
    }

    void ClientHost::Join(const Net::WorldKey& key, const Net::Blob& payload, const bool present,
                          const optional<bool> standing)
    {
        State& s = *m_State;
        const u32 token = s.NextToken;
        s.NextToken += 1;
        s.Pending.push_back(
            State::PendingJoin{.Token = token,
                               .Key = key,
                               .Payload = payload,
                               .Present = present,
                               .Durability = Net::ResolveSessionDurability(present, standing)});
    }

    void ClientHost::JoinInto(const Net::WorldKey& key, Scene& adoptScene, const Net::Blob& payload,
                              const bool present, const optional<bool> standing)
    {
        State& s = *m_State;
        const u32 token = s.NextToken;
        s.NextToken += 1;
        s.Pending.push_back(
            State::PendingJoin{.Token = token,
                               .Key = key,
                               .Payload = payload,
                               .AdoptScene = &adoptScene,
                               .Present = present,
                               .Durability = Net::ResolveSessionDurability(present, standing)});
    }

    void ClientHost::Travel(const Net::WorldKey& key, const Net::Blob& payload, const bool present,
                            const optional<bool> standing)
    {
        State& s = *m_State;
        const vector<u8> message = Net::EncodeTravelRequest(Net::TravelRequestMessage{
            .Key = key,
            .Payload = payload,
            .Present = present,
            .Durability = Net::ResolveSessionDurability(present, standing)});
        (void)s.Client->Server().Send(Net::Channel::ReliableOrdered,
                                      Net::EncodeWorldEnvelope(Net::ControlJoinId, message));
    }

    void ClientHost::Leave(const Net::JoinId join)
    {
        m_State->LeaveJoin(join);
    }

    void ClientHost::RegisterChannel(const Net::ChannelId channel,
                                     function<void(const Net::Blob&)> onMessage)
    {
        VE_ASSERT(channel != Net::InvalidChannelId, "RegisterChannel on the invalid channel id");
        m_State->Channels.insert_or_assign(channel, std::move(onMessage));
    }

    VoidResult ClientHost::Send(const Net::ChannelId channel, Net::Blob payload)
    {
        State& s = *m_State;
        VE_ASSERT(channel != Net::InvalidChannelId, "game message sent on the invalid channel");
        if (s.Client->State() != Net::ClientState::Connected)
        {
            return std::unexpected("client is not connected");
        }
        if (payload.Bytes.size() > Net::MaxMessagePayloadSize)
        {
            return std::unexpected(
                fmt::format("message payload of {} bytes exceeds the {}-byte bound",
                            payload.Bytes.size(), Net::MaxMessagePayloadSize));
        }
        vector<u8> frame =
            Net::EncodeWorldEnvelope(Net::ControlJoinId, Net::EncodeGameMessage(channel, payload));
        if (s.OutboundMessages.size() >= Net::MaxOutboundMessages ||
            s.OutboundBytes + frame.size() > Net::MaxOutboundMessageBytes)
        {
            return std::unexpected(
                fmt::format("the outbound message queue is full ({} messages, {} bytes queued)",
                            s.OutboundMessages.size(), s.OutboundBytes));
        }
        s.OutboundBytes += frame.size();
        s.OutboundMessages.push_back(std::move(frame));
        return {};
    }

    void ClientHost::DeliverMessages()
    {
        State& s = *m_State;
        // Swap the queue out so a handler sending during delivery appends to a fresh queue for the
        // next frame rather than extending this iteration.
        const vector<State::InboundMessage> inbound = std::move(s.Inbound);
        s.Inbound.clear();
        for (const State::InboundMessage& message : inbound)
        {
            const auto it = s.Channels.find(message.Channel);
            if (it == s.Channels.end())
            {
                if (s.UnregisteredWarned.insert(message.Channel).second)
                {
                    Log::Warn("ClientHost dropping message on unregistered channel {:016X}",
                              message.Channel);
                }
                continue;
            }
            it->second(message.Payload);
        }
    }

    void ClientHost::Pump(f64 now)
    {
        State& s = *m_State;
        s.Client->Pump(now);

        if (s.Client->State() != Net::ClientState::Connected)
        {
            return;
        }

        // Auto-join the configured world once, the moment the connection is up. The single-world
        // convenience presents its world, so the join is the account's gameplay entry.
        if (s.AutoJoin && !s.AutoJoinRequested)
        {
            Join(s.AutoJoinKey, {}, /*present=*/true);
            s.AutoJoinRequested = true;
        }

        // Send any not-yet-sent join requests, enveloped at the join-control tier.
        for (State::PendingJoin& pending : s.Pending)
        {
            if (!pending.Sent)
            {
                const vector<u8> payload = Net::EncodeJoinRequest(
                    Net::JoinRequestMessage{.Key = pending.Key,
                                            .RequestToken = pending.Token,
                                            .Payload = pending.Payload,
                                            .Durability = pending.Durability});
                (void)s.Client->Server().Send(
                    Net::Channel::ReliableOrdered,
                    Net::EncodeWorldEnvelope(Net::ControlJoinId, payload));
                pending.Sent = true;
            }
        }

        // Flush queued game messages into the reliable stream, in send order.
        for (const vector<u8>& frame : s.OutboundMessages)
        {
            (void)s.Client->Server().Send(Net::Channel::ReliableOrdered, frame);
        }
        s.OutboundMessages.clear();
        s.OutboundBytes = 0;

        // The per-pump inbound message budget: a flooding peer is dropped (with its queued
        // messages) rather than starving the join replies and spawn stream sharing the channel.
        u32 inboundMessages = 0;
        usize inboundBytes = 0;

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
                if (type == Net::JoinMessageType::GameMessage)
                {
                    inboundMessages += 1;
                    inboundBytes += message.size();
                    if (inboundMessages > Net::MaxInboundMessagesPerPump ||
                        inboundBytes > Net::MaxInboundMessageBytesPerPump)
                    {
                        Log::Warn("ClientHost disconnecting: inbound message flood ({} messages, "
                                  "{} bytes in one pump)",
                                  inboundMessages, inboundBytes);
                        s.Client->Disconnect();
                        s.Inbound.clear();
                        break;
                    }
                    if (optional<Net::GameMessageFrame> frame =
                            Net::DecodeGameMessage(env->Payload))
                    {
                        // Queued, not dispatched: delivery is frame-safe (DeliverMessages).
                        s.Inbound.push_back(
                            State::InboundMessage{.Channel = frame->Envelope.Channel,
                                                  .Payload = std::move(frame->Payload)});
                    }
                    continue;
                }
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
                                                               .Pose = directed->Pose,
                                                               .LeaveOnReady = directed->Leave,
                                                               .Present = directed->Present,
                                                               .Durability = directed->Durability});
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
                                         applied.LastConsumedInputTick, s.Replay, jc->Tolerances);
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

    const Net::Blob& ClientHost::JoinPayload(const Net::JoinId join) const
    {
        static const Net::Blob empty;
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->Payload : empty;
    }

    const Net::Blob& ClientHost::ArrivalPose(const Net::JoinId join) const
    {
        static const Net::Blob empty;
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->Pose : empty;
    }

    bool ClientHost::IsPresenting(const Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr && jc->Present;
    }

    Net::WorldKey ClientHost::JoinKey(const Net::JoinId join) const
    {
        const State::JoinClient* jc = m_State->JoinClientOf(join);
        return jc != nullptr ? jc->Key : Net::WorldKey{};
    }

    optional<Net::JoinId> ClientHost::JoinForKey(const Net::WorldKey& key) const
    {
        for (const auto& [joinId, jc] : m_State->Joins)
        {
            if (jc.Key == key)
            {
                return joinId;
            }
        }
        return std::nullopt;
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

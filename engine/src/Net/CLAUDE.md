# Veng/Net — server-authoritative client/server networking

`Veng/Net/` is the core-engine networking layer — in `libveng` proper, not an optional library —
because it is load-bearing for gameplay components (`Authority`, `PlayerInput`) that already live
in the engine, and the world drive must know the tick model regardless. It turns a game into a
server-authoritative client/server system: the server simulates truth, clients display it. It is
**not lockstep** — cross-machine float determinism is never required and no bug hunt should chase
it; the Sim/View split already draws the line (Sim state replicates, View state derives locally).
Project-wide conventions are in [the root CLAUDE.md](../../../CLAUDE.md), the runtime overview in
[engine/CLAUDE.md](../../CLAUDE.md), the gameplay/ECS layer in [../Scene/CLAUDE.md](../Scene/CLAUDE.md),
and the task-oriented guide is [docs/guides/networking.md](../../../docs/guides/networking.md).

## The tick model

The tick model is the prerequisite, threaded through `SystemContext`. The world drive is an
accumulator: the **Sim phase steps at a fixed `SimTickRate`** (`GameWorldInfo`, default 60 Hz)
with a monotonic `u64` tick, the **View phase runs once per frame** with the frame delta and an
interpolation `Alpha`, and the render gather blends transforms between the last two ticks.
`SystemContext` carries `Tick` (the fixed tick, the wire's unit of time), `Alpha` (View-phase
interpolation fraction), and `Role` (`NetRole::Server`/`Client`, the authority a tick runs under)
beside the always-present `Assets`/`Input`/`Tasks`.

**`NetRole` is a per-world property, not a process-global one.** Each world ticks under its own
authority role, so one process can host a `Server`-tier world and display a `Client`-tier one at
once. `Role` is resolved per world by the `Host`-side role map (`Application::NetState::WorldRoles`)
as it builds each world's `SystemContext` — the `World` itself holds no `NetRole` and does not know
it is replicated. **Authority and transport are orthogonal axes:** the role is *what authority a
world holds*; whether the *process* has a listening/connecting transport (the `--server`/`--join`
launch flag, `GetNetRole()`) is a separate axis. A standalone world is `Server` with no transport
bound — "Server without a transport" is already expressible, so there is no `Local` role and no
offline/online authority fork.

## Transport

The transport seam is socket-free (`Transport.h`). A `Transport` moves opaque byte datagrams
to/from opaque `EndpointId`s; `UdpTransport` hides its sockets behind a pImpl and
`LoopbackTransport` (an in-process pair) carries no platform surface — so every public `Veng/Net/`
header compiles under `include_hygiene` with sockets linked PRIVATE, the same guarantee the
renderer's Native idiom gives. `FaultInjectionTransport` wraps any transport with **seeded,
deterministic** drop/duplicate/reorder faults for device-free adversarial testing. The transport is
**hand-rolled, not vendored** — the reliable-UDP core is a bounded problem and
ENet/GameNetworkingSockets buy surface the replication model doesn't use.

## Connections

`Connection.h` is the per-peer reliability layer. Two channel disciplines over the transport:
**unreliable-sequenced** (latest-wins, never retransmitted — snapshots, input) and
**reliable-ordered** (resent until acked, MTU-capped — handshake, spawn/despawn). Time is injected
through `Update(now)` (no wall clock, fully deterministic under test). **Sockets pump non-blocking
on the main thread at frame boundaries** (receive → tick → send).

`Server.h`/`Client.h` are the connection lifecycle. A `Net::Server` listens/accepts/denies; a
`Net::Client` connects. The handshake is **two-tier** (`Handshake.h`): a **connection tier**
establishes the process↔process link — the connect request carries `Net::ProtocolVersion` (**2**, the
version the multiplexed wire introduced) + the active pack's content digest, rejected loudly on a
mismatch (the `VengModuleAbiVersion` discipline on the wire, so the wire carries only asset ids,
never assets) — and a **per-world join tier** joins one world (below). The `ConnectAcceptMessage`
carries **only the assigned connection id**: it no longer bakes in a single level or seat, since
those are per-world and now ride the join reply. Connection ids are server-assigned `u32`s — the
value `Authority::Owner` holds. There is no encryption or authentication — the scope is LAN/trusted
networks; the `Transport` seam is where such a layer would sit.

## Replication

Replication is reflection-driven (`Replication.h`). A type opts in with a **`VE_REPLICATED`** mark
beside its `VE_REFLECT` block (`Transform`, `Viewer`, `Possesses`, `Session` marked builtin; a game
marks its own). A runtime **`NetIdentity`** (server-assigned `u32`, never authored/persisted) is
the wire key; the client keeps a `NetId → Entity` map. `EncodeSnapshot`/`ApplySnapshot` are the
entity-granular codec (per-component `WriteFields` bytes, MTU-packable, replicated `Entity` fields
translated to NetIds and remapped on apply) — **the reflection serializer is the wire codec; there
is no IDL**. `ReplicationServer` diffs the scene per connection (dirty `Authority::Server` entities
gated by per-entity change ticks, sent until acked) and emits reliable **spawn/despawn** (carrying
a prefab `AssetId` when associated via `SetEntityPrefab`, so the client instantiates through the
ordinary prefab path) + unreliable **snapshots** on a snapshot-interval tick. `ReplicationClient`
applies latest-wins, marks replicated entities **`Tier::Remote`**, and buffers each Transform
snapshot for the **View-phase `RemoteInterpolationSystem`**, which renders a remote ~2 snapshot
intervals in the past.

A client promotes the entities its seat owns to **`Tier::Predicted`** and re-runs the real Sim
systems for them each tick (`PredictionHistory` records tick/input/state); on an authoritative
snapshot the **`Net::Reconciliation`** pass compares the recorded prediction at the snapshot's
`LastConsumedInputTick` against the authoritative record (per-field epsilon on spatial leaves,
exact elsewhere) and, on a mismatch, restores + replays the recorded inputs through the real
systems and eases the visual residual through a decaying `PredictionError` render offset (sim never
lies; only the render pose eases). So the local pawn responds on the tick its input is sampled;
remotes stay interpolated.

## The compressed wire

The snapshot wire is compressed behind the codec seam (`DeltaCodec`, `Quantize`, `BitStream`). Per
(connection, entity) the encoder emits **ack-keyed field deltas** — only the leaves that changed
since the connection's acked baseline, a full self-describing record reserved for spawn / a
keyframe cadence / the drift-tolerant fallback; the baseline advances from the bounded unacked send
window on ack. **Quantization** (server-opt-in `GameNetInfo::QuantizeSpatial`) rounds a Transform's
position to fixed-point and its rotation to smallest-three on the wire while the sim state stays
full-float (the reconcile epsilon is kept ≥ the quantum). The **packed `PlayerInput`** encodes
against the seat's context's resolved action list keyed by a context-stack hash, with the
reflection form as the fallback. Every compressed form opts down from a canonical self-describing
form negotiated by an ack or a hash, so drift tolerance and the fixtures survive.

## Interest management

Interest management is a per-connection relevancy filter on the send loop (`Interest`).
`Interest(conn)` = a headless-safe spatial query around the connection's pawn ∪ the
**`AlwaysRelevant`**-marked entities (`VE_ALWAYS_RELEVANT`, on `Session` + `Viewer` builtin) ∪ the
`GameNetInfo::InterestPolicy` hook ∪ the entities the connection owns, with hysteresis + a dwell
floor. Set enter sends the spawn + baseline; leave sends a **despawn-with-reason**
(`DespawnReason::Visibility` vs `Destroyed`) the client tears down side-effect-free (never a death)
and re-baselines on re-entry. `GameNetInfo::InterestRadius` 0 (the default) replicates the whole
world, so interest is opt-in per game.

## Input replication

Input replicates client→server (`Replication.h` input half, `InputFeed.h`). The client stamps its
seat's resolved `PlayerInput` per tick and sends the **last N ticks redundantly** over the
unreliable channel (`InputSendBuffer`, loss-tolerant without retransmission); the server buffers
per connection in a small **`InputJitterBuffer`** (slews toward a target depth, decays edge phases
on an underrun) and feeds the seat's `PlayerInput` at the matching tick, from which the control
system re-derives `Intent` **unchanged**. `InputFeed.h`'s
`StampLocalSeatInput`/`IngestConnectionInputs`/`FeedSeatInputs` are the device-free helpers the
world drive threads. The **authority filter** (`HasAuthority(context, scene, entity)`, a
`SystemContext::Role` × `Authority::Tier` query) gates the authoritative Sim advancers: a
`Server`-tier entity runs only on a `Server` peer, `Local` always locally, `Remote` never — so a
client's Sim never fights the snapshot stream while AI/server `Intent` producers still write
directly.

## The multiplexed transport: three id spaces, one connection

One connection carries **N worlds**, multiplexed at the `Host`. Three id spaces name three different
things and are deliberately not interchangeable:

- **`WorldInstanceId`** — a `WorldRunner` handle for a running world instance, process-local to
  whichever peer holds it (server and client each mint their own). **Process-private; never goes on
  the wire.**
- **`WorldKey`** (`WorldKey.h`) — an opaque **128-bit** value a **consumer** defines to name a world
  *by intent/content*; the engine never interprets it (a UUID, a content hash, a packed seed+coords,
  a zero-extended integer). It is what a client presents to request a world and the key the server's
  get-or-create map is keyed on. It rides **only the join request**, a cold control message, so it
  costs nothing on the hot path. `Net::DefaultWorldKey` is the well-known key the single-world
  convenience joins.
- **`JoinId`** (`u16`, `NetEvents.h`) — a **server-assigned, per-connection** wire tag for one joined
  world, framed ahead of every replicated message. Per-connection scope means it can never name
  another connection's worlds. `ControlJoinId` (0) is reserved for the join-control tier.

Every world-tagged and join-control message rides the **world-multiplexing envelope**
(`WorldEnvelope.h`): `[0x00 marker][JoinId u16 LE][payload]`, a thin framing above the `Connection`'s
two reliability channels — `Transport`/`UdpTransport`/`LoopbackTransport` are untouched, no socket
surface is added, and the decode is bounds-checked (a short/garbage frame returns `nullopt` and is
dropped before any routing). The marker is zero, which `ControlMessageType` never uses, so an
enveloped frame is never mistaken for a connection-tier control message on the shared reliable
channel: the connection handshake rides unenveloped, world/join traffic rides behind the marker.

**Replication is per-world instances, multiplexed at the `Host` — not one object with world-keyed
internals.** A `ServerHost` holds **one `ReplicationServer` per hosted world**, a `ClientHost` **one
`ReplicationClient` per joined world**, each exactly the single-world replication of `Replication.h`,
keyed by connection as before. The `Host` owns the set and does the mux/demux by the envelope's
`JoinId`. So **ack/baseline isolation is structural**: an instance only ever sees its own world's
connections and acks, so one world's ack can never advance a peer world's baseline. This is isolation
of replication **state**, not the wire stream: worlds over one connection still share its two
reliability channels and all worlds tick serially, so a lost reliable message or a sim stall on one
world can still delay a peer sharing its connection or process. The honest guarantee is that each
world **converges** — not that the streams are independent. Per-world channel sequencing is a named
future.

## Hosts & Application wiring

`Host.h` is the world glue. **`ServerHost`** wraps the lifecycle + replication halves and the world
lifecycle. A client joins a world by presenting a `WorldKey`; the host resolves it in a fixed order —
**authorize** (the `Authorize` hook: may this connection join/create this key? default allow-all),
**per-connection cap** (`MaxJoinedWorldsPerConnection`, default 4), **server-wide cap**
(`MaxHostedWorlds`, default 64), then **get-or-create** through the consumer `WorldFactory`
(`WorldKey → ServerWorldResolution`, opening a world on a miss via the runner, **reusing** on a hit so
two connections presenting the same key **converge on one shared instance**). It then assigns a
per-connection `JoinId`, spawns a **`Viewer` seat entity** in that world (`Authority{ Server, Owner =
id }`, no `SeatInput` — the remote path), and replies (`JoinAcceptMessage`) with the world's level, a
**content digest** of the resolved world, and the seat's wire id; the game-mode spawn rule pawns the
pawnless seat with no net awareness. Each world's stream is gated on a **per-world `ClientReady`**,
and inbound datagrams are **demuxed by `JoinId` only to a world the connection was granted** — a tag
naming an ungranted world is **dropped, not routed**. A refused join surfaces a `JoinDenyReason`
(`NotAuthorized` / `PerConnectionCapReached` / `HostedWorldsCapReached` / `NoSuchWorld`) and leaves
the connection live. Pre-registered worlds (`Create` + `AddWorld`) are joinable by key and never
reaped; a **factory-opened world is refcounted by its live-join count**, held warm for
`IdleKeepWarmDwell` (default 5 s) after its last join leaves, then reaped through the `CloseWorld`
hook — so a reconnect or a briefly-emptied shared world re-converges on the warm instance. The
per-join server surface keys by `(conn, join)` — `ReplicationForJoin` / `WorldForJoin` / `SeatFor` /
`IsGranted` / `IsReady` / `JoinsFor` / `CurrentJoin`, with no-arg / `(conn)` forms the current-join
convenience.

**`ClientHost`** owns one `ReplicationClient` per joined world. `Join(WorldKey)` requests a world;
`ClientHostInfo{ WorldKey, AutoJoin }` auto-joins one key on connect (the single-world convenience).
On the join reply it **validates the echoed content digest** against its own reconstruction (the
`WorldDigest` hook), rejecting a mismatch loudly, loads the named level with server-authoritative
authored entities **skipped** (they arrive from the stream), acks the per-world `ClientReady`,
applies that world's spawn/snapshot stream, and wires the Local-tier presentation to its replicated
seat's `Possesses`. **Identity and clock scope per `JoinId`:** each joined world keeps its own NetId
map, prediction history, and **tick-offset controller**, so a client joining two worlds over one
socket keeps a distinct tick lead per world (`TickSync(join)` / `ObserveTickSync(join, tick)`). The
flat `ClientHostInfo` hooks (`LoadLevel` / `OnPossession` / `Prediction` / `Replay` / `Tolerances`)
are shared across joins — a multiplexed client distinguishes joins by the level id (or scene)
`LoadLevel` returns. Both hosts are usable standalone; **`Application` mounts them** as the
plug-and-play path.

`Application` wiring is a launch decision, not a build. `ApplicationInfo::Net` (an
`optional<GameNetInfo>` — `Port`, `MaxConnections`, `SnapshotIntervalTicks`, `InputRedundancyTicks`,
the quantization/keyframe knobs, `InterestRadius`/`InterestPolicy`, and the client `PredictionPolicy`)
tunes the hosts; **activation is a launch flag**, parsed by `LaunchArguments`: `--server` (listen
server) `[--headless]` (dedicated — Sim + net pump, no render tail), `--join <host[:port]>` (client),
and **`--netsim latency=100,jitter=20,loss=5,dup=1,reorder=2`** wraps the constructed transport in a
seeded `SimulatedTransport` for playable adversity — a dev/QA tool shipped in every build, inert
unless set. The single managed world joins by `Net::DefaultWorldKey` (auto-join server-side
pre-registers it; the client auto-joins it), so the one-world session is one joined world and one
`JoinId` — behavior-identical to the pre-multiplex path. **Per-world roles ride
`NetState::WorldRoles`**, a `WorldInstanceId → NetRole` map: `PumpNet()` iterates the net-active
worlds and pumps each through **`PumpNetWorld(WorldInstanceId, NetRole)`** — receive → sim ticks
(feeding buffered input before the Sim phase server-side, stamping resolved input after it
client-side) → send, once per frame. Server input is keyed **per `(connection, JoinId)`**
(`Net::InputBufferKey(conn, join)`; `IngestConnectionInputs` peels the envelope and gates on the
granted set; `FeedSeatInputs(host, buffers, WorldInstanceId, Scene&[, tick])` feeds each world's
seats). A game reaches the layer through `GetNetRole()` (the process transport-arm axis) /
`GetServerHost()` / `GetClientHost()` and the `OnClientPossession(world, pawn)` hook. **Standalone is
a server with no transport** — one authority model always, no offline/online fork; `--server`/`--join`
only add remote connections.

**The honest per-world-clock guarantee.** Each join keeps its own tick-offset estimator, so a client
joining two worlds keeps a distinct tick lead per world. But `ServerHost::Pump` takes **one server
tick for the whole host**, so genuinely different per-world tick *rates* on one host are an
Application-level concern, not something the host drives; the per-`JoinId` clock isolation is
exercised structurally (distinct estimators), not through a genuine different-rate end-to-end
scenario.

## Tests & exemplar

The hello-triangle sample's opt-in multiplayer mode is the consumption exemplar: a
`MultiplayerMode` tag that switches its spawn rule to keep only the player prefab's Local-tier
presentation in a net session, a per-seat pawn reconciler that pawns the listen host's own seat and
each connection's from the shared `netpawn` prefab, and the `OnClientPossession` / host camera
wiring. It consumes the managed `Application` net path, so a `--server`/`--join` launch joins the
single managed world by `Net::DefaultWorldKey` over the multiplexed transport — one joined world, one
`JoinId` — and drives the `ServerHost` through the current-join convenience accessors. The two-world
integration suite (`tests/unit/net_two_world.cpp` — a server scene + client scene over a
`LoopbackTransport`, and a `SimulatedTransport` for the adversity cases, stepped tick-by-tick) is the
authoritative correctness guard and the regression net; it covers the multiplexed and adversarial
cases too — per-world replication-state isolation with convergence, get-or-create convergence on a
shared key, gated inbound demux, per-world ack scoping, the per-connection join cap, the
`MaxHostedWorlds` cap, the join digest-mismatch reject, idle-world reap + warm re-join, the
per-`JoinId` clock, and the stale-`ProtocolVersion` reject. The consolidated
convergence suite (`net_reconciliation.cpp`) runs prediction + rollback + delta + quantization +
interest under combined loss + latency and asserts byte-equal convergence (lossless wire) or
within-quantum convergence after quiescence, with bounded history/baseline. Prediction/rollback,
delta compression + quantization + packed input, and interest management all sit behind the stable
`ActionState`/component shapes and the extensible `Tier` enum. There is no lag compensation, no
transport security, no spectator/replay/host-migration support, and the editor's Play mode is not a
network client.

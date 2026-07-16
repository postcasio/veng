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
establishes the process↔process link — the connect request carries `Net::ProtocolVersion` (**4**, the
version that added the account id to the connect request) + the active pack's content digest,
rejected loudly on a mismatch (the `VengModuleAbiVersion` discipline on the wire, so the wire
carries only asset ids, never assets) — and a **per-world join tier** joins one world (below). The
`ConnectAcceptMessage` carries **only the assigned connection id**: it no longer bakes in a single
level or seat, since those are per-world and now ride the join reply. Connection ids are
server-assigned `u32`s — the value `Authority::Owner` holds.

**Who a connection is, is a `Net::AccountId`** (`AccountId.h`): an opaque persistent 128-bit id the
consumer mints and the engine only compares and hashes (the `WorldKey` discipline applied to
identity). A client presents it once in the connect request (a cold message) — the
`GameNetInfo::Identity` hook resolves the local player's id per process activation, an unset hook
minting a random valid ephemeral id (`GenerateAccountId`, the zero-config posture: reattach and
persistence then key on nothing durable). The server admits it through
`ServerInfo::AdmitAccount` (`GameNetInfo::AdmitAccount` on the managed path): the hook may
normalize or refuse (nullopt → `DenyReason::AccountRefused`); unset accepts-as-presented. **Exactly
one live connection holds an account** — a duplicate presenter is refused
`AccountAlreadyConnected`, the existing binding undisturbed. That refusal is **transient across the
zombie window**: after a crash or silent drop the stale binding stays live until
`Connection::TimeoutInterval` fires, so the reason is documented **retryable** and a consumer's
reconnect flow retries with backoff until the timeout clears it (a liveness-probing takeover and
last-in-wins displacement were both rejected — displacement rules on a spoofable transport are a
griefing primitive). There is no encryption or authentication — the scope is LAN/trusted networks;
the `Transport` seam is where such a layer would sit, and `AdmitAccount` is where a consumer
verifies identity (or wires an allowlist as a stopgap). **The posture's consequence:** whoever
presents an account id *is* that account, so once anything durable keys on it the id is a
capability token — hosting beyond a trusted LAN is unsafe until identity is verified there.

## The session record — reconnecting is reattaching

**A session is an account's sitting with this host** (`Session.h`): a **`SessionRecord`** —
the standing joins the account holds plus its last gameplay world as **(key, factory params,
arrival pose)** — owned by a **`SessionRegistry`** living beside the `WorldDirectory` at the host
tier (an `Application` constructs one, a `ServerHost` borrows it via `ServerHostInfo::Sessions` or
builds a private one). Deliberately **not** a component: the record exists while the account is
offline, spans worlds, and keys by account. It is maintained **as a side effect of the existing
operations**, no consumer bookkeeping: every join/travel resolves a **durability class**
(`SessionDurability`, via `ResolveSessionDurability`) from its `Present` flag and an explicit
`optional<bool> Standing` override — unset resolves to *not presenting is standing*, so a
presenting travel is the gameplay entry (params doubling as the arrival pose until a capture
refreshes it), a non-presenting join is a standing entry (removed by its explicit leave, kept by a
disconnect), and `Standing = false` opts a prefetch/spectate join out of the record entirely. The
record stores the *resolved* class, so a reclassification is a call-site change, never a data
migration. Pose freshness is the consumer's `CaptureTravelPose` hook (`GameNetInfo`), invoked at
disconnect and the debounced save checkpoint with the gameplay join's world + seat.

**Reattach**: on an admitted account with a record, the optional `TransformOnReattach` hook
rewrites the record (the rewrite is kept), standing joins are re-issued as **non-presenting
directed travels** with no leave arm, and the gameplay entry resolves through the directory —
get-or-place by key, the factory re-running with the recorded **params** when the key misses (a
reaped dynamic world re-materializes, or a placement policy re-matches a live neighbor) — then a
presenting directed travel delivers the recorded **pose** back (`ClientHost::ArrivalPose`). A
gameplay resolve failure (denial, caps) clears the entry with a logged reason and the client lands
at its front door; a persisted entry whose params/pose carry a `TypeId` unknown to the type
registry is untrusted and cleared the same way. Standalone reattach is the same registry with no
wire: `Application` bootstrap consults the local account's record and resolves it through the
local directory, so single-player continue and multiplayer reattach are one code path.

**Durability** is the `LoadSession`/`SaveSession` hook pair (`GameNetInfo`): the blob is the
record's reflection-binary encoding (`EncodeSessionRecord`/`DecodeSessionRecord`, schema-tolerant),
the engine owns *when and what* (load on first admit; save on disconnect, on `StopNet`/teardown,
and debounced at the checkpoint), the consumer owns *where*. No hooks → records live for the
process lifetime, the zero-config LAN posture.

## Replication

Replication is reflection-driven (`Replication.h`). A type opts in with a **`VE_REPLICATED`** mark
beside its `VE_REFLECT` block (`Transform`, `Viewer`, `Possesses` marked builtin; a game
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
**`AlwaysRelevant`**-marked entities (`VE_ALWAYS_RELEVANT`, on `Viewer` builtin) ∪ the
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
**authorize** (the `Authorize` hook over a **`Net::JoinRequestInfo`** — the connection, its admitted
account (always valid: admission precedes authorization), the key, and the payload; default
allow-all), **per-connection cap** (`MaxJoinedWorldsPerConnection`, default 4), then **get-or-place**
through the placement policy, opening a fresh bucket through the consumer `WorldFactory`
(`WorldKey → ServerWorldResolution`) when the policy asks for one, bounded by the **server-wide cap**
(`MaxHostedWorlds`, default 64). It then assigns a
per-connection `JoinId`, spawns a **`Viewer` seat entity** in that world (`Authority{ Server, Owner =
id }`, no `SeatInput` — the remote path, stamped with a **`SeatAccount`** — a builtin,
**non-replicated** component, so the account id stays server-local and is never broadcast to world
members; a game wanting a public identity replicates its own display component), and replies
(`JoinAcceptMessage`) with the world's level, a
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
convenience — and the identity pair `AccountFor(conn)` / `ConnectionFor(account)` (single-valued
because one live connection holds an account; a reconnect re-points it).

**The world lifetime policy is the role-neutral `WorldDirectory` (`Veng/WorldDirectory.h`).** The
`WorldKey → live-instance` map, the get-or-place resolution order, the presence refcount, the keep-warm
dwell, the idle reap, and the caps live in a directory that runs in **every role, transport or none**: a
`ServerHost` **borrows** one (`ServerHostInfo::Directory`; unset builds a private one from the host's
caps/hooks, so a stand-alone host is unchanged), and a standalone `Application` **constructs** one and
resolves its travels through it. The host reports live joins as presence (`AddJoin`/`RemoveJoin`); the
directory's **other presence source is pins** (`Pin`/`Unpin`) — the presentation drive pins the world a
viewport shows, so "a presented world is never reaped" and "a hosted world is refcounted by joins" are
one refcount. Both sources carry the presence's **account**: a join records the connection's admitted
account, a pin the local player's (`Application::GetLocalAccount`, resolved through the `Identity`
hook at bootstrap; a headless `--dedicated` host resolves none — the host is nobody), so
**`MembersOf(WorldKey)`** unions the accounts present across a key's buckets — the membership
primitive — with the listen host's own player a first-class member beside connected ones. A bucket at zero presence starts its dwell; past it the directory invokes the consumer
**`CloseWorld` hook first, then `WorldRunner::CloseWorld`** — the hook-before-teardown ordering is the
persistence capture point, guaranteed in every role.

**Get-or-place: a `WorldKey` maps to N buckets, resolved by a placement policy.** The map is
`WorldKey → [WorldInstance]` — a key may have several live **buckets**, each (host-side) a full instance
with its own `ReplicationServer` (replication-state isolation is structural per bucket; the buckets still
tick serially in the host). On a join the directory offers the key's live buckets (each a `WorldPlacement`
carrying its presence count **and its recorded `TravelPayload`**) to the `Placement` policy
`(WorldKey, connection, payload, buckets) → optional<WorldInstanceId>`: returning an existing bucket
converges on it, returning `nullopt` opens a fresh bucket through the `WorldFactory`
(`(WorldKey, payload) → ServerWorldResolution`, bounded by `MaxHostedWorlds`). The **default policy is
convergence** — one bucket per key — so a host that sets no capacity is byte-identical to a 1:1
get-or-create map. The built-in **capacity policy** is driven by one knob, **`MaxPlayersPerInstance`**
(`0` = no max, the default = pure convergence): a value > 0 places a joiner into the first bucket under
capacity and opens a fresh bucket when every existing one is full. A consumer may supply its own
`Placement` for a different fill rule — comparing the requester's payload against each live bucket's
recorded params is the **proximity match** enabler. Placement is **purely server-side and off the wire**
— the client only ever names the `WorldKey` (and its opaque payload); which bucket it lands in is the
server's decision, identified per-connection by the `JoinId`, and the echoed content digest is identical
across buckets of one key. A bucket that empties reaps per the idle keep-warm dwell and drops out of the
key's list, its peers untouched.

**The travel payload and server-directed travel.** `Net::Blob` (`Blob.h`) is the one opaque
`{ TypeId; bytes }` shape for every consumer value the engine **moves but never decodes** — travel
params and game messages alike; `Net::TravelPayload` aliases it (the travel surfaces' spelling). As
the travel payload it rides the **join request** into
`Authorize`/`Placement`/`WorldFactory`, is recorded on the bucket, and is **echoed in the join reply**
so a client's factory-parameterized reconstruction has its inputs — a world can be parameterized by data
no client-derivable key encodes (a drop-out position). A client that must let the server resolve such a
key sends a **travel-request** control message; the server replies a **directed-travel**
`{ Leave, Join, Payload }` (also available unprompted via `ServerHost::DirectTravel`): the client joins
`Join` and, once that join is ready, leaves `Leave` — **make-before-break**, so a denied join leaves the
client exactly where it was. `Application::Travel(TravelInfo)` is the one primitive across standalone
(directory resolve → present-on-ready rebind → pin destination / unpin departed), client (travel-request
→ directed travel), and listen host; the `TravelRequest` request component lowers onto it.

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

**Adopt-in-place, leave, and the swap.** `ClientHost::JoinInto(key, scene)` is the **adopt** join:
the reply loads no level — the borrowed `scene` is the client's already-standing reconstruction, and
the stream applies into it (the digest is still validated). `ClientHost::Leave(join)` /
`Application::LeaveWorld(join)` is the **scene-preserving leave**: it destroys exactly that join's
wire-owned spawned set (walking the `ReplicationClient`'s `NetId → Entity` map, recursive
`DestroyEntity`), *releases* its adopted anchor bindings (the claimants survive), demotes the
predicted set, drops the per-join state, and sends a **leave-notice** (`JoinMessageType::LeaveNotice`)
so the server tears down the seat, decrements directory presence, and surfaces a `NetEventType::WorldLeft`
(the connection staying live). The **swap** is make-before-break over one scene: `JoinWorld(to, adopt)`
adopts the destination while the source join stays live and streaming; on the destination's readiness
the possession handoff runs (`OnClientPossession` for its pawn, the source's predicted set demoted and
the destination's promoted in one pump — exactly one predicted set across the boundary); then
`LeaveWorld(from)`. Any failure before the leave aborts with the source untouched (the structural
snap-back). Two joins over one scene keep **disjoint wire-id spaces** (each `ReplicationClient` its own
map); a fresh-world join left then closed reproduces the old teardown.

**Stable-anchor adoption.** A builtin reflected `NetAnchor { u64 Lo; u64 Hi }` (opaque; the game mints
it, not `VE_REPLICATED`) names content **derived on both peers that also carries server-authoritative
state**. An authoritative entity carrying one replicates its anchor in its **spawn record** (a field
beside the prefab id, read before any entity is created). On the client an anchored spawn resolves a
**claimant** — a live local entity carrying the equal anchor, found by scanning `View<NetAnchor>` —
and **adopts** it: the wire id maps to the claimant and the record's components apply onto it (the
types the stream *added* recorded for release); a claimant-less spawn falls back to an ordinary
wire-owned spawn with a one-shot warning. Binding is **single-source** — one live join binds a claimant
at a time (a shared `AnchorBindings` registry across a client's joins), and a second live join binding
an already-bound claimant, or two live claimants of one anchor, is a fatal assert. Release (on leave or
despawn) never destroys the claimant: it removes exactly the stream-added component types, keeps the
pre-existing values, and frees the anchor for the next join.

`Application` wiring is a launch *or runtime* decision, not a build. `ApplicationInfo::Net` (an
`optional<GameNetInfo>` — `Port`, `MaxConnections`, `SnapshotIntervalTicks`, `InputRedundancyTicks`,
the quantization/keyframe knobs, `InterestRadius`/`InterestPolicy`, the client `PredictionPolicy`, the
identity hooks `Identity` / `AdmitAccount`, plus
the hosting hooks `WorldFactory` / `Authorize` / `Placement` / `MaxPlayersPerInstance` / `CloseWorld`
and the `MaxHostedWorlds` / `MaxJoinedWorldsPerConnection` / `IdleKeepWarmDwell` caps) tunes the hosts;
**activation is a launch flag or a runtime call**. `LaunchArguments` parses `--server` (listen server)
`[--headless]` (dedicated — Sim + net pump, no render tail), the first-class **`--dedicated`** (the
honest name for `--server --headless`), `--join <host[:port]>` (client), and **`--netsim
latency=100,jitter=20,loss=5,dup=1,reorder=2`** wraps the constructed transport in a seeded
`SimulatedTransport` for playable adversity — a dev/QA tool shipped in every build, inert unless set.
The same host construction is exposed as runtime operations for a menu-driven flow: **`StartHosting()`**
mounts the `ServerHost` on the managed world after boot (mirrors `--server`, with the `GameNetInfo`
hooks), **`Connect(host, port)`** mounts the `ClientHost` against an endpoint (mirrors `--join`), and
**`StopNet()`** drops the host back to standalone — a process that calls none of them, or that used a
launch flag, behaves exactly as before. The managed viewport re-points to another open world at runtime
through **`RebindManagedViewport(index, world)`** (deferred to the top of frame, like a reconfigure). The single managed world joins by `Net::DefaultWorldKey` (auto-join server-side
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

## The game message channel

Messages are the **event complement to replicated world-state**: named, reliable-ordered,
connection-scoped opaque `Net::Blob`s the engine moves and never interprets (`Messages.h`) — what
state structurally cannot carry: pre-membership delivery (an invite must reach a connection before
it holds the group's join), history a latest-wins snapshot legitimately skips (chat), and
request/response. **Messages carry no state** — state over messages would rebuild replication by
hand. A **`ChannelId`** (a minted `u64`, the `SystemId` id family) names each channel; the framing
is a `MessageEnvelope { Channel, Size }` message kind on the reliable-ordered channel under
`ControlJoinId` (the join-control tier's thin-envelope discipline; decode bounds-checked, a
truncated or padded frame drops before routing). Channels share the connection's one reliable
stream, so ordering is **per connection across all channels**; per-channel sequencing is a named
future. Delivery is **reliable-ordered within a live connection, at-most-once across its
lifetime** — no engine retry, no offline queue (a mailbox is state and belongs in a world or a
store). Topology is client → server and server → connection(s) only — no client ↔ client; the
client → server direction doubles as the write lane into host-side services (a remote client's
state-world write is a channel request the owning service validates and applies). The surfaces
live on the hosts: `RegisterChannel` (one handler per channel; an unregistered channel drops with
a one-shot log), `ServerHost::Send` by connection or by **account** (via `ConnectionFor`; the
listen host's own local account, `ServerHostInfo::LocalAccount`, loops back to the local handler
connection-free), `SendToWorldMembers` (fan-out over the directory's `MembersOf`), and
`ClientHost::Send`. Bounds fail loudly, never silently: max payload is
`Net::MaxMessagePayloadSize` (the reliable message bound minus the framing, ~1.1 KiB — no
fragmentation), a per-connection **outbound** queue cap (256 messages / 64 KiB, whichever first)
fails further sends, a per-connection **inbound** per-pump budget (256 messages / 64 KiB) drops
the flooding connection with a logged reason, and a send to an unknown or disconnected account
fails immediately. **Receipt is frame-safe**: the pumps only queue inbound messages;
`DeliverMessages()` dispatches them — `Application` calls it at its top-of-frame request-drain
slot, so a handler observing scene state never runs mid-tick.

## Tests & exemplar

The hello-triangle sample's opt-in multiplayer mode is the consumption exemplar: a
`MultiplayerMode` tag that switches its spawn rule to keep only the player prefab's Local-tier
presentation in a net session, a per-seat pawn reconciler that pawns the listen host's own seat and
each connection's from the shared `netpawn` prefab, and the `OnClientPossession` / host camera
wiring. It consumes the managed `Application` net path, so a `--server`/`--join` launch joins the
single managed world by `Net::DefaultWorldKey` over the multiplexed transport — one joined world, one
`JoinId` — and drives the `ServerHost` through the current-join convenience accessors. It registers
one demo message channel (a minted `ChannelId`; a ping/notify round-trip carrying a reflected value
packed through the field serializer) as the canonical message-channel usage. Its `--name
<s>` launch token supplies the `Identity` hook as a stable hash of the name, so relaunching with the
same name presents the same account; without it the process-random ephemeral default stands. The two-world
integration suite (`tests/unit/net_two_world.cpp` — a server scene + client scene over a
`LoopbackTransport`, and a `SimulatedTransport` for the adversity cases, stepped tick-by-tick) is the
authoritative correctness guard and the regression net; it covers the multiplexed and adversarial
cases too — per-world replication-state isolation with convergence, get-or-create convergence on a
shared key, gated inbound demux, per-world ack scoping, the per-connection join cap, the
`MaxHostedWorlds` cap, the join digest-mismatch reject, idle-world reap + warm re-join, the
per-`JoinId` clock, the stale-`ProtocolVersion` reject, and the account-identity band (the admitted
account through `Authorize`/`SeatAccount`/the host accessors, the `AdmitAccount` refusal and
normalization, the duplicate-live refusal with the first connection undisturbed, reconnect
re-binding across a fresh `ConnectionId`, the zombie-window refusal clearing on timeout,
`MembersOf` across joined worlds with a local account beside connected ones, and the minted
ephemeral default) — and the message-channel band (per-channel ordered exactly-once delivery under
seeded drop/reorder while two worlds' snapshots interleave, pre-membership delivery, the
disconnected-account and oversize send failures, both outbound caps, the inbound flood drop
sparing the peer connection, the truncated-frame drop, the one-shot unregistered-channel log,
frame-safe receipt under a tick guard, and the local-account loopback + members fan-out). The consolidated
convergence suite (`net_reconciliation.cpp`) runs prediction + rollback + delta + quantization +
interest under combined loss + latency and asserts byte-equal convergence (lossless wire) or
within-quantum convergence after quiescence, with bounded history/baseline. Prediction/rollback,
delta compression + quantization + packed input, and interest management all sit behind the stable
`ActionState`/component shapes and the extensible `Tier` enum. There is no lag compensation, no
transport security, no spectator/replay/host-migration support, and the editor's Play mode is not a
network client.

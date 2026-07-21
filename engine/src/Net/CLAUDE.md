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
establishes the process↔process link — the connect request carries `Net::ProtocolVersion` (**5**, the
version that added the opaque account profile to the connect request) + the active pack's content digest,
rejected loudly on a mismatch (the `VengModuleAbiVersion` discipline on the wire, so the wire
carries only asset ids, never assets) — and a **per-world join tier** joins one world (below). The
`ConnectAcceptMessage` carries **only the assigned connection id**: it no longer bakes in a single
level or seat, since those are per-world and now ride the join reply. Connection ids are
server-assigned `u32`s — the value `Authority::Owner` holds.

**The stale-peer story is not a uniform loud deny — it splits on message shape.** A connect request
that **decodes** but carries a different `ProtocolVersion` is denied loudly with
`DenyReason::ProtocolMismatch` (the tested case: a peer whose request has the current wire layout
but a mismatched version number). But a peer whose request is a **different length** — a binary built
against a connect request with fewer fields — runs `DecodeConnectRequest` out of bytes: it returns
`nullopt` and the frame is **dropped silently as malformed**, no deny emitted, so that peer never
sees `ProtocolMismatch` — it simply **times out** at `Connection::TimeoutInterval`. So the wire break
fails loud only when the two requests are the same length; a length-changing bump is caught by the
decode as a silent drop-and-timeout. Both are safe (no stale peer is admitted); they differ in
whether the peer is told why.

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
verifies identity (or wires an allowlist as a stopgap). **The posture's consequence, and it grows
once records key on the account:** whoever presents an account id *is* that account — and with the
session record and a durable store keyed on it, that includes **write-through to its persisted
state** (its saved gameplay world, its standing joins, whatever a consumer persists under the id). So
the account id is a **capability token**: possession is authority, unverified, until an
authentication layer lands behind the `AdmitAccount` seam. Hosting beyond a trusted LAN is unsafe
until then.

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
wire: `Application` consults the local account's record and resolves it through the
local directory, so single-player continue and multiplayer reattach are one code path.

The two paths **differ on failure, deliberately**. A hosted reattach *clears* the denied gameplay
entry (`SessionRegistry::ClearGameplay`) — the connection is live, so the client is directed to its
front door and the stale entry is not worth retrying. `Application::RestoreLocalSession` **keeps**
the record: it logs the reason, leaves the currently presented world up (at bootstrap that is world
#0, the startup level) and returns, so the next restore retries the same record. A standalone
resolve failure is typically transient — an unmounted pack, a factory that has not been installed
yet — and persisting a cleared record would destroy the continue permanently on one bad run. A
denied *standing* entry is skipped individually and the remaining entries still resolve, on both
paths.

**When the standalone restore runs is the consumer's call.** `GameWorldInfo::RestoreLocalSessionOnBoot`
(default `true`) runs it at bootstrap — the continue-style posture, zero game code. Set `false`, world
#0 (the startup level) stays presented and the game drives the identical path itself through the public
**`Application::RestoreLocalSession()`** once the store its record lives in is open — what a front-end
that owns the first travel wants, and what a player-less dedicated host (no local account to restore)
sets. The restore is **reversible**, not merely idempotent: **`Application::ReleaseLocalSession()`**
drops the standing-join local pins the restore took and evicts the account's cached record
(`SessionRegistry::Evict`, saving a dirty record first; `Clear` does every account), so a subsequent
restore reloads through `LoadSession` against whatever store is now open and carries no presence from
the released one. The record's standing list is untouched — release ends the process's *hold* on the
worlds, not the account's memberships (`LeaveStanding` does that) — so *open store → restore → … →
release → open next store → restore* is clean.

**Durability** is the `LoadSession`/`SaveSession` hook pair (`GameNetInfo`): the blob is the
record's reflection-binary encoding (`EncodeSessionRecord`/`DecodeSessionRecord`, schema-tolerant),
the engine owns *when and what* (load on first admit; save on disconnect, on `StopNet`/teardown,
and debounced at the checkpoint), the consumer owns *where*. No hooks → records live for the
process lifetime, the zero-config LAN posture.

The engine ships a **default backend** for that *where*: `Veng/Persistence/SessionStore.h` binds
the pair onto a `Store` (`RegisterSessionFamily` + `MakeSessionHooks`), one record per account in
the engine-minted `veng.sessions` family, keyed on the full 128-bit account id. It takes a
`function<Store*()>` **source** rather than a reference, since the registry is built once at init
while a store is scoped to an open slot — and a source returning null *is* the memory-only posture,
so the zero-config default above is expressible rather than special-cased. The binding is opt-in;
the raw hook pair remains the extension seam for a consumer with bespoke storage. See
[src/Persistence/CLAUDE.md](../Persistence/CLAUDE.md).

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
allow-all), **per-connection cap** (`MaxJoinedWorldsPerConnection`, default 8 — budgeting the
standing-join architecture, the arithmetic documented on `GameNetInfo`), then **get-or-place**
through the placement policy, opening a fresh bucket through the consumer `WorldFactory`
(`WorldKey → ServerWorldResolution`) when the policy asks for one, bounded by the **server-wide cap**
(`MaxHostedWorlds`, default 64). It then assigns a
per-connection `JoinId`, spawns a **`Viewer` seat entity** in that world (`Authority{ Server, Owner =
id }`, no `SeatInput` — the remote path, stamped with a **`SeatAccount`** — a builtin,
**non-replicated** component, so the account id stays server-local and is never broadcast to world
members; a game wanting a public identity replicates its own display component), and replies
(`JoinAcceptMessage`) with the world's level (the invalid id for a **level-less data world** — no
placebo level, no stub asset), its **`SimTickRate`**, a
**content digest** of the resolved world, and the seat's wire id; the game-mode spawn rule pawns the
pawnless seat with no net awareness. Each world's stream is gated on a **per-world `ClientReady`**,
and inbound datagrams are **demuxed by `JoinId` only to a world the connection was granted** — a tag
naming an ungranted world is **dropped, not routed**. A refused join surfaces a `JoinDenyReason`
(`NotAuthorized` / `PerConnectionCapReached` / `HostedWorldsCapReached` / `NoSuchWorld`) and leaves
the connection live. Pre-registered worlds (`Create` + `AddWorld`) are joinable by key and never
reaped; a **factory-opened world is refcounted by its live-join count**, held warm for
`IdleKeepWarmDwell` (default 5 s; a resolution may name its own patience through
`ServerWorldResolution::IdleDwell` — a long-lived data world dwells minutes while a gameplay bubble
keeps the seconds default) after its last join leaves, then reaped through the `CloseWorld`
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

**A non-presenting *local* standing travel counts as directory presence.** A remote join calls
`AddJoin`, so the directory counts it; the connectionless local player has no join to report, so a
non-presenting local (listen-host / standalone) standing travel takes a **local-account pin** instead.
`Application::TravelStandalone`, on a **standing** durability, calls `AcquireLocalStanding` — a `Pin`
for `GetLocalAccount` on the resolved bucket, tracked keyed by `WorldKey` (idempotent per key, matching
`SessionRegistry::RecordStandingJoin`'s dedupe) — so the local standing membership feeds the same
presence refcount and `MembersOf` a remote join's does. The standalone continue warms its restored
standing joins the same way. Because the pin is the local player's, a world with both a local standing
member and remote joins survives until **both** are gone: each remote departure decrements presence,
but the local pin holds the bucket above zero, so `IdleSince` is never stamped under a still-present
local member. Release is explicit — the local player has no disconnect — through
**`Application::LeaveStanding(key)`**, the connectionless counterpart of a client leave: it `Unpin`s the
tracked bucket and drops the key from the session's standing list, so the bucket's dwell owns its fate
once every other presence is also gone. A bucket that only ever held the local member reaps when that
member leaves via `LeaveStanding`; until then it is legitimately kept warm, not leaked.

**A consumer holds a world warm by key with an accountless infrastructure pin.**
**`Application::HoldWorldWarm(key)`** resolves the key through the directory (get-or-place, opening
through the factory on a miss — a **requester-less** resolve, so a factory keying off the requester
sees the invalid account) and takes an **accountless `Pin`** on the resolved bucket, tracked keyed by
`WorldKey` and idempotent per key. It is the infrastructure counterpart of the account-scoped
standing-join pin above: `LeaveStanding`'s pin carries the local account (a standing *membership*, so
`MembersOf` reports it), while a warm pin **belongs to no account** and is never a member — it holds a
data world resident for the process itself, not a player. Both feed the one presence refcount, so a
world with a warm pin and any joins stays warm until every pin and join is gone.
**`Application::ReleaseWorldWarm(key)`** drops the pin and lets the dwell own the world's fate once
every other presence is gone. This is the sanctioned way to keep a data world resident — preferred
over inflating `ServerWorldResolution::IdleDwell` toward infinity, which only stretches the reap
window rather than expressing an explicit hold.

**Get-or-place: a `WorldKey` maps to N buckets, resolved by a placement policy.** The map is
`WorldKey → [WorldInstance]` — a key may have several live **buckets**, each (host-side) a full instance
with its own `ReplicationServer` (replication-state isolation is structural per bucket; the buckets still
tick serially in the host). On a join the directory offers the key's live buckets (each a `WorldPlacement`
carrying its presence count **and its recorded travel payload (`Net::Blob`)**) to the `Placement` policy
`(WorldKey, connection, payload, buckets) → optional<WorldInstanceId>`: returning an existing bucket
converges on it, returning `nullopt` opens a fresh bucket through the `WorldFactory`
(`(JoinRequestInfo, WorldKey, payload) → ServerWorldResolution`, bounded by `MaxHostedWorlds`). The
factory receives the resolving **`Net::JoinRequestInfo`** ahead of the key and payload it also carries,
so a world can **project the requester's account at open** — per-account state materialized for the
joining player. A resolve not driven by a particular join (an `Application::HoldWorldWarm` warm
pre-open) passes a **requester-less** request — the invalid account — which a factory reads as "no
specific requester" rather than a real player. The **default policy is
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
params and game messages alike; the travel surfaces spell it `Net::Blob`. As
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
authored entities **skipped** (they arrive from the stream) — or, for a reply naming **no level**
(a scene-less data world), installs an empty stream-populated scene through the `OpenEmptyWorld`
hook, `LoadLevel` never invoked and the digest validated the same — acks the per-world
`ClientReady`, applies that world's spawn/snapshot stream, and wires the Local-tier presentation to
its replicated seat's `Possesses`. **Identity and clock scope per `JoinId`:** each joined world
keeps its own NetId map, prediction history, and **tick-offset controller** — constructed at the
`SimTickRate` its join reply carried, so a 1 Hz data world's RTT converts into whole slow-tick
leads while a 60 Hz world sharing the socket keeps its own fast lead (`TickSync(join)` /
`ObserveTickSync(join, tick)`; `ClientHostInfo::TickSync.TickRate` is only the pre-reply default). The
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
honest name for `--server --headless`), `--join <host[:port]>` (client), **`--no-render`** (drop the
render tail on a headless run whose frames nothing reads — a headless *client* has no `ServerHost` and
so otherwise renders every frame, which on a debug build can starve the net pump), and **`--netsim
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
joining two worlds keeps a distinct tick lead per world. `ServerHost::Pump` stamps each hosted world's
snapshot cadence and ack baselines in **that world's own sim tick** — its Scene's change tick, the
tick its writes were stamped at — so worlds of genuinely different `SimTickRate`s share one host with
each clock isolated end to end: a 1 Hz world and a 60 Hz world on one host each advance their own
cadence, baselines, and header ticks, and the two-world suite exercises the different-rate case, not
only the structural estimator isolation.

**Replication cadence stamps in per-world tick space.** `Pump` reads each hosted world's own change
tick and stamps its snapshot cadence and per-connection ack baselines against it, regardless of the
host pump rate. A world ticking far below the host pump rate — a data world at, say, 1 Hz against a
60 Hz host — has its writes fall on a snapshot-interval tick *in its own tick space*, so they qualify
as delta changes against its own advancing baseline and ride the ordinary delta cadence rather than
only the periodic **keyframe** cadence; and that world's join **tick estimator tracks the world's own
clock**, because the header tick it observes advances in the world's tick space, matching the client
world's `SimClock`, so it does not re-sync per host frame. Component change ticks already live in the
world's tick space (a Scene stamps a write with its own sim tick), so aligning the baseline and header
to the same space is what closes the gap; an unstamped component (change tick 0) is `≤` any baseline
in either space, so it replicates through spawn and keyframes exactly as before. A world at or near the
host rate is byte-identical to a host-tick stamp, since its own tick and the host tick coincide.

**Snapshot emission is one per qualifying world tick, not one per host pump.** Stamping the cadence
in the world's tick space is necessary but not sufficient on its own: `ReplicationServer::Generate`
gates on the world's own sim tick, but `Generate` runs once per host `Pump` — faster than a sub-rate
world advances — so a single qualifying tick persists across many consecutive pumps, and the
`tick % SnapshotInterval == 0` gate would re-emit a fresh snapshot on every one of them (a
`SnapshotInterval` of 1 emitting a snapshot every pump, ~60× intended; the keyframe cadence inflating
the same way). Each connection records `LastSnapshotTick` — the world tick its last snapshot went out
on — and skips a repeat of it, collapsing emission to **one snapshot per qualifying world tick
regardless of pump rate**; the sentinel (never a real tick) fires the first snapshot, including one at
tick 0. A world ticking at or above the host rate advances its tick every pump and is unaffected; a
slow frame running zero sim steps no longer re-emits the prior tick's snapshot. So a sub-rate world's
idle traffic converges to zero between changes rather than spending a pump-rate burst.
`ServerHost::ReplicationBytesForWorld(WorldInstanceId)` reports a world's accumulated replication
traffic (snapshots + spawns/despawns) over the host's lifetime — the instrument that measures a
low-rate world's delta cost between keyframes and proves its idle traffic flat between changes.

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

## Resolving a world: which instances, which join, which host

Membership answers "who is in this key"; **resolution answers "is this key live, and where"** — the
question every world-addressed feature starts with, and the one a consumer otherwise answers by
mirroring the directory's own map against registration and close notifications.
**`WorldDirectory::InstancesOf(WorldKey)`** returns the key's live buckets as
`std::span<const WorldPlacement>` — each carrying its `WorldInstanceId`, its presence, and its
recorded travel payload, exactly the shape the placement policy is offered. The read is **plural by
construction, not by caution**: `MaxPlayersPerInstance` opens a fresh bucket once every existing one
is full, so a key legitimately holds several live instances and a singular resolve would be undefined
the moment capping turned on. A caller wanting the sole bucket of an uncapped key reads the first
element. The view is directory-owned scratch, valid until the next `InstancesOf`; it is not a live
window onto the map, so a caller holding it across a `Resolve` or a reap copies what it needs. A
bucket stops appearing exactly when `ReapIdle` drops it — the same moment the `CloseWorld` hook fires.

The directory is reachable from outside the engine through **`Application::GetWorldDirectory()`**
(null before the managed world is started, and always for an application configured without one).
The directory is the application's in **every role** — a standalone app resolves travels through it,
a mounted `ServerHost` borrows it, a joining client still owns one — so the accessor, not a
per-query façade, is the reach: the queries are already the directory's own vocabulary and a façade
would duplicate every one of them.

The two host-side resolves complete the set. **`ClientHost::JoinForKey(WorldKey)`** is the inverse of
`JoinKey(JoinId)`: the client's own join for a key, or nothing — single-valued because a repeat
`Join` of a joined key is idempotent, and empty for a key it left. **`ServerHost::IsReplicatingWorld(WorldInstanceId)`**
is the **deferral gate**: true while this host holds a live replication instance for the world (opened
at `Create`, added through `AddWorld`, or opened by the factory) and false once the directory reaps it
and the host drops its replication state. Work that must wait until a world actually replicates gates
on it rather than scanning `Connections()` × `JoinsFor` × `WorldForJoin` for a join that resolves
there. It is **narrower than the directory's `Contains`**: a shared directory may hold a bucket some
other role opened that this host does not replicate.

All four are **host-local reads over state the tier already maintains** — no wire format, no new
bookkeeping, no lifecycle change.



<!-- -- carve 74/01 -- -->

<!-- -- carve 74/02 -- -->

## The admission profile

**The engine interprets the account id; every other account fact is a game-defined, engine-opaque
payload.** The `GameNetInfo::PresentProfile` hook yields one `Net::Blob` per process activation, the
account-scoped counterpart of the join-scoped travel payload: the client puts it in its **connect
request**, the host holds it per admitted account, and only game code ever decodes it. Presenting is
opt-in and **presenting nothing is byte-identical to the pre-profile handshake apart from the empty
blob header** — the default is none.

The host surfaces it two ways: **`ServerHost::ProfileOf(AccountId)`** (nullptr when none was
presented) and **`Net::JoinRequestInfo::Profile`**, a borrowed `const Blob*` beside the join's own
`const Blob& Payload` — a pointer because `JoinRequestInfo` is a pure value/view built per resolve,
per `Authorize`, and per placement candidate, so an owning blob would deep-copy the profile on each,
and because "none presented" then has one spelling across both surfaces instead of an empty-blob
sentinel on one and `nullptr` on the other.

**The local account presents too.** A listen host and a standalone app perform no connect, so nothing
would ever invoke the hook for their own account — yet `JoinRequestInfo` also serves the
transport-less local resolve. `Application` evaluates `PresentProfile` once at bootstrap beside
`Identity` (`GetLocalProfile()`), hands it to the host as `ServerHostInfo::LocalProfile`, and threads
it onto every local-arm `JoinRequestInfo`, so `ProfileOf(localAccount)` and a local resolve answer
exactly as a remote join does across all three topologies. The local entry is owned by
`ServerConnectionId`, which no connection ever holds, so no teardown clears it.

The posture is three properties:

- **Opaque.** The engine never decodes the blob and no engine path branches on its `TypeId`; the
  bytes round-trip untouched, nothing added or stripped.
- **Host-terminal.** The profile is never replicated, never forwarded to a peer, and never enters
  world state by engine action. A game wanting peer-visible identity replicates its own component —
  the documented pattern, the same one the non-replicated `SeatAccount` establishes.
- **Bounded.** `Net::MaxProfileBytes` (`MaxReliableMessageSize` minus `ConnectRequestOverhead`) is
  published and documented on the hook. The connect request is a single reliable message with no
  fragmentation, so an over-budget profile cannot be split: it **refuses the connect with
  `DenyReason::ProfileTooLarge`** rather than truncating, because a silently shortened opaque payload
  is a corruption the consumer that authored it cannot detect. The refusal happens at both ends — a
  client whose own profile is over budget refuses locally and sends nothing, and a host refuses an
  over-budget presented profile at the door.

The bytes are **client-authored data under the existing admission trust posture**, unchanged by the
profile: a host may assert what a profile says, never verify it, until an authentication layer lands
behind `AdmitAccount`.

The profile is **connection-scoped state the engine does not persist**: it is re-presented on
reconnect. Because an account can briefly hold two connections when a reconnect beats the old
connection's timeout, each held profile records the connection that presented it — the most recently
admitted connection's profile wins, and a teardown clears the entry only while the departing
connection still owns it.

Carrying the blob grew `ConnectRequestMessage`, whose decoder requires an exact fixed prefix, so
`Net::ProtocolVersion` is **5**.

## Putting a value into a blob

`Net::Blob` is opaque to the engine, so the consumer supplies the codec — and there are only two
codecs worth writing, both shipped in **`Veng/Net/BlobCodec.h`** (a sibling of `Blob.h`, so `Blob`
itself stays a pure value type with no reflection-walker dependency).

**The fixed-layout form** — `EncodeBlob<T>(value, tag)` / `DecodeBlob<T>(blob, expected)` — packs a
trivially-copyable payload's object representation into `sizeof(T)` bytes. **The tag is an explicit
parameter, and that is the design point.** A blob's tag is a *discriminator*, not a description of
the byte layout: a consumer routinely tags a payload with a type other than the one it carries so a
receiver can tell two payloads apart on one channel, and a payload meant to read as absent carries
`InvalidTypeId`. A helper hard-wiring `TypeIdOf<T>()` would exclude every wire struct that is not
itself reflected — most of them are not — and would silently rewrite the discriminator wherever a
consumer cross-tags, breaking the receiver. Passing `TypeIdOf<T>()` expresses the plain case;
passing anything else expresses the real one.

The decode's size test is **`>=`, not `==`**: a longer payload sharing the tag decodes its leading
`sizeof(T)` bytes, and a shorter one reads as absent — the asymmetry a shared channel depends on.
A caller wanting exactness compares the blob's own byte count.

Two contracts ride the fixed-layout form. It is a **same-build convenience, not a versioned wire
contract**: it encodes T's in-memory layout, which differs across compilers, architectures, and any
edit to T, so a payload crossing a build boundary or evolving its fields belongs on the record form.
And **a successful decode guarantees only the tag matched and the byte count sufficed** — nothing
validates the bytes. Blobs arrive from the wire as untrusted input, so a size-matching adversarial
payload `memcpy`'d into a `T` yields out-of-range enums, `bool`s that are neither 0 nor 1, and NaN
floats; **the caller must validate every field whose domain is narrower than its representation**.
A payload from an unadmitted peer belongs on the record form. Every rejection — a wrong tag, a short
payload, an empty blob — is `nullopt`, never an assert, per the recoverable-read policy.

**The record form** — `EncodeBlobRecord<T>(value, registry)` / `DecodeBlobRecord<T>(blob, registry)`
— runs a reflected value through `WriteFields`/`ReadFields` and tags the blob `TypeIdOf<T>()`. It is
the schema-tolerant one: a field the record names and T no longer has is skipped, and a field of T
the record omits keeps its default, so a payload whose shape evolves survives in both directions.
`net_blob_codec.cpp` pins both round-trips, the cross-tagged payload, the `>=` prefix decode, and
the drift cases.

## The social toolkit — vocabulary, not a feature

`Veng/Net/Social.h` is the multiplayer-social vocabulary every invite-gated shared world re-derives:
an **`InviteTable`**, a **join judge**, a **presence classifier**, and a **roster diff**. It is pure
value logic over engine types (`WorldKey`, `AccountId`, monotonic seconds) — no socket, no scene, no
clock, no connection — so all of it is unit-testable end to end and compiles under
`include_hygiene`. The two roster templates are header-only; the three non-template pieces live in
`src/Net/Social.cpp`. **No engine system calls any of it**: the engine ships no roster component, no
membership policy, no caps, and no invite flow, and a consumer composes these against whatever
membership model it defines.

**`InviteTable` holds one-shot capability tokens keyed on `(WorldKey, AccountId)`.** `Issue` mints a
token for the pair, replacing any token the pair already holds and restarting its window; `Consume`
removes the entry and succeeds **exactly once** when the presented token matches a live entry;
`IsInvited` is the const listing query; `SweepExpired` / `ClearFor` / `GetCount` are the
housekeeping. **The token is the capability and the pair only the addressing key** — `AccountId` is
documented unauthenticated and spoofable, so admitting on `(world, invitee)` alone would degrade the
table from an unguessable-secret check to an ACL anyone able to name an invitee satisfies. A
**mismatched token fails and leaves the entry standing**, so a wrong guess never burns the invite
the legitimate holder still needs. Time is **monotonic seconds since issue** — an entry is live
while `now - issue < expirySeconds` — so expiry never moves under an NTP step or a user clock
change. Expiry is swept on demand: by `SweepExpired`, or by a `Consume` that lands on a stale entry;
the const query reports an expired entry absent without removing it.

**`JudgeInviteGatedJoin(inRoster, hasInvite, rosterCount, capacity)` is pure and does not consume.**
A roster member always readmits (`AdmitMember`) — a member reattaching after a disconnect is not
competing for a seat, so capacity is not consulted for one. A non-member is judged on its invite
first and its seat second, so an uninvited joiner is `RefuseUninvited` whether or not the roster is
full and learns nothing about occupancy; an invited one is `AdmitByInvite` while a seat remains and
`RefuseFull` otherwise. **The caller consumes the token only on `AdmitByInvite`**, and because
capacity is judged before any consume a `RefuseFull` invitee's one-shot token survives for a retry
once a seat frees. Consuming before judging silently burns it — the ordering guarantee is
externalized by the pure form, so it is stated on the declaration rather than left for callers to
rediscover.

**`ClassifyMemberPresence(inRoster, online, present, connected)` separates a disconnect from a
leave.** A connection loss is not a departure: the member keeps its roster membership and is merely
marked `Offline`, so it returns as a `Rejoin` rather than joining from nothing; a first appearance is
a `Join`; an observation matching the recorded state is `None`. All four inputs are load-bearing —
`online` is what separates `Rejoin` from `None` on a present member and what makes `Offline` fire
once rather than on every sweep tick.

**`DeriveRosterNotices<Key, Payload>(before, after, changed)` derives events from replicated state,
with no message traffic.** Two roster snapshots diff into ordered `Joined` / `Left` / `CameOnline` /
`WentOffline` / `Changed` notices; a member present in both may yield **two** notices in one diff
(a presence notice and a `Changed`), so a payload edit is never lost behind an online-flag flip.
Notices come in `after` order for every row `after` holds, then the `Left` notices in `before`
order; a `Left` carries the earlier snapshot's payload, every other notice the later one's. **The
match key is a template parameter, not `AccountId`** — a replicated roster commonly carries a
display identity and deliberately *not* an account id, which is host-side identity a consumer may
have no intention of putting on the wire, so hard-wiring it would force that disclosure on any
adopter. The **`changed` predicate is likewise a parameter**: a consumer routinely suppresses the
change notice for some payload transitions rather than firing on every inequality. An empty
predicate falls back to `operator!=` where `Payload` supports it and never fires `Changed` where it
does not.

`tests/unit/net_social.cpp` (fast band) carries the cases: issue/consume-once/expiry/replace plus
the wrong-token-survives pin, every judge verdict including reattach-over-capacity and the
`RefuseFull`-leaves-the-token-live pin, the full sixteen-row presence matrix, and the roster diff's
events, both change tests, the two-notices-in-one-diff case, and the empty↔populated edges.

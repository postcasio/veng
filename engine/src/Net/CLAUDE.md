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
`Net::Client` connects. The **handshake carries a protocol version + the active pack's content
digest** and rejects a mismatch loudly — the `VengModuleAbiVersion` discipline on the wire, so the
wire carries only asset ids, never assets. Connection ids are server-assigned `u32`s — the value
`Authority::Owner` holds. There is no encryption or authentication — the scope is LAN/trusted
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

## Hosts & Application wiring

`Host.h` is the world glue. `ServerHost` binds the lifecycle + replication halves to a world: on
accept it spawns a **`Viewer` seat entity** for the connection (`Authority{ Server, Owner = id }`,
no `SeatInput` — the documented remote path), names it (with the level) in the `ConnectAccept`, and
gates its stream on `ClientReady`; the game-mode spawn rule pawns the pawnless seat with no net
awareness. `ClientHost` loads the accepted level with server-authoritative authored entities
**skipped** (they arrive from the stream), acks, applies the spawn/snapshot stream, and wires the
client's Local-tier presentation to its own replicated seat's `Possesses`. Both are usable
standalone; **`Application` mounts them** as the plug-and-play path.

`Application` wiring is a launch decision, not a build. `ApplicationInfo::Net` (an
`optional<GameNetInfo>` — `Port`, `MaxConnections`, `SnapshotIntervalTicks`,
`InputRedundancyTicks`, the quantization/keyframe knobs, `InterestRadius`/`InterestPolicy`, and the
client `PredictionPolicy`) tunes the hosts; **activation is a launch flag**, parsed by
`LaunchArguments`: `--server` (listen server) `[--headless]` (dedicated — Sim + net pump, no render
tail), `--join <host[:port]>` (client), and **`--netsim latency=100,jitter=20,loss=5,dup=1,reorder=2`**
wraps the constructed transport in a seeded `SimulatedTransport` (the `FaultInjectionTransport`
grown a latency/jitter delay queue) for playable adversity — a dev/QA tool shipped in every build,
inert unless set. `Application`'s `PumpNet` runs receive → sim ticks (feeding buffered input before
the Sim phase server-side, stamping resolved input after it client-side) → send, once per frame
after the tick loop. A game reaches the layer through `GetNetRole()` / `GetServerHost()` /
`GetClientHost()` and the `OnClientPossession(world, pawn)` hook. **Standalone is a server with no
transport** — one authority model always, no offline/online fork; `--server`/`--join` only add
remote connections.

## Tests & exemplar

The hello-triangle sample's opt-in multiplayer mode is the consumption exemplar: a
`MultiplayerMode` tag that switches its spawn rule to keep only the player prefab's Local-tier
presentation in a net session, a per-seat pawn reconciler that pawns the listen host's own seat and
each connection's from the shared `netpawn` prefab, and the `OnClientPossession` / host camera
wiring. The two-world integration suite (`tests/unit/net_two_world.cpp` — a server scene + client
scene over a `LoopbackTransport`, and a `SimulatedTransport` for the adversity cases, stepped
tick-by-tick) is the authoritative correctness guard and the regression net; the consolidated
convergence suite (`net_reconciliation.cpp`) runs prediction + rollback + delta + quantization +
interest under combined loss + latency and asserts byte-equal convergence (lossless wire) or
within-quantum convergence after quiescence, with bounded history/baseline. Prediction/rollback,
delta compression + quantization + packed input, and interest management all sit behind the stable
`ActionState`/component shapes and the extensible `Tier` enum. There is no lag compensation, no
transport security, no spectator/replay/host-migration support, and the editor's Play mode is not a
network client.

# Networking: server-authoritative multiplayer

This guide covers building a **client/server multiplayer** game on veng: how the
engine replicates state, what you mark to put a component on the wire, how a
connection becomes a playable seat, and how you launch a listen server, a
dedicated server, and a joining client. It builds directly on two earlier guides —
[Writing gameplay systems](writing-gameplay-systems.md) (the Sim/View split, the
Input → Intent → Movement pattern, the authority filter) and
[Multi-seat input](multi-seat-input.md) (the `Viewer` seat, `SeatInput`,
`Possesses`) — because networking **consumes those seams unchanged**. A remote
player is a seat whose input arrives from the wire instead of a device; nothing
downstream of `PlayerInput` knows the difference.

The live reference is the `hello-triangle` sample's multiplayer mode (its
[`main.cpp`](../../examples/hello-triangle/main.cpp) and
[`netpawn.prefab.json`](../../examples/hello-triangle/assets/prefabs/netpawn.prefab.json)),
and the authoritative correctness proof is the two-world integration suite
([`tests/unit/net_two_world.cpp`](../../tests/unit/net_two_world.cpp)) — a server
scene and a client scene stepped tick-by-tick in one process. Open them beside
this guide.

---

## The model in one picture

```
 CLIENT                                             SERVER
 ──────                                             ──────
 devices → InputMappingSystem → PlayerInput ──wire──► seat's PlayerInput (jitter buffer)
                    (unchanged)                            │
                                                           ▼ control system (unchanged)
 snapshots ◄──wire── replicated component state ◄── Intent → MovementSystem → Transform …
     │                (Authority::Server, dirty-gated,       Sim phase, fixed tick
     ▼                 reflection-encoded)
 spawn/despawn (reliable, by prefab AssetId)
     │
     ▼
 client Scene: Remote-tier entities ── RemoteInterpolationSystem (View) ── render
 local-tier entities (cameras, viewers) simulate locally, never on the wire
```

The commitments this model makes, each of which shapes how you author a game:

- **The server owns truth; clients display it.** This is *not* lockstep — there is
  no cross-machine determinism requirement, and no bug hunt should ever chase float
  divergence. The server simulates; a client renders what the server sends.
- **Standalone is a server with no transport.** The Sim phase is *always*
  authoritative for `Authority::Server` entities. Single-player is the degenerate
  server; there is no offline/online fork to keep in sync. The client is the one
  genuinely distinct role (`NetRole::Client`).
- **The sim ticks fixed; the view runs per frame.** Sim systems step at a fixed
  `SimTickRate` with a monotonic `u64` tick number — "tick 4812" is the wire's unit
  of time. View systems run once per frame with an interpolation alpha. (See
  [Writing gameplay systems](writing-gameplay-systems.md) for the tick model.)
- **Interpolation only, in v1.** A remote entity renders ~2 snapshot intervals in
  the past and interpolates between samples; the local pawn wears its round-trip
  latency. Client-side **prediction, delta compression, and interest management are
  the next phase**, not this one — the honest correctness-first cut.

---

## 1. Mark what replicates — `VE_REPLICATED`

A component crosses the wire only if its type is tagged `VE_REPLICATED`, a mark
that sits beside its `VE_REFLECT` block (the wire codec *is* the reflection
serializer — no second schema, no IDL):

```cpp
VE_REFLECT(::MyGame::Health, 0x…ULL)
VE_FIELD(Current, .DisplayName = "Current")
VE_FIELD(Max,     .DisplayName = "Max")
VE_REFLECT_END();

VE_REPLICATED(::MyGame::Health);   // now Health snapshots to clients
```

The engine marks the builtins a networked game always needs — `Transform`,
`Viewer`, `Possesses`, `Session` — so a pawn's pose and a seat's possession
replicate with no work from you. Mark **your own** gameplay components that carry
server-authoritative state a client must see (health, ammo, team). Deliberately
**not** replicated, and why:

- **`PlayerInput`** flows client→server on its own input channel (the redundant
  last-N-ticks window), not as replicated state.
- **`Intent`** is re-derived every tick server-side from the seat's input — it
  never needs to travel.
- **`SeatInput`**, **`CameraFollow`/`CameraLook`** are client-local view/device
  state, derived per client, never authoritative.

The rule of thumb: replicate **Sim state a client cannot re-derive** (the pawn's
position, its health); leave **View state** (camera pose) and **input** (the raw
command) off the wire. The Sim/View split already drew this line for you.

---

## 2. A connection is a seat; a rule pawns it

On accept, the server spawns a **`Viewer` seat entity** for the connection —
`Authority{ Server, Owner = connectionId }` and **no `SeatInput`** (the documented
remote path: a seat with no `SeatInput` is skipped by `InputMappingSystem`, because
its input comes from the wire). The seat is replicated, so the joining client
receives its own seat as a Remote-tier mirror and learns which one is *its* own
from the accept.

Turning that pawnless seat into a playing pawn is an ordinary game-mode rule —
**the same spawn logic single-player uses**, with one extra line: the pawn inherits
the seat's owner. In `hello-triangle` the host reconciles pawns against its seats
each frame (both the listen host's own Local-tier seat and each connection's
`Server`-tier remote seat), pawning any that lacks one:

```cpp
// Pawn every connection seat that lacks a pawn, threading the connection id onto the
// pawn's Authority::Owner so the authority filter and the input feed agree on who owns it.
const Entity pawn = prefab->Get()->SpawnInto(world, GetAssetManager()).Roots.front();
world.Get<Authority>(pawn).Owner = id;          // the owner-threading line
world.Get<Possesses>(seat).Pawn = pawn;
```

Two details make the pawn render on the *client*:

- **Spawn it from a prefab and associate that prefab** with the replication server
  (`ReplicationServer::SetEntityPrefab(netId, prefabId)`). A prefab-associated spawn
  rides the reliable channel as an `AssetId`, so the client **instantiates the
  prefab locally** — mesh, materials, and all — rather than receiving a bare
  component list. (An unassociated spawn carries only the replicated components, so
  its unreplicated `MeshRenderer` would not appear on the client.) The pawn prefab
  is a single entity carrying `Transform`, `Intent`, `Mover`, `Authority`, and a
  `MeshRenderer` — small and self-contained.
- **Assign the wire id before the spawn is generated.** The host assigns NetIds in
  its `Pump`, so call `AssignServerNetIds(world, host.Allocator())` right after
  spawning and read the pawn's `NetIdentity::Id` to set the prefab association ahead
  of that Pump.

On disconnect the host tears the seat down; the rule reaps the orphaned pawn
(destroy the pawn whose owner no longer has a live seat). The wire input the server
buffers for that connection then feeds the seat's `PlayerInput`, and the **unchanged**
control system re-derives `Intent` from it — exactly as for a local seat.

---

## 3. The authority filter — a Sim system acts only where it owns

A Sim system that **advances authoritative state** (moves a pawn, integrates a
prop) must act only on the entities *this peer* owns, or a client's Sim phase will
fight the snapshot stream. `HasAuthority(context, scene, entity)` is the one-line
gate, reading the entity's `Authority` tier against the tick's `NetRole`:

```cpp
scene.Each<Transform, Intent>(
    [&](Entity entity, Transform& transform, Intent& intent)
    {
        if (!HasAuthority(context, scene, entity)) { return; }
        // ... advance the entity ...
    });
```

- A **`Server`-tier** entity is simulated only by a `Server`-role peer.
- A **`Local`-tier** entity is always simulated locally (client-local view/UI).
- A **`Remote`-tier** entity — the client-side mirror of a server-owned entity — is
  never simulated; the interpolation system displays it.

**Standalone and the server are unchanged** (every `Server`-tier entity passes), so
single-player behaves identically whether or not a system consults the filter — but
a system that will ever run on a client must consult it. The builtin
`MovementSystem` and the motion systems already do; adopt the same line in your
own authoritative Sim systems. AI and server producers that write `Intent` directly
are unaffected — the filter gates the *advancing* systems, not the `Intent`
producers.

---

## 4. Launch: listen server, dedicated server, joining client

Networking is a **launch decision**, not a build. A game opts into the knobs by
setting `ApplicationInfo::Net` (a `GameNetInfo` — port, max connections, snapshot
interval, input redundancy); leaving it at `GameNetInfo{}` takes the zero-config
defaults. The knobs are inert until a launch flag activates a mode:

| Launch | Role | What runs |
|---|---|---|
| *(no flag)* | `Server` | Offline single-player — a server with no transport. Byte-identical to a game with no net code. |
| `--server` | `Server` | A **listen server**: the windowed app starts listening; it hosts and renders. |
| `--server --headless` | `Server` | A **dedicated server**: the Sim phase + net pump with no render tail. |
| `--join <host[:port]>` | `Client` | Connects, loads the accepted level (server-authoritative entities skipped), and displays the stream. |

`Application` mounts the hosts and drives the pump for you: it receives, ticks the
Sim phase (feeding each connection's buffered input into its seat first), and sends
this frame's snapshot + spawn stream. A game reaches the machinery through
`GetNetRole()`, `GetServerHost()`, and `GetClientHost()`, and wires a joined
client's local presentation through the `OnClientPossession(world, pawn)` hook —
called when the client's own seat's possessed pawn changes, so the game aims its
Local-tier camera at the replicated pawn.

Where does a joined client's **local control seat** come from? The same player
prefab single-player uses. The sample tags the world when a net flag is set and its
spawn rule then spawns the prefab with the server-authoritative (`Server`-tier)
entities skipped — so on a client the prefab's **Local-tier** presentation (a
`Viewer` + `SeatInput` + `PlayerInput` + the gameplay `InputContextStack`, plus the
follow camera) still spawns while its pawn arrives from the stream. `InputMappingSystem`
fills that seat, the world drive sends it, and the server drives the owned pawn from
it. The server pawns every pawnless seat — the listen host's own Local seat and each
connection's remote seat alike — from the shared net-pawn prefab (the owner-threaded
rule of §2), so one code path covers both.

The parity handshake rejects a mismatched client loudly: the protocol version and
the active pack's content digest must agree, so the wire only ever carries asset
ids, never assets. A content mismatch is a clean rejection, not a silent desync.

---

## 5. Verify it: the two-world integration suite

A live two-process run is the manual acceptance test, but the **authoritative,
deterministic** proof is the two-world in-process suite
([`tests/unit/net_two_world.cpp`](../../tests/unit/net_two_world.cpp)): a server
`Scene` + `ServerHost` and a client `Scene` + `ClientHost` over a
`LoopbackTransport`, stepped tick-by-tick with injected time — no socket, no ICD,
no wall clock. It consolidates the whole stack into end-to-end scenarios you can
copy for your own game:

- **Join → play → converge → leave:** the full lifecycle, with the client's
  interpolated pose asserted field-wise against the server's after a quiescent tail.
- **Input round-trip:** a scripted client `ActionState` drives the server pawn
  through the unchanged pipeline, and the client renders it delayed and bounded.
- **Abuse:** seeded loss/reorder/duplication (via `FaultInjectionTransport`) — the
  join completes, no spawn doubles or drops, and the stream converges after the loss
  burst; a hostile input stream drops rather than faulting the server.
- **Two clients:** each sees both pawns, and each drives only its own (authority +
  ownership).

Because it is deterministic by construction (fixed tick, injected time, seeded
faults), it is the regression net every later net change runs against. Model your
own game's net tests on it: stand up two scenes over a `LoopbackTransport`, step
them by hand, and assert convergence.

---

## Client-side prediction and reconciliation

The local pawn is **predicted**, not interpolated: the client promotes the pawn its
seat possesses (and the replicated subtree under it) to **`Tier::Predicted`** and runs
the real control + movement Sim systems for it each client tick, so it responds on the
tick its input is sampled rather than a round trip later. The client records each tick's
input and predicted state in a bounded history.

The server confirms which of the client's inputs its state reflects: every snapshot
header carries a **`LastConsumedInputTick`** beside the buffer-depth feedback. On each
snapshot the client compares its recorded prediction at that tick against the
authoritative record — spatial leaves (position, rotation) within a tolerance, discrete
state exactly. On a match the prediction stands. On a mismatch the client **restores** the
predicted set to the authoritative record and **replays** its recorded inputs forward
through the real Sim systems (a rollback of a handful of entities, bounded by the history
depth), then hides the visual residual behind a render offset that decays through
`Math::ExpApproach` — the simulation snaps to truth immediately; only the camera-facing
pose eases. A correction beyond a teleport-scale threshold snaps without smoothing.

### The one rule for gameplay authors: `IsReplay`

Rollback re-runs your Sim systems for ticks they already ran once. A system that only
**advances state** (movement, a rule reading and writing components) needs no awareness of
this — re-running it from the restored state is exactly the point. But a system with an
**external side effect** — spawning an entity, triggering a sound, emitting an event
outward — must **not** repeat that effect per replayed tick, because it already fired on
the tick's first live simulation. Gate the side effect on **`SystemContext::IsReplay`**:

```cpp
void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override
{
    AdvanceState(scene, delta);           // always runs — pure state
    if (!context.IsReplay)                // fire outward effects on the live tick only
    {
        SpawnPickupEffect(scene);
    }
}
```

`IsReplay` is false on every live tick and in the View phase; it is true only during a
reconciliation replay. During replay the world around the predicted set is frozen at the
present (remote entities hold their current interpolated pose, no View systems run), so a
predicted-vs-world interaction in the replay window uses present-world context — the server
remains the arbiter of what actually happened.

## Wire compression — automatic, behind the codec

The snapshot wire shrinks three ways, all behind the stable packet shapes, so the game code
above is unchanged whether or not they are on:

- **Ack-keyed field deltas** ride always. A connection's snapshots carry only the fields that
  changed since the tick it last acknowledged; a full self-describing record is reserved for
  spawns, a periodic keyframe (`GameNetInfo::KeyframeIntervalSnapshots`), and the drift-tolerant
  fallback. The delta path is loss-tolerant by construction — everything dirty since the ack is
  re-sent each snapshot until a new ack advances the baseline — so it needs no game awareness.
- **Spatial quantization** (`GameNetInfo::QuantizeSpatial`, on by default) rounds a `Transform`'s
  position to a fixed-point grid (`PositionQuantum`, 1 mm default over `PositionExtent`) and its
  rotation to a smallest-three quaternion on the wire. **The sim state stays full-float on both
  ends** — only the wire representation rounds, and the server never quantizes its own truth. The
  reconciliation epsilon (below) is kept ≥ the quantum, so the rounding never reads as a
  misprediction.
- **The packed `PlayerInput`** encodes each action's value and phase against the seat's active
  context's resolved action list, keyed by a context-stack hash both ends verify; a mismatch
  (a mid-flight context switch) falls back to the reflection form for that packet.

## Interest management — a connection hears about its neighborhood

Set `GameNetInfo::InterestRadius` (0, the default, replicates the whole world) to gate each
connection's stream by relevancy: a connection hears about the entities within the radius of its
pawn, plus every **always-relevant** entity, plus an optional `GameNetInfo::InterestPolicy` hook.

- **Always-relevant** is a type mark beside `VE_REPLICATED` — **`VE_ALWAYS_RELEVANT(Type)`**. An
  entity carrying any always-relevant component is relevant to every connection regardless of
  distance; `Session` and the seat (`Viewer`) carry it builtin, so global game state always
  reaches every client. A connection also always sees the entities it owns (its predicted pawn).
- **Entering** interest sends the entity's spawn + baseline; **leaving** sends a
  despawn carrying `DespawnReason::Visibility` (versus `Destroyed`). **A visibility despawn is
  not a death** — the client tears the entity down with no game-event side effects, and a re-entry
  re-spawns and re-baselines it. Hysteresis (a leave radius wider than the enter radius, plus a
  minimum dwell) keeps a boundary from flapping.
- The relevancy query is a headless-safe scene query, so a dedicated server filters interest with
  no graphics stack.

## Playing under adversity — `--netsim` and the tuning knobs

The launcher ships a network-simulation flag in every build (a dev/QA tool, inert unless set):

```sh
hello_triangle-launcher --join 127.0.0.1 --netsim latency=100,jitter=20,loss=5,dup=1,reorder=2
```

`latency`/`jitter` are milliseconds; `loss`/`dup`/`reorder` are percentages. It wraps whichever
transport the mode constructs (client or server), so an asymmetric setup runs it on one side. The
acceptance target is the exemplar *playing well* at 100 ms / 5 % — the local pawn responsive
(prediction), remotes smooth, corrections imperceptible in normal play.

The knobs worth tuning per game, and what each trades:

| Knob | Default | Trade |
|------|---------|-------|
| `InterestRadius` | 0 (off) | Bandwidth vs. how far a player sees; the scale lever. |
| `QuantizeSpatial` / `PositionQuantum` | on / 1 mm | Bandwidth vs. positional precision on the wire. |
| `KeyframeIntervalSnapshots` | 16 | Re-base cost vs. recovery latency after baseline loss. |
| `SnapshotIntervalTicks` | 2 (30 Hz) | Bandwidth vs. remote-interpolation freshness. |
| `ReconcileTolerances::Position` | 1 cm | Correction sensitivity; must stay ≥ `PositionQuantum`. |

## What remains future

Deliberately deferred — do not reach for these yet:

- **Lag compensation** (server-side historical rewind for authoritative hit tests).
- **Encryption / authentication.** The current scope is loopback/LAN/trusted-transport.
- **Spectators, replays, host migration**, and **editor Play as a network client** — each a
  consumer of the snapshot stream, each its own scope when earned.

## Where to go next

- **[Writing gameplay systems](writing-gameplay-systems.md)** — the fixed-tick
  model, the Sim/View split, and the authority filter in full.
- **[Multi-seat input](multi-seat-input.md)** — the `Viewer` seat, `SeatInput`, and
  `Possesses` the net layer keys on.
- The generated API reference documents every `Veng/Net/` symbol named here.

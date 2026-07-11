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

## What v1 does not do

Deliberately deferred to the next networking phase — do not reach for these yet:

- **Client-side prediction + rollback** of the local pawn (the local pawn currently
  wears its latency). This needs sim rewind/replay, its own phase.
- **Delta compression, quantization, and a bit-packed input encoding.** v1 sends
  full reflected state and the reflection-encoded `ActionState`; the optimization
  tier is later, behind the stable component and `ActionState` shapes.
- **Interest management** (per-connection relevancy). v1 replicates every
  `Authority::Server` entity to every connection.
- **Encryption / authentication.** v1 is loopback/LAN/trusted-transport scope.

The `Tier` enum was designed to extend (`Remote` now, a predicted tier reserved),
and the `ActionState`/component shapes are stable, so adopting the optimization tier
later needs no change to the game code you write against this guide.

## Where to go next

- **[Writing gameplay systems](writing-gameplay-systems.md)** — the fixed-tick
  model, the Sim/View split, and the authority filter in full.
- **[Multi-seat input](multi-seat-input.md)** — the `Viewer` seat, `SeatInput`, and
  `Possesses` the net layer keys on.
- The generated API reference documents every `Veng/Net/` symbol named here.

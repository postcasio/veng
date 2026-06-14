# planset-8 — compiled RenderGraph

**Phase goal:** move `RenderGraph` from its immediate-mode form — a fresh vector of
pass structs rebuilt every frame, every barrier re-derived per `Execute` — to a
**compiled** graph that computes pass topology, the derived barrier/layout schedule,
transient resource allocation, and one-time validation **once**, then **replays**
that schedule per frame with only the per-pass record callbacks running. The
authoring surface (`AddPass`/`.Color`/`.Sample`/…) is preserved; what changes is the
**`Compile()` → replay** lifecycle replacing rebuild-every-frame, and a resource
model that splits **graph-owned transients** (logical handles the graph allocates and
resolves per frame) from **imported** concrete resources (the swapchain image, an
app-owned target).

This is **future area 9** ([compiled-rendergraph.md](../future/compiled-rendergraph.md)).
It is the enabling prerequisite for the **scene renderer** (area 8): the
`PassContext::Resolved(ResourceId)` channel and the `Compile`/`Resize`/`Configure`
recompile seams the scene renderer's public shape assumes are exactly what this
planset builds. It also lands the resource-model split that lets a pass be authored
as a **reusable unit** — it names logical handles, not concrete app resources. It
builds directly on the shipped `RenderGraph` and changes no other subsystem.

## The decisions that shape this planset

1. **The resource model splits into transients and imports — this is the real API
   shift.** Today every `Access` names a concrete app-owned `Ref<ImageView>`. The
   compiled graph cannot bake a schedule around concrete views whose backing the
   graph may pool. So resources become a typed, vk-free `ResourceId`:
   - **Transients** are graph-owned: declared with `CreateTransient({.Format, .Extent,
     .Usage})` as a logical handle with no backing. The graph allocates the concrete
     `Image`/`ImageView` at compile and **resolves** it per frame; a callback may
     **not** capture the concrete view.
   - **Imports** are external concrete resources registered with `Import(...)` —
     never allocated, never aliased; they pin the graph's outputs. An import's
     concrete view is **late-bound per frame** as a binding passed to `Execute`
     (not baked, and not lingering state on the graph). The acquired swapchain view
     differs every frame — the swapchain holds N distinct images, each its own
     tracked state, and `AcquireNextImage` selects a different one per frame — so an
     import is resolved to *this frame's* concrete image, which is also what keeps
     the tracked-state barrier source (its real `PresentSrc`) correct.

2. **Callbacks receive a typed `PassContext`, not a bare `CommandBuffer&`.** The pass
   record callback changes from `function<void(CommandBuffer&)>` to
   `function<void(PassContext&)>`, where `PassContext` exposes `Cmd()` and
   `Resolved(ResourceId) → ImageView&` (the concrete view for a declared transient,
   this frame). This is the record-time channel a compiled graph **requires** —
   aliased transients have no fixed view to capture, so the graph must hand the
   concrete view to the callback at record time. It is a **concrete typed struct,
   not a stringly-typed `void*` blackboard** — that idiom clashes with veng's typed
   vocabulary identity. (The scene renderer later widens `PassContext` with a
   `View()` accessor for its per-frame `SceneView`; that type does not exist yet and
   is area 8/area 7, not this planset.)

3. **`Compile()` caches structure, not commands.** Compilation splits the work in
   two: **compile** (once, on a structural change) derives the ordered
   barrier/transition schedule, transient allocation, the per-graphics-pass
   `RenderingInfo` skeleton, and runs one-time validation; **record** (every frame)
   runs each pass's callback — *which objects you draw, camera matrices,
   push-constant indices* cannot be baked. The cache is topology; draw recording
   stays per-frame.

4. **Recompile is the consumer re-`Compile()`ing on a structural change.** With an
   explicit `CompiledGraph` (decision 5), there is **no internal dirty flag or
   structural hash** to maintain: the consumer knows when it changed topology
   (passes added/removed) or a transient's extent/format, and re-builds + re-compiles
   then. The future scene renderer keys this off `Configure` (topology) and `Resize`
   (sizing) — building those seams now is what buys that migration cheaply.
   **Data-driven emptiness is not a recompile:** "nothing to draw this frame" keeps
   the pass compiled-in and records **zero draws**; per-frame data never recompiles.

5. **`RenderGraph` is a builder; `Compile()` returns a `CompiledGraph` that replays.**
   An explicit two-type split, not internal compiled state on a persistent
   `RenderGraph`: author passes on a `RenderGraph`, call `Compile()` once to get a
   `CompiledGraph`, and `CompiledGraph::Execute(cmd, imports)` replays it per frame.
   No immediate-mode `Execute` survives on `RenderGraph` after this planset — one
   compiled path, and the explicit type makes "compile once, replay many" impossible
   to misuse. (Resolves the doc's "keep an immediate-mode path?" open question, and
   matches the doc's own `CompiledGraph g = builder.Compile()` sketch.)

6. **Barriers-and-replay first; transient aliasing second, behind a pure rule.** The
   schedule cache and the resource-model split are the load-bearing change; transient
   **aliasing** (sharing backing memory between non-overlapping transients) is a
   memory optimization layered on top. Plans 01–02 give each transient its own
   allocation; plan 03 adds aliasing once the lifetime analysis is trusted — and
   extracts that analysis as a **pure, device-free rule** unit-tested without a GPU,
   exactly as `DecideBarrier`/`ScopeFor` already are. This is the recommended split.

7. **`CompiledGraph` hides its `vk::` schedule behind the Native idiom.** The baked
   schedule carries `Backend::SubresourceState` (`vk::` layout/stage/access), so it
   cannot live in a public header. `CompiledGraph` forward-declares a `struct Native;`
   and holds it as `Unique<Native>` (defined in the `.cpp`, with the schedule + the
   graph-allocated transient images) — the same public/backend split every resource
   uses. `RenderGraph::Compile()` returns a `Unique<CompiledGraph>` (single owner, per
   `docs/ownership.md`). `RenderGraph`, `CompiledGraph`, `ResourceId`, `TransientDesc`,
   and `PassContext` keep their public headers free of backend includes so
   `include_hygiene` stays green.

8. **Single-copy transients; no per-frame ring-buffering.** A transient is allocated
   single-copy and reused every frame. veng runs **two frames in flight**
   (`MaxFramesInFlight = 2`) and `AcquireNextFrame` waits only the fence from two
   frames ago — there is **no full idle**, so single-copy is not safe-by-stall. It is
   safe-by-construction instead: every frame submits to the **one graphics queue in
   submission order**, and a persistent image's tracked state survives across frames,
   so when frame N+1 writes a transient `DecideBarrier` emits a **write-after-read
   barrier** against frame N's last use — the GPU serializes the reuse. This is
   exactly why the sample's single-copy app-owned scene/depth images are correct and
   validation-clean today. Ring-buffering a transient (or the handed-out output) per
   frame-in-flight only **recovers frame overlap** (frame N+1 needn't wait on frame
   N's read) — a throughput optimization, not a correctness fix. It is the output
   hazard the scene renderer addresses at its `GetOutput()` boundary — out of scope
   here.

9. **Single-queue, linear, no culling — unchanged.** The graph stays linear (no
   reordering) and single-queue; dead-pass culling and multi-queue/async-compute
   scheduling are noted seams, not built. The linear, fixed order is exactly what
   makes transient live-range analysis (a pass-index interval per transient) almost
   free.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| `ResourceId` + `CreateTransient` (graph-owned) / `Import` (late-bound external) resource model | A `SceneRenderer` owning a compiled graph (area 8) |
| `PassContext` (`Cmd()` + `Resolved(ResourceId)`) replacing the bare `CommandBuffer&` callback | `PassContext::View()` / the `SceneView` per-frame input (area 8/7) |
| `RenderGraph` builder → `CompiledGraph` replay lifecycle; consumer-driven re-`Compile()` on structural change; holding the sample's compiled graphs across frames | Parallel pass recording into secondary command buffers (area 2 seam) |
| One-time compile validation (read-before-write, format/usage, attachment-extent agreement) | Multi-queue / async-compute scheduling across the graph |
| Transient aliasing via a pure, unit-tested live-range/assignment rule | Dead-pass culling (note the seam; imports already pin outputs) |
| Migrating `examples/hello-triangle` (depth → transient, scene/swapchain → imports) | Per-frame ring-buffering of transients / outputs (area 8 `GetOutput`) |
| GPU test proving compile/replay + aliasing; unit test for the pure allocation rule | Retaining a separate immediate-mode execution path |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Logical resources + `PassContext`](01-logical-resources.md) | Introduce the vk-free `ResourceId`, `TransientDesc`, `CreateTransient`/`Import` resource model and the `PassContext` (`Cmd()` + `Resolved(ResourceId)`) callback, replacing the concrete-`Ref<ImageView>` accesses and the bare `CommandBuffer&` lambda. Imports are late-bound as bindings passed to `Execute(cmd, imports)`. Execute stays immediate-mode (resolve + derive + run each frame) so the API shift is verifiable in isolation. Migrate the sample: depth buffer → a graph transient, scene image + swapchain → imports. | proposed |
| 02 | [`Compile()` → replay lifecycle](02-compile-replay.md) | Split `RenderGraph` (builder) from an explicit `CompiledGraph` (`Unique<CompiledGraph>` from `RenderGraph::Compile()`; its `vk::` schedule hidden behind the Native idiom): compile once into an ordered transition schedule + per-graphics-pass `RenderingInfo` skeleton + one-time validation; `CompiledGraph::Execute(cmd, imports)` replays (resolve resources, emit transitions through the tracked-state `TransitionImage`, drive rendering, run callbacks). Drop `RenderGraph::Execute`. Hold the sample's two compiled graphs across frames, re-compiling only on resize; deliver per-frame data through `this` + late-bound imports, not closure-captured values. | proposed |
| 03 | [Transient aliasing + pure rule](03-transient-aliasing.md) | Extract a pure, device-free live-range/slot-assignment rule into a backend header (mirroring `BarrierDecision.h`); unit-test it (`tests/unit/`) for interval overlap and reuse. Wire it into compile so non-overlapping transients share backing memory. Prove end-to-end with a GPU test using ≥2 non-overlapping transients; the sample's single transient is unaffected. | proposed |
| 04 | [Docs + roadmap re-cut](04-docs-roadmap.md) | Update `plans/README.md` (planset-8 line), `CLAUDE.md` (the RenderGraph section: compiled lifecycle, transient/import model, `PassContext`), and the future docs — mark area 9 delivered in `future/README.md`, trim `compiled-rendergraph.md` to its enduring seams, and note in `scene-renderer.md` that its compiled prerequisite has landed. | proposed |

## Dependency & order

01 → 02 → 03 → 04, strictly. 01 lands the resource model + `PassContext` while
execution stays immediate-mode, so the API change is isolated and sample-verified on
its own. 02 flips execution to compile/replay on that model. 03 is a pure-rule +
aliasing optimization on the compiled allocator from 02. 04 is roadmap-only. No
edges to any other planset's in-flight work; this builds only on the shipped
`RenderGraph`.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the
same pass (plans 01 and 02 touch it) → verify (clean build, `ctest` green, smoke
binary writes a correct-sized PPM, 1280×720 RGB ≈ 2,764,816 bytes) → update this
table → one commit per plan, `Plan NN: <summary>` with a `Co-Authored-By` trailer
(`planset-8:` for roadmap-only edits).

- **The public headers must stay backend-free.** `RenderGraph`/`CompiledGraph`/
  `ResourceId`/`TransientDesc`/`PassContext` are vk-free; the compiled schedule's
  `vk::` state lives behind `CompiledGraph`'s forward-declared `Unique<Native>`. The
  **`include_hygiene` test** guards this — every plan keeps it green.
- **Plans 01–03 create/draw GPU resources** (the sample plus the new graph-allocated
  transients) and must pass the `VE_DEBUG` validation gate
  (`ctest --test-dir build-debug -L validation`). Transient allocation creates and
  transitions new images each compile; the path reuses `Image::Create` and the
  tracked-state `TransitionImage`, both already validation-clean — **no plan may
  widen the allowlist** (it is empty).
- **The smoke PPM is non-deterministic** (wall-clock rotation) — never golden-compare
  it; verify size + exit 0, per `CLAUDE.md`.
- **Plan 03's live-range/allocation rule is pure CPU** — its unit tests run under
  `ctest -L unit`, driver-free, alongside `barrier_decision`.
- **Delegation.** Plan 03's pure rule + its unit tests are good `model: sonnet`
  work once the allocator interface is fixed; keep the resource-model contract (01),
  the compile/replay lifecycle (02), and the sample migrations on the main thread.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

`RenderGraph` compiles its topology, barrier schedule, and transient allocation once
and replays per frame; passes are authored against logical resource handles and
receive their concrete per-frame views through `PassContext`; the recompile seams the
scene renderer needs exist. Update [plans/README.md](../README.md) with the planset-8
line, mark **area 9 done** in [future/README.md](../future/README.md), and re-cut the
future ordering: area 8 (scene renderer) loses its one enabling prerequisite and
becomes the next-most-ready rendering area, while the editor (area 6) and scene/entity
model (area 7) are unchanged.

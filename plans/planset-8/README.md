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
     concrete view is **late-bound per frame** (the acquired swapchain view differs
     every frame), supplied at `Execute`, not baked.

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

4. **A compiled graph invalidates on structure, never on per-frame data.** Recompile
   is triggered by passes added/removed (topology) or a transient's extent/format
   changing (allocation) — an **explicit dirty flag**, the recommended start over a
   structural hash. **Data-driven emptiness is not a recompile:** "nothing to draw
   this frame" keeps the pass compiled-in and records **zero draws**. For the future
   scene-renderer consumer those two triggers are precisely `Configure` (topology)
   and `Resize` (sizing); building the seam now is what buys that migration cheaply.

5. **One compiled path; no retained immediate-mode execution.** The per-frame-rebuild
   path is not kept as a second supported mode — one code path, less to keep green.
   The authoring surface is unchanged and recompile-on-structural-change keeps
   re-authoring cheap, so nothing is lost. (Resolves the doc's "keep an immediate-mode
   path?" open question.)

6. **Barriers-and-replay first; transient aliasing second, behind a pure rule.** The
   schedule cache and the resource-model split are the load-bearing change; transient
   **aliasing** (sharing backing memory between non-overlapping transients) is a
   memory optimization layered on top. Plans 01–02 give each transient its own
   allocation; plan 03 adds aliasing once the lifetime analysis is trusted — and
   extracts that analysis as a **pure, device-free rule** unit-tested without a GPU,
   exactly as `DecideBarrier`/`ScopeFor` already are. This is the recommended split.

7. **The compiled state hides its `vk::` types behind the Native idiom.** The baked
   schedule carries `Backend::SubresourceState` (`vk::` layout/stage/access), so it
   cannot live in the public `RenderGraph.h`. `RenderGraph` forward-declares a
   `struct Compiled;` and holds it as `Unique<Compiled>` — the same public/backend
   split every resource uses — keeping `RenderGraph.h` free of backend includes so
   `include_hygiene` stays green. `ResourceId`, `TransientDesc`, and `PassContext`
   are vk-free public types.

8. **Single-in-flight transients; no per-frame ring-buffering.** A transient is
   allocated single-copy and reused every frame, matching the sample's existing
   app-owned scene/depth images (single-copy, persistent, validation-clean today).
   Ring-buffering a transient (or the handed-out output) per frame-in-flight is the
   **output**-image hazard the scene renderer addresses at its `GetOutput()` boundary
   — explicitly out of scope here.

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
| `Compile()` → replay lifecycle; explicit dirty-flag invalidation; persisting the sample's graphs across frames | Parallel pass recording into secondary command buffers (area 2 seam) |
| One-time compile validation (read-before-write, format/usage, attachment-extent agreement) | Multi-queue / async-compute scheduling across the graph |
| Transient aliasing via a pure, unit-tested live-range/assignment rule | Dead-pass culling (note the seam; imports already pin outputs) |
| Migrating `examples/hello-triangle` (depth → transient, scene/swapchain → imports) | Per-frame ring-buffering of transients / outputs (area 8 `GetOutput`) |
| GPU test proving compile/replay + aliasing; unit test for the pure allocation rule | Retaining a separate immediate-mode execution path |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Logical resources + `PassContext`](01-logical-resources.md) | Introduce the vk-free `ResourceId`, `TransientDesc`, `CreateTransient`/`Import` (late-bound) resource model and the `PassContext` (`Cmd()` + `Resolved(ResourceId)`) callback, replacing the concrete-`Ref<ImageView>` accesses and the bare `CommandBuffer&` lambda. Execute stays immediate-mode (resolve + derive + run each frame) so the API shift is verifiable in isolation. Migrate the sample: depth buffer → a graph transient, scene image + swapchain → imports. | proposed |
| 02 | [`Compile()` → replay lifecycle](02-compile-replay.md) | Split the builder from a `Compiled` state (held `Unique<Compiled>`, Native idiom): compile once into an ordered transition schedule + per-graphics-pass `RenderingInfo` skeleton + one-time validation; `Execute` replays (resolve resources, emit transitions through the tracked-state `TransitionImage`, drive rendering, run callbacks). Add the explicit dirty-flag recompile seam. Drop the immediate-mode path. Persist the sample's two graphs across frames, delivering per-frame data through `this`/late-bound imports, not closure-captured values. | proposed |
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

- **`RenderGraph.h` must stay backend-free.** `ResourceId`/`TransientDesc`/
  `PassContext` are vk-free; the compiled schedule's `vk::` state lives behind the
  forward-declared `Unique<Compiled>`. The **`include_hygiene` test** guards this —
  every plan keeps it green.
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

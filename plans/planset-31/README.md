# planset-31 — viewports: a view owns its rectangle, the engine drives the list

**Phase goal:** make *"a renderable view into a world"* a first-class, ownable, listable thing —
a **`Viewport`** that **owns its region of the window** and renders into its own texture — and have
the engine drive a **central list** of them each frame. Before this, every consumer that wanted a
rendered scene hand-wired the same trio (a `SceneRenderer`, a `Sampler`, an ImGui texture, the
`Execute` + `PrepareForAccess(Sample)` barrier) plus, for the window, a `SwapChainCompositePass` +
a `CompiledGraph` rebuilt on resize. The shipped sample built it once in `main.cpp`; the editor
built it again, per panel, in `SceneViewportPanel`. Two sites, the same ceremony, with **placement
treated as external** — which is exactly what blocks splitscreen.

This planset inverts that. A `Viewport` carries a **`ViewportRegion`** (its rectangle in the
window) and a **role** — `Presented` (the engine compositor places its texture into its region) or
`Offscreen` (a consumer samples its texture: an ImGui panel, or a material). **Render-to-texture is
the floor either way** — every viewport renders into its own target at its region's resolution, and
the deferred pipeline never scatters into a swapchain sub-rect; the compositor *places* the finished
textures. So **splitscreen falls out** as "register N `Presented` viewports with quadrant regions,"
the editor is "N `Offscreen` viewports whose regions track their ImGui panels," and a monitor/mirror
is "an `Offscreen` viewport a material samples." Owning the region also gives every viewport a
**window↔view mapping** (`WindowToViewport`, `ScreenToWorldRay`) — the seam editor picking uses now
and multi-seat pointer routing will use later. It takes up the viewport slice of
[future area 8 (scene-renderer architecture)](../future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries),
the render-driving slice of [future area 6 (editor application)](../future/README.md#6-editor-application),
and leaves the pointer-routing seam [future area 4 (event & input)](../future/README.md#4-event--input-systems) will consume.

## The shape

```
                    ┌─ Viewport (role=Presented) ──┐
ViewState ──set────►│  region + SceneRenderer (RTT) │─ GetOutput ─► GatherPass ─► Composite ─► swapchain
SetRegion ─────────►└───────────────────────────────┘   (each Presented vp     (ImGui over,  ▲
                                                          into its region)       encode once)
 (window rect)      ┌─ Viewport (role=Offscreen) ──┐                ImGui overlay ────────────┘
                    │  region + SceneRenderer (RTT) │─ GetOutput ──────► ImGui::Image (a panel, at its region)
                    └───────────────────────────────┘─ GetOutputHandle ─► Material (RTT sampler field)

input mapping:  WindowToViewport(pt) / ScreenToWorldRay(pt)  ─►  editor picking now · pointer→seat routing later
Application drive-list: [vp0, vp1, …]   Frame: render each (registration order) ─► OnRender/ImGui ─► gather + composite
```

- **`Viewport` = a region + a renderer + a role.** It owns a `SceneRenderer`, carries a
  `ViewportRegion` (its window rectangle, whose size drives the render extent), takes a per-frame
  `ViewState` (scene + camera + tone/bloom knobs, *pushed* by the owner), and on `Render(cmd)` does
  the `Execute` + `PrepareForAccess(Sample)` itself. Its product is a sampleable
  `Ref<ImageView>` (`GetOutput`) and a bindless `TextureHandle` (`GetOutputHandle`).
- **The role gates engine compositing, nothing else.** `Presented`: the engine compositor blits its
  texture into its region (fullscreen game = whole window; splitscreen = a quadrant). `Offscreen`:
  a consumer samples its texture (ImGui panel / material). The region is *universal* state — an
  `Offscreen` editor panel still owns a region (for resize + picking); the role only decides whether
  the **engine** places it.
- **Central driving, local ownership, RAII cleanup.** The engine drive-list holds **non-owning raw
  `Viewport*`**, not the viewports. The owner constructs via the house factory and registers —
  `m_vp = Viewport::Create(info); host.RegisterViewport(*m_vp);` — keeping the owning `Unique`;
  `~Viewport` removes the engine's pointer, so registration is explicit but dropping the `Unique` is
  the whole of cleanup (no explicit unregister). "0..N renderers including zero" is the list length;
  lifetime stays with whoever created the view.
- **A gather pass assembles; the composite encodes.** A new `GatherPass` scissor-blits each
  `Presented` viewport's texture into its region on one full-window linear-HDR assembly target;
  `SwapChainCompositePass` then consumes that single target *unchanged* (ImGui over, display-transfer
  encode once). One `Presented` viewport covering the window is the old game case; zero is the editor
  (a cleared target); N is splitscreen — the same gather + composite tail for all three, and the
  composite's HDR/color-space encode is left untouched.
- **Owning the region yields a window↔view mapping.** `WindowToViewport` (hit-test + remap) and
  `ScreenToWorldRay` (using the region + the camera from the last `ViewState`) are gameplay-agnostic
  primitives — editor entity-picking consumes them immediately, and multi-seat *pointer* routing
  (which quadrant did a click land in?) consumes them later.

## The spine — four principles

1. **A view owns its rectangle.** Placement is intrinsic, not supplied by a downstream consumer.
   This is what makes splitscreen a list of viewports with quadrant regions rather than a bespoke
   compositing path — and it is what makes the name `Viewport` *correct* (a viewport is, classically,
   a rect of the render target) rather than a borrowed screen-region object.

2. **Render-to-texture is the floor; a gather pass assembles.** Every viewport renders into its own
   texture at its region's resolution (per-view resolution is what splitscreen wants anyway); the
   engine never scatters the deferred pipeline into a swapchain sub-rect. "Presented to the window"
   is then a gather pass *placing* finished textures into one assembly target the composite consumes,
   which is why the game's main view, a splitscreen quadrant, an editor panel, and a monitor material
   are one mechanism.

3. **Push the per-frame source, derive nothing.** The owner sets `ViewState` (scene + camera +
   knobs) each frame; the viewport never reaches into the scene for a camera. This matches both
   existing sites and preserves the editor's existing one-frame camera latency.

4. **The viewport is gameplay-agnostic; the seam is shared.** The viewport exposes region +
   window↔view mapping and knows nothing of `Viewer`/`PlayerInput`. Editor picking and (future)
   multi-seat pointer routing both consume that seam; per-device gamepad routing is independent of
   viewports entirely (routed by device id, an area-4 concern). The viewport leaves the seam without
   importing gameplay.

## The frame this produces

| Phase | Step | Who |
|---|---|---|
| Update | tick sim, compute cameras, `SetRegion`/`SetViewState` per viewport | app / editor panels |
| **Render** | **render every registered viewport (registration order): `Execute` + barrier** | **engine (the drive-list)** |
| Render | `OnRender` builds the ImGui frame — `UI::Image(vp.GetOutput())` per `Offscreen` panel viewport | app / editor |
| Render | `ImGuiLayer::Render` records the overlay | engine (when ImGui on) |
| Composite | gather each `Presented` viewport into its region on the assembly target, then composite (ImGui over, encode once) | engine |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | The `Viewport` type | `Veng::Renderer::Viewport` over `SceneRenderer`: `ViewportRegion`/`SetRegion`, `Role { Presented, Offscreen }`, `ViewState`/`SetViewState`, `Render` (owns `Execute` + `Sample` barrier), `GetOutput`/`GetOutputHandle`/`GetRegion`/`GetRole`. Purely additive. | done |
| 01 | Gather pass + assembled composite | A new `GatherPass` scissor-blits each `Presented` viewport into its region on one full-window linear-HDR assembly target; `SwapChainCompositePass` consumes that single target *unchanged* (ImGui over, encode once). One vp = the game; zero = the editor; N = splitscreen. Sample still renders one full-window viewport — golden unmoved. | done |
| 02 | Application drive-list + managed primary + frame loop | `Viewport::Create` (house factory) + `RegisterViewport(Viewport&)` (stores a non-owning `Viewport*`; `~Viewport` self-unregisters), an optional engine-owned managed primary `Presented` viewport (region = whole window, tracks resize), the managed gather+composite driven from the `Presented` set (incl. `SetSwapChainTarget` re-target), the engine render phase + narrowed `OnRender`. **Migrate hello-triangle** — delete its renderer/composite/sampler/texture boilerplate. Golden unmoved. | done |
| 03 | Editor on the central list | Panels hold a `Unique<Viewport>` (`Viewport::Create`) and register it (`Offscreen`), feed `SetRegion` from their ImGui content rect, push `ViewState`, and let the engine render; `EditorHost` deletes its hand-rolled blit/present path and runs the engine tail. No `Presented` viewports ⇒ a cleared assembly target, ImGui only. | proposed |
| 04 | Splitscreen — tests only | gpu test: N `Presented` viewports with quadrant regions driven through the gather + composite into an **offscreen** target; assert both the gather's received placements (regions) and the assembled quadrant pixels. No sample feature. | proposed |
| 05 | RTT-to-material + render ordering | An `Offscreen` viewport's `GetOutputHandle` bound into a material via `Material::SetTextureHandle`, with the registration-order render guarantee (producer before consumer, same frame, single queue, no ring) documented and covered by a viewport-feeds-viewport gpu test. | proposed |
| 06 | Viewport input mapping | `WindowToViewport(pt) → optional<vec2>` (hit-test + remap) and `ScreenToWorldRay(pt) → optional<Ray>` (region + last `ViewState` camera); a `Ray` primitive in `Veng/Math/`. Unit-tested (pure math). The gameplay-agnostic seam editor picking uses now and multi-seat pointer routing uses later. | proposed |
| 07 | Docs + roadmap | `engine/CLAUDE.md` (a `Viewport` section), `editor/CLAUDE.md` (panels own viewports, engine drives), `future/README.md` areas 4/6/8, this record. Full `ctest` + `smoke_golden` + `validation_gate` green. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

**00** (the type) is foundational. **01** (gather pass) is independent of 00 and can land in
parallel. **02** depends on 00 + 01. **03** (editor), **04** (splitscreen tests), **05**
(material-RTT), and **06** (input mapping) all depend on 02 and are independent of each other.
**07** is last. Worktree-isolated parallel dispatch should branch 02 from the 00+01 integration
commit, then 03–06 from 02 — see [[project_megaexec_worktree_base]].

**Shared-file caveat:** `Viewport.h` is edited by 05 (doc the RTT contract on `GetOutputHandle`) and
06 (the mapping API), and 03 *consumes* the region but does **not** call the Plan 06 API. So 03/05/06
are logically independent but touch the same header — merge them in number order (05 then 06, or
serialize 06 before any later editor picking pass) rather than expecting a conflict-free three-way
merge. 01's and 02's `main.cpp` composite edits also overlap, which the 02-from-00+01 base handles.

## The decisions this planset settles

- **Central list, RAII ownership.** The engine drives every viewport everywhere; `EditorPanel`'s
  scene-execute responsibility disappears; "0..N viewports" is one `vector` of raw pointers.
  Ownership stays **local** (the caller holds the `Unique<Viewport>` from `Viewport::Create` and
  registers it; `~Viewport` self-unregisters); only the *driving* is central. The cost is the larger
  editor refactor in Plan 03.
- **Placement is intrinsic.** The viewport owns its region; the gather pass places `Presented`
  viewports into one assembly target; an `Offscreen` viewport's region tracks its consumer (the ImGui
  panel) and drives its resize + picking. This is what unlocks splitscreen and legitimizes the
  `Viewport` name.

## What remains (future)

**Multi-seat input routing** — fanning per-device input into the right `PlayerInput` per `Viewer`,
with *pointer* events routed by viewport-region hit-test (consuming Plan 06's mapping) and *gamepad*
events by device id (independent of viewports). The area-4 increment after this, now unblocked by
the region seam. **Declared inter-viewport dependencies** (a topological render order over
registration order) once an RTT graph gets deep. **Output ringing** for an async/off-queue consumer
(the single-copy contract holds for same-frame, same-queue RTT, so this is only needed off the
graphics queue). **A runtime monitor/mirror sample feature** (a `.vmat` sampling an `Offscreen`
viewport) is the obvious demo on Plan 05, deferred to keep this planset asset-free.
